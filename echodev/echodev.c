/*-
 * Copyright (c) 2024 John Baldwin <jhb@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/sx.h>
#include <sys/uio.h>

#include "echodev.h"

struct echodev_softc {
	struct cdev *dev;
	char *buf;
	size_t len;
	size_t valid;
	struct sx lock;
};

static MALLOC_DEFINE(M_ECHODEV, "echodev", "Demo echo character device");

static d_read_t echo_read;
static d_write_t echo_write;
static d_ioctl_t echo_ioctl;

static struct cdevsw echo_cdevsw = {
	.d_version =	D_VERSION,
	.d_read =	echo_read,
	.d_write =	echo_write,
	.d_ioctl =	echo_ioctl,
	.d_name =	"echo"
};

static int
echo_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct echodev_softc *sc = dev->si_drv1;
	size_t todo;
	int error;

	if (uio->uio_resid == 0)
		return (0);

	sx_xlock(&sc->lock);

	/* Wait for bytes to read. */
	while (sc->valid == 0) {
		error = sx_sleep(sc, &sc->lock, PCATCH, "echord", 0);
		if (error != 0) {
			sx_xunlock(&sc->lock);
			return (error);
		}
	}

	todo = MIN(uio->uio_resid, sc->valid);
	error = uiomove(sc->buf, todo, uio);
	if (error == 0) {
		/* Wakeup any waiting writers. */
		if (sc->valid == sc->len)
			wakeup(sc);

		sc->valid -= todo;
		memmove(sc->buf, sc->buf + todo, sc->valid);
	}
	sx_xunlock(&sc->lock);
	return (error);
}

static int
echo_write(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct echodev_softc *sc = dev->si_drv1;
	size_t todo;
	int error;

	if (uio->uio_resid == 0)
		return (0);

	sx_xlock(&sc->lock);
	while (uio->uio_resid != 0) {
		/* Wait for space to write. */
		while (sc->valid == sc->len) {
			error = sx_sleep(sc, &sc->lock, PCATCH, "echowr", 0);
			if (error != 0) {
				sx_xunlock(&sc->lock);
				return (error);
			}
		}

		todo = MIN(uio->uio_resid, sc->len - sc->valid);
		error = uiomove(sc->buf + sc->valid, todo, uio);
		if (error == 0) {
			/* Wakeup any waiting readers. */
			if (sc->valid == 0)
				wakeup(sc);

			sc->valid += todo;
		}
	}
	sx_xunlock(&sc->lock);
	return (error);
}

static int
echo_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int fflag,
    struct thread *td)
{
	struct echodev_softc *sc = dev->si_drv1;
	int error;

	switch (cmd) {
	case ECHODEV_GBUFSIZE:
		sx_slock(&sc->lock);
		*(size_t *)data = sc->len;
		sx_sunlock(&sc->lock);
		error = 0;
		break;
	case ECHODEV_SBUFSIZE:
	{
		size_t new_len;

		if ((fflag & FWRITE) == 0) {
			error = EPERM;
			break;
		}

		error = 0;
		new_len = *(size_t *)data;
		sx_xlock(&sc->lock);
		if (new_len == sc->len) {
			/* Nothing to do. */
		} else if (new_len < sc->len) {
			if (new_len < sc->valid)
				error = EBUSY;
			else
				sc->len = new_len;
		} else {
			/* Wakeup any waiting writers. */
			if (sc->valid == sc->len)
				wakeup(sc);

			sc->buf = reallocf(sc->buf, new_len, M_ECHODEV,
			    M_WAITOK | M_ZERO);
			sc->len = new_len;
		}
		sx_xunlock(&sc->lock);
		break;
	}
	case ECHODEV_CLEAR:
		if ((fflag & FWRITE) == 0) {
			error = EPERM;
			break;
		}

		sx_xlock(&sc->lock);

		/* Wakeup any waiting writers. */
		if (sc->valid == sc->len)
			wakeup(sc);

		sc->valid = 0;
		sx_xunlock(&sc->lock);
		error = 0;
		break;
	default:
		error = ENOTTY;
		break;
	}
	return (error);
}

static int
echodev_create(struct echodev_softc **scp, size_t len)
{
	struct make_dev_args args;
	struct echodev_softc *sc;
	int error;

	sc = malloc(sizeof(*sc), M_ECHODEV, M_WAITOK | M_ZERO);
	sx_init(&sc->lock, "echo");
	sc->buf = malloc(len, M_ECHODEV, M_WAITOK | M_ZERO);
	sc->len = len;
	make_dev_args_init(&args);
	args.mda_flags = MAKEDEV_WAITOK | MAKEDEV_CHECKNAME;
	args.mda_devsw = &echo_cdevsw;
	args.mda_uid = UID_ROOT;
	args.mda_gid = GID_WHEEL;
	args.mda_mode = 0600;
	args.mda_si_drv1 = sc;
	error = make_dev_s(&args, &sc->dev, "echo");
	if (error != 0) {
		free(sc->buf, M_ECHODEV);
		sx_destroy(&sc->lock);
		free(sc, M_ECHODEV);
		return (error);
	}
	*scp = sc;
	return (0);
}

static void
echodev_destroy(struct echodev_softc *sc)
{
	if (sc->dev != NULL)
		destroy_dev(sc->dev);
	free(sc->buf, M_ECHODEV);
	sx_destroy(&sc->lock);
	free(sc, M_ECHODEV);
}

static int
echodev_modevent(module_t mod, int type, void *data)
{
	static struct echodev_softc *echo_softc;

	switch (type) {
	case MOD_LOAD:
		return (echodev_create(&echo_softc, 64));
	case MOD_UNLOAD:
		if (echo_softc != NULL)
			echodev_destroy(echo_softc);
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

DEV_MODULE(echodev, echodev_modevent, NULL);

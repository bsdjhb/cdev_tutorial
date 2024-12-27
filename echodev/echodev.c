/*-
 * Copyright (c) 2024 John Baldwin <jhb@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/filio.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/poll.h>
#include <sys/selinfo.h>
#include <sys/sx.h>
#include <sys/uio.h>

#include "echodev.h"

struct echodev_softc {
	struct cdev *dev;
	char *buf;
	size_t len;
	size_t valid;
	struct sx lock;
	struct selinfo rsel;
	struct selinfo wsel;
	u_int writers;
	bool dying;
};

static MALLOC_DEFINE(M_ECHODEV, "echodev", "Demo echo character device");

static d_open_t echo_open;
static d_close_t echo_close;
static d_read_t echo_read;
static d_write_t echo_write;
static d_ioctl_t echo_ioctl;
static d_poll_t echo_poll;
static d_kqfilter_t echo_kqfilter;
static void	echo_kqread_detach(struct knote *);
static int	echo_kqread_event(struct knote *, long);
static void	echo_kqwrite_detach(struct knote *);
static int	echo_kqwrite_event(struct knote *, long);

static struct filterops echo_read_filterops = {
	.f_isfd =	1,
	.f_detach =	echo_kqread_detach,
	.f_event =	echo_kqread_event
};

static struct filterops echo_write_filterops = {
	.f_isfd =	1,
	.f_detach =	echo_kqwrite_detach,
	.f_event =	echo_kqwrite_event
};

static struct cdevsw echo_cdevsw = {
	.d_version =	D_VERSION,
	.d_open =	echo_open,
	.d_close =	echo_close,
	.d_read =	echo_read,
	.d_write =	echo_write,
	.d_ioctl =	echo_ioctl,
	.d_poll =	echo_poll,
	.d_kqfilter =	echo_kqfilter,
	.d_flags =	D_TRACKCLOSE,
	.d_name =	"echo"
};

static int
echo_open(struct cdev *dev, int fflag, int devtype, struct thread *td)
{
	struct echodev_softc *sc = dev->si_drv1;

	if ((fflag & FWRITE) != 0) {
		/* Increase the number of writers. */
		sx_xlock(&sc->lock);
		if (sc->writers == UINT_MAX) {
			sx_xunlock(&sc->lock);
			return (EBUSY);
		}
		sc->writers++;
		sx_xunlock(&sc->lock);
	}
	return (0);
}

static int
echo_close(struct cdev *dev, int fflag, int devtype, struct thread *td)
{
	struct echodev_softc *sc = dev->si_drv1;

	if ((fflag & FWRITE) != 0) {
		sx_xlock(&sc->lock);
		sc->writers--;
		if (sc->writers == 0) {
			/* Wakeup any waiting readers. */
			wakeup(sc);
			selwakeup(&sc->rsel);
			KNOTE_LOCKED(&sc->rsel.si_note, 0);
		}
		sx_xunlock(&sc->lock);
	}
	return (0);
}

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
	while (sc->valid == 0 && sc->writers != 0) {
		if (sc->dying)
			error = ENXIO;
		else if (ioflag & O_NONBLOCK)
			error = EWOULDBLOCK;
		else
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
		selwakeup(&sc->wsel);
		KNOTE_LOCKED(&sc->wsel.si_note, 0);
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
			if (sc->dying)
				error = ENXIO;
			else if (ioflag & O_NONBLOCK)
				error = EWOULDBLOCK;
			else
				error = sx_sleep(sc, &sc->lock, PCATCH, "echowr",
				    0);
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
			selwakeup(&sc->rsel);
			KNOTE_LOCKED(&sc->rsel.si_note, 0);
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
			selwakeup(&sc->wsel);
			KNOTE_LOCKED(&sc->wsel.si_note, 0);
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
		selwakeup(&sc->wsel);
		KNOTE_LOCKED(&sc->wsel.si_note, 0);
		sx_xunlock(&sc->lock);
		error = 0;
		break;
	case FIONBIO:
		/* O_NONBLOCK is supported. */
		error = 0;
		break;
	case FIOASYNC:
		/* O_ASYNC is not supported. */
		if (*(int *)data != 0)
			error = EINVAL;
		else
			error = 0;
		break;
	case FIONREAD:
		sx_slock(&sc->lock);
		*(int *)data = MIN(INT_MAX, sc->valid);
		sx_sunlock(&sc->lock);
		error = 0;
		break;
	case FIONWRITE:
		sx_slock(&sc->lock);
		*(int *)data = MIN(INT_MAX, sc->len - sc->valid);
		sx_sunlock(&sc->lock);
		error = 0;
		break;
	default:
		error = ENOTTY;
		break;
	}
	return (error);
}

static int
echo_poll(struct cdev *dev, int events, struct thread *td)
{
	struct echodev_softc *sc = dev->si_drv1;
	int revents;

	revents = 0;
	sx_slock(&sc->lock);
	if (sc->valid != 0 || sc->writers == 0)
		revents |= events & (POLLIN | POLLRDNORM);
	if (sc->valid < sc->len)
		revents |= events & (POLLOUT | POLLWRNORM);
	if (revents == 0) {
		if ((events & (POLLIN | POLLRDNORM)) != 0)
			selrecord(td, &sc->rsel);
		if ((events & (POLLOUT | POLLWRNORM)) != 0)
			selrecord(td, &sc->wsel);
	}
	sx_sunlock(&sc->lock);
	return (revents);
}

static int
echo_kqfilter(struct cdev *dev, struct knote *kn)
{
	struct echodev_softc *sc = dev->si_drv1;

	switch (kn->kn_filter) {
	case EVFILT_READ:
		kn->kn_fop = &echo_read_filterops;
		kn->kn_hook = sc;
		knlist_add(&sc->rsel.si_note, kn, 0);
		return (0);
	case EVFILT_WRITE:
		kn->kn_fop = &echo_write_filterops;
		kn->kn_hook = sc;
		knlist_add(&sc->wsel.si_note, kn, 0);
		return (0);
	default:
		return (EINVAL);
	}
}

static void
echo_kqread_detach(struct knote *kn)
{
	struct echodev_softc *sc = kn->kn_hook;

	knlist_remove(&sc->rsel.si_note, kn, 0);
}

static int
echo_kqread_event(struct knote *kn, long hint)
{
	struct echodev_softc *sc = kn->kn_hook;

	kn->kn_data = sc->valid;
	if (sc->writers == 0) {
		kn->kn_flags |= EV_EOF;
		return (1);
	}
	kn->kn_flags &= ~EV_EOF;
	return (kn->kn_data > 0);
}

static void
echo_kqwrite_detach(struct knote *kn)
{
	struct echodev_softc *sc = kn->kn_hook;

	knlist_remove(&sc->wsel.si_note, kn, 0);
}

static int
echo_kqwrite_event(struct knote *kn, long hint)
{
	struct echodev_softc *sc = kn->kn_hook;

	kn->kn_data = sc->len - sc->valid;
	return (kn->kn_data > 0);
}

static void
echo_kn_lock(void *arg)
{
	struct sx *sx = arg;

	sx_xlock(sx);
}

static void
echo_kn_unlock(void *arg)
{
	struct sx *sx = arg;

	sx_xunlock(sx);
}

static void
echo_kn_assert_lock(void *arg, int what)
{
	struct sx *sx __diagused = arg;

	sx_assert(sx, what);
}

static void
echo_knlist_init(struct knlist *knl, struct echodev_softc *sc)
{
	knlist_init(knl, &sc->lock, echo_kn_lock, echo_kn_unlock,
	    echo_kn_assert_lock);
}

static int
echodev_create(struct echodev_softc **scp, size_t len)
{
	struct make_dev_args args;
	struct echodev_softc *sc;
	int error;

	sc = malloc(sizeof(*sc), M_ECHODEV, M_WAITOK | M_ZERO);
	sx_init(&sc->lock, "echo");
	echo_knlist_init(&sc->rsel.si_note, sc);
	echo_knlist_init(&sc->wsel.si_note, sc);
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
		knlist_destroy(&sc->rsel.si_note);
		knlist_destroy(&sc->wsel.si_note);
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
	if (sc->dev != NULL) {
		/* Force any sleeping threads to exit the driver. */
		sx_xlock(&sc->lock);
		sc->dying = true;
		wakeup(sc);
		sx_xunlock(&sc->lock);

		destroy_dev(sc->dev);
	}
	knlist_destroy(&sc->rsel.si_note);
	knlist_destroy(&sc->wsel.si_note);
	seldrain(&sc->rsel);
	seldrain(&sc->wsel);
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

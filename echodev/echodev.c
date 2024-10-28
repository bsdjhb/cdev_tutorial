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

static MALLOC_DEFINE(M_ECHODEV, "echodev", "Demo echo character device");

static struct cdev *echodev;
static char *echobuf;
static size_t echolen;
static struct sx echolock;

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
	size_t todo;
	int error;

	sx_slock(&echolock);
	if (uio->uio_offset >= echolen) {
		error = 0;
	} else {
		todo = MIN(uio->uio_resid, echolen - uio->uio_offset);
		error = uiomove(echobuf + uio->uio_offset, todo, uio);
	}
	sx_sunlock(&echolock);
	return (error);
}

static int
echo_write(struct cdev *dev, struct uio *uio, int ioflag)
{
	size_t todo;
	int error;

	sx_xlock(&echolock);
	if (uio->uio_offset >= echolen) {
		error = EFBIG;
	} else {
		todo = MIN(uio->uio_resid, echolen - uio->uio_offset);
		error = uiomove(echobuf + uio->uio_offset, todo, uio);
	}
	sx_xunlock(&echolock);
	return (error);
}

static int
echo_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int fflag,
    struct thread *td)
{
	int error;

	switch (cmd) {
	case ECHODEV_GBUFSIZE:
		sx_slock(&echolock);
		*(size_t *)data = echolen;
		sx_sunlock(&echolock);
		error = 0;
		break;
	case ECHODEV_SBUFSIZE:
	{
		size_t new_len;

		if ((fflag & FWRITE) == 0) {
			error = EPERM;
			break;
		}

		new_len = *(size_t *)data;
		sx_xlock(&echolock);
		if (new_len == echolen) {
			/* Nothing to do. */
		} else if (new_len < echolen) {
			echolen = new_len;
		} else {
			echobuf = reallocf(echobuf, new_len, M_ECHODEV,
			    M_WAITOK | M_ZERO);
			echolen = new_len;
		}
		sx_xunlock(&echolock);
		error = 0;
		break;
	}
	case ECHODEV_CLEAR:
		if ((fflag & FWRITE) == 0) {
			error = EPERM;
			break;
		}

		sx_xlock(&echolock);
		memset(echobuf, 0, echolen);
		sx_xunlock(&echolock);
		error = 0;
		break;
	default:
		error = ENOTTY;
		break;
	}
	return (error);
}

static int
echodev_load(size_t len)
{
	struct make_dev_args args;
	int error;

	sx_init(&echolock, "echo");
	echobuf = malloc(len, M_ECHODEV, M_WAITOK | M_ZERO);
	echolen = len;
	make_dev_args_init(&args);
	args.mda_flags = MAKEDEV_WAITOK | MAKEDEV_CHECKNAME;
	args.mda_devsw = &echo_cdevsw;
	args.mda_uid = UID_ROOT;
	args.mda_gid = GID_WHEEL;
	args.mda_mode = 0600;
	error = make_dev_s(&args, &echodev, "echo");
	return (error);
}

static int
echodev_unload(void)
{
	if (echodev != NULL)
		destroy_dev(echodev);
	sx_destroy(&echolock);
	free(echobuf, M_ECHODEV);
	return (0);
}

static int
echodev_modevent(module_t mod, int type, void *data)
{
	switch (type) {
	case MOD_LOAD:
		return (echodev_load(64));
	case MOD_UNLOAD:
		return (echodev_unload());
	default:
		return (EOPNOTSUPP);
	}
}

DEV_MODULE(echodev, echodev_modevent, NULL);

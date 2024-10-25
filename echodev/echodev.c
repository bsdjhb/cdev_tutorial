/*-
 * Copyright (c) 2024 John Baldwin <jhb@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sx.h>
#include <sys/uio.h>

static struct cdev *echodev;
static char echobuf[64];
static struct sx echolock;

static d_read_t echo_read;
static d_write_t echo_write;

static struct cdevsw echo_cdevsw = {
	.d_version =	D_VERSION,
	.d_read =	echo_read,
	.d_write =	echo_write,
	.d_name =	"echo"
};

static int
echo_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	size_t todo;
	int error;

	if (uio->uio_offset >= sizeof(echobuf))
		return (0);

	sx_slock(&echolock);
	todo = MIN(uio->uio_resid, sizeof(echobuf) - uio->uio_offset);
	error = uiomove(echobuf + uio->uio_offset, todo, uio);
	sx_sunlock(&echolock);
	return (error);
}

static int
echo_write(struct cdev *dev, struct uio *uio, int ioflag)
{
	size_t todo;
	int error;

	if (uio->uio_offset >= sizeof(echobuf))
		return (EFBIG);

	sx_xlock(&echolock);
	todo = MIN(uio->uio_resid, sizeof(echobuf) - uio->uio_offset);
	error = uiomove(echobuf + uio->uio_offset, todo, uio);
	sx_xunlock(&echolock);
	return (error);
}

static int
echodev_load(void)
{
	struct make_dev_args args;
	int error;

	sx_init(&echolock, "echo");
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
	return (0);
}

static int
echodev_modevent(module_t mod, int type, void *data)
{
	switch (type) {
	case MOD_LOAD:
		return (echodev_load());
	case MOD_UNLOAD:
		return (echodev_unload());
	default:
		return (EOPNOTSUPP);
	}
}

DEV_MODULE(echodev, echodev_modevent, NULL);

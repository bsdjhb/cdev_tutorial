/*-
 * Copyright (c) 2024 John Baldwin <jhb@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/module.h>

static struct cdev *echodev;

static struct cdevsw echo_cdevsw = {
	.d_version =	D_VERSION,
	.d_name =	"echo"
};

static int
echodev_load(void)
{
	struct make_dev_args args;
	int error;

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

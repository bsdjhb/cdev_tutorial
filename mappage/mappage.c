/*-
 * Copyright (c) 2025 John Baldwin <jhb@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <vm/vm.h>
#include <vm/pmap.h>

static MALLOC_DEFINE(M_MAPPAGE, "mappage", "Demo mappage character device");

static d_mmap_t mappage_mmap;

static struct cdevsw mappage_cdevsw = {
	.d_version =	D_VERSION,
	.d_mmap =	mappage_mmap,
	.d_name =	"mappage"
};

static int
mappage_mmap(struct cdev *dev, vm_ooffset_t offset, vm_paddr_t *paddr,
    int nprot, vm_memattr_t *memattr)
{
	if (offset != 0)
		return (EINVAL);

	*paddr = pmap_kextract((uintptr_t)dev->si_drv1);
	return (0);
}

static int
mappage_create(void)
{
	struct make_dev_args args;
	struct cdev *cdev;
	void *page;
	int error;

	page = malloc(PAGE_SIZE, M_MAPPAGE, M_WAITOK | M_ZERO);
	make_dev_args_init(&args);
	args.mda_flags = MAKEDEV_WAITOK | MAKEDEV_CHECKNAME;
	args.mda_devsw = &mappage_cdevsw;
	args.mda_uid = UID_ROOT;
	args.mda_gid = GID_WHEEL;
	args.mda_mode = 0600;
	args.mda_si_drv1 = page;
	error = make_dev_s(&args, &cdev, "mappage");
	if (error != 0) {
		free(page, M_MAPPAGE);
		return (error);
	}
	return (0);
}

static int
mappage_modevent(module_t mod, int type, void *data)
{
	switch (type) {
	case MOD_LOAD:
		return (mappage_create());
	default:
		return (EOPNOTSUPP);
	}
}

DEV_MODULE(mappage, mappage_modevent, NULL);

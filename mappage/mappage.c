/*-
 * Copyright (c) 2025 John Baldwin <jhb@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rwlock.h>
#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>

static d_mmap_single_t mappage_mmap_single;

static struct cdevsw mappage_cdevsw = {
	.d_version =	D_VERSION,
	.d_mmap_single = mappage_mmap_single,
	.d_name =	"mappage"
};

static int
mappage_mmap_single(struct cdev *cdev, vm_ooffset_t *offset, vm_size_t size,
    struct vm_object **object, int nprot)
{
	vm_object_t obj;

	obj = cdev->si_drv1;
	if (OFF_TO_IDX(round_page(*offset + size)) > obj->size)
		return (EINVAL);

	vm_object_reference(obj);
	*object = obj;
	return (0);
}

static int
mappage_create(struct cdev **cdevp)
{
	struct make_dev_args args;
	vm_object_t obj;
	int error;

	obj = vm_pager_allocate(OBJT_PHYS, NULL, PAGE_SIZE,
	    VM_PROT_DEFAULT, 0, NULL);
	if (obj == NULL)
		return (ENOMEM);
	make_dev_args_init(&args);
	args.mda_flags = MAKEDEV_WAITOK | MAKEDEV_CHECKNAME;
	args.mda_devsw = &mappage_cdevsw;
	args.mda_uid = UID_ROOT;
	args.mda_gid = GID_WHEEL;
	args.mda_mode = 0600;
	args.mda_si_drv1 = obj;
	error = make_dev_s(&args, cdevp, "mappage");
	if (error != 0) {
		vm_object_deallocate(obj);
		return (error);
	}
	return (0);
}

static void
mappage_destroy(struct cdev *cdev)
{
	if (cdev == NULL)
		return;

	vm_object_deallocate(cdev->si_drv1);
	destroy_dev(cdev);
}

static int
mappage_modevent(module_t mod, int type, void *data)
{
	static struct cdev *mappage_cdev;

	switch (type) {
	case MOD_LOAD:
		return (mappage_create(&mappage_cdev));
	case MOD_UNLOAD:
		mappage_destroy(mappage_cdev);
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

DEV_MODULE(mappage, mappage_modevent, NULL);

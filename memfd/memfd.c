/*-
 * Copyright (c) 2025 John Baldwin <jhb@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <vm/vm.h>
#include <vm/vm_object.h>

static d_open_t memfd_open;
static d_mmap_single_t memfd_mmap_single;
static void	memfd_dtor(void *);

static struct cdevsw memfd_cdevsw = {
	.d_version =	D_VERSION,
	.d_open =	memfd_open,
	.d_mmap_single = memfd_mmap_single,
	.d_name =	"memfd"
};

static int
memfd_open(struct cdev *cdev, int fflag, int devtype, struct thread *td)
{
	vm_object_t obj;
	int error;

	/* Read-only and write-only opens make no sense. */
	if ((fflag & (FREAD | FWRITE)) != (FREAD | FWRITE))
		return (EINVAL);

	/*
	 * Create an anonymous VM object with an initial size of 0 for
	 * each open file descriptor.
	 */
	obj = vm_object_allocate_anon(0, NULL, td->td_ucred, 0);
	if (obj == NULL)
		return (ENOMEM);
	error = devfs_set_cdevpriv(obj, memfd_dtor);
	if (error != 0)
		vm_object_deallocate(obj);
	return (error);

}

static void
memfd_dtor(void *arg)
{
	vm_object_t obj = arg;

	vm_object_deallocate(obj);
}

static int
memfd_mmap_single(struct cdev *cdev, vm_ooffset_t *offset, vm_size_t size,
    struct vm_object **object, int nprot)
{
	vm_object_t obj;
	vm_pindex_t objsize;
	vm_ooffset_t delta;
	void *priv;
	int error;

	error = devfs_get_cdevpriv(&priv);
	if (error != 0)
		return (error);
	obj = priv;

	/* Grow object if necessary. */
	objsize = OFF_TO_IDX(round_page(*offset + size));
	VM_OBJECT_WLOCK(obj);
	if (objsize > obj->size) {
		delta = IDX_TO_OFF(objsize - obj->size);
		if (!swap_reserve_by_cred(delta, obj->cred)) {
			VM_OBJECT_WUNLOCK(obj);
			return (ENOMEM);
		}
		obj->size = objsize;
		obj->charge += delta;
	}

	vm_object_reference_locked(obj);
	VM_OBJECT_WUNLOCK(obj);
	*object = obj;
	return (0);
}

static int
memfd_create(struct cdev **cdevp)
{
	struct make_dev_args args;

	make_dev_args_init(&args);
	args.mda_flags = MAKEDEV_WAITOK | MAKEDEV_CHECKNAME;
	args.mda_devsw = &memfd_cdevsw;
	args.mda_uid = UID_ROOT;
	args.mda_gid = GID_WHEEL;
	args.mda_mode = 0600;
	return (make_dev_s(&args, cdevp, "memfd"));
}

static void
memfd_destroy(struct cdev *cdev)
{
	if (cdev == NULL)
		return;

	destroy_dev(cdev);
}

static int
memfd_modevent(module_t mod, int type, void *data)
{
	static struct cdev *memfd_cdev;

	switch (type) {
	case MOD_LOAD:
		return (memfd_create(&memfd_cdev));
	case MOD_UNLOAD:
		memfd_destroy(memfd_cdev);
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

DEV_MODULE(memfd, memfd_modevent, NULL);

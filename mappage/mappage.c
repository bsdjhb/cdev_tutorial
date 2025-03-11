/*-
 * Copyright (c) 2025 John Baldwin <jhb@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <vm/pmap.h>

struct mappage_softc {
	struct cdev *dev;
	void	*page;
	struct mtx lock;
	bool	dying;
	bool	mapped;
};

static MALLOC_DEFINE(M_MAPPAGE, "mappage", "Demo mappage character device");

static d_mmap_single_t mappage_mmap_single;
static int	mappage_pager_ctor(void *handle, vm_ooffset_t size,
    vm_prot_t prot, vm_ooffset_t foff, struct ucred *cred, u_short *color);
static void	mappage_pager_dtor(void *handle);
static int	mappage_pager_fault(vm_object_t obj, vm_ooffset_t offset,
    int prot, vm_page_t *mres);

static struct cdevsw mappage_cdevsw = {
	.d_version =	D_VERSION,
	.d_mmap_single = mappage_mmap_single,
	.d_name =	"mappage"
};

static struct cdev_pager_ops mappage_cdev_pager_ops = {
	.cdev_pg_ctor = mappage_pager_ctor,
	.cdev_pg_dtor = mappage_pager_dtor,
	.cdev_pg_fault = mappage_pager_fault,
};

static int
mappage_mmap_single(struct cdev *cdev, vm_ooffset_t *offset, vm_size_t size,
    struct vm_object **object, int nprot)
{
	struct mappage_softc *sc = cdev->si_drv1;
	vm_object_t obj;

	if (round_page(*offset + size) > PAGE_SIZE)
		return (EINVAL);

	mtx_lock(&sc->lock);
	if (sc->dying) {
		mtx_unlock(&sc->lock);
		return (ENXIO);
	}
	mtx_unlock(&sc->lock);

	obj = cdev_pager_allocate(sc, OBJT_DEVICE, &mappage_cdev_pager_ops,
	    OFF_TO_IDX(PAGE_SIZE), nprot, *offset, curthread->td_ucred);
	if (obj == NULL)
		return (ENXIO);

	/*
	 * If an unload started while we were allocating the VM
	 * object, dying will now be set and the unloading thread will
	 * be waiting in destroy_dev().  Just release the VM object
	 * and fail the mapping request.
	 */
	mtx_lock(&sc->lock);
	if (sc->dying) {
		mtx_unlock(&sc->lock);
		vm_object_deallocate(obj);
		return (ENXIO);
	}
	mtx_unlock(&sc->lock);

	*object = obj;
	return (0);
}

static int
mappage_pager_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot,
    vm_ooffset_t foff, struct ucred *cred, u_short *color)
{
	struct mappage_softc *sc = handle;

	mtx_lock(&sc->lock);
	sc->mapped = true;
	mtx_unlock(&sc->lock);

	*color = 0;
	return (0);
}

static void
mappage_pager_dtor(void *handle)
{
	struct mappage_softc *sc = handle;

	mtx_lock(&sc->lock);
	sc->mapped = false;
	mtx_unlock(&sc->lock);
}

static int
mappage_pager_fault(vm_object_t object, vm_ooffset_t offset, int prot,
    vm_page_t *mres)
{
	struct mappage_softc *sc = object->handle;
	vm_page_t page;
	vm_paddr_t paddr;

	paddr = pmap_kextract((uintptr_t)sc->page + offset);

	/* See the end of old_dev_pager_fault in device_pager.c. */
	if (((*mres)->flags & PG_FICTITIOUS) != 0) {
		page = *mres;
		vm_page_updatefake(page, paddr, VM_MEMATTR_DEFAULT);
	} else {
		VM_OBJECT_WUNLOCK(object);
		page = vm_page_getfake(paddr, VM_MEMATTR_DEFAULT);
		VM_OBJECT_WLOCK(object);
		vm_page_replace(page, object, (*mres)->pindex, *mres);
		*mres = page;
	}
	vm_page_valid(page);
	return (VM_PAGER_OK);
}

static int
mappage_create(struct mappage_softc **scp)
{
	struct make_dev_args args;
	struct mappage_softc *sc;
	int error;

	sc = malloc(sizeof(*sc), M_MAPPAGE, M_WAITOK | M_ZERO);
	mtx_init(&sc->lock, "mappage", NULL, MTX_DEF);
	sc->page = malloc(PAGE_SIZE, M_MAPPAGE, M_WAITOK | M_ZERO);
	make_dev_args_init(&args);
	args.mda_flags = MAKEDEV_WAITOK | MAKEDEV_CHECKNAME;
	args.mda_devsw = &mappage_cdevsw;
	args.mda_uid = UID_ROOT;
	args.mda_gid = GID_WHEEL;
	args.mda_mode = 0600;
	args.mda_si_drv1 = sc;
	error = make_dev_s(&args, &sc->dev, "mappage");
	if (error != 0) {
		free(sc->page, M_MAPPAGE);
		mtx_destroy(&sc->lock);
		free(sc, M_MAPPAGE);
		return (error);
	}
	*scp = sc;
	return (0);
}

static int
mappage_destroy(struct mappage_softc *sc)
{
	mtx_lock(&sc->lock);
	if (sc->mapped) {
		mtx_unlock(&sc->lock);
		return (EBUSY);
	}
	sc->dying = true;
	mtx_unlock(&sc->lock);

	destroy_dev(sc->dev);
	free(sc->page, M_MAPPAGE);
	mtx_destroy(&sc->lock);
	free(sc, M_MAPPAGE);
	return (0);
}

static int
mappage_modevent(module_t mod, int type, void *data)
{
	static struct mappage_softc *mappage_softc;

	switch (type) {
	case MOD_LOAD:
		return (mappage_create(&mappage_softc));
	case MOD_UNLOAD:
		if (mappage_softc == NULL)
			return (0);
		return (mappage_destroy(mappage_softc));
	default:
		return (EOPNOTSUPP);
	}
}

DEV_MODULE(mappage, mappage_modevent, NULL);

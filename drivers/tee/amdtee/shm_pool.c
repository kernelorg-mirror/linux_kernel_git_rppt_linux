// SPDX-License-Identifier: MIT
/*
 * Copyright 2019 Advanced Micro Devices, Inc.
 */

#include <linux/slab.h>
#include <linux/tee_core.h>
#include <linux/psp.h>
#include "amdtee_private.h"

static int pool_op_alloc(struct tee_shm_pool *pool, struct tee_shm *shm,
			 size_t size, size_t align)
{
	unsigned int order = get_order(size);
	unsigned long va;
	int rc;

	/*
	 * Ignore alignment since this is already going to be page aligned
	 * and there's no need for any larger alignment.
	 */
	va = (unsigned long)kzalloc(PAGE_SIZE << (order), GFP_KERNEL);
	if (!va)
		return -ENOMEM;

	shm->kaddr = (void *)va;
	shm->paddr = __psp_pa((void *)va);
	shm->size = PAGE_SIZE << order;

	/* Map the allocated memory in to TEE */
	rc = amdtee_map_shmem(shm);
	if (rc) {
		kfree((void *)va);
		shm->kaddr = NULL;
		return rc;
	}

	return 0;
}

static void pool_op_free(struct tee_shm_pool *pool, struct tee_shm *shm)
{
	/* Unmap the shared memory from TEE */
	amdtee_unmap_shmem(shm);
	kfree((void *)(unsigned long)shm->kaddr);
	shm->kaddr = NULL;
}

static void pool_op_destroy_pool(struct tee_shm_pool *pool)
{
	kfree(pool);
}

static const struct tee_shm_pool_ops pool_ops = {
	.alloc = pool_op_alloc,
	.free = pool_op_free,
	.destroy_pool = pool_op_destroy_pool,
};

struct tee_shm_pool *amdtee_config_shm(void)
{
	struct tee_shm_pool *pool = kzalloc_obj(*pool);

	if (!pool)
		return ERR_PTR(-ENOMEM);

	pool->ops = &pool_ops;

	return pool;
}

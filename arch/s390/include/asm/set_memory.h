/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASMS390_SET_MEMORY_H
#define _ASMS390_SET_MEMORY_H

#include <asm-generic/set_memory.h>

int set_memory_4k(unsigned long addr, unsigned long numpages);

/*
 * Generate a variant of each set_memory() function:
 *
 * __set_memory_yy(void *start, void *end);
 *
 * It exists for convenience to avoid the usual (unsigned long) casts.
 */
#define __SET_MEMORY_FUNC(fname)					\
static inline int __##fname(void *start, void *end)			\
{									\
	unsigned long numpages;						\
									\
	numpages = (end - start) >> PAGE_SHIFT;				\
	return fname((unsigned long)start, numpages);			\
}

__SET_MEMORY_FUNC(set_memory_ro)
__SET_MEMORY_FUNC(set_memory_rw)
__SET_MEMORY_FUNC(set_memory_nx)
__SET_MEMORY_FUNC(set_memory_x)
__SET_MEMORY_FUNC(set_memory_rox)

bool kernel_page_present(struct page *page);

#endif

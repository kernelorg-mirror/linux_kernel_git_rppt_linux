/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_SET_MEMORY_H
#define __ASM_SET_MEMORY_H

/*
 * Functions to change memory attributes.
 */
int set_memory_ro(unsigned long addr, int numpages);
int set_memory_rw(unsigned long addr, int numpages);
int set_memory_x(unsigned long addr, int numpages);
int set_memory_nx(unsigned long addr, int numpages);

#ifdef CONFIG_GENERIC_PAGEATTR
int set_memory_rox(unsigned long addr, int numpages);
#define set_memory_rox set_memory_rox

#ifdef CONFIG_ARCH_HAS_SET_DIRECT_MAP
struct page;
int set_direct_map_invalid_noflush(struct page *page, unsigned int nr);
int set_direct_map_default_noflush(struct page *page, unsigned int nr);
#endif
#endif

#endif

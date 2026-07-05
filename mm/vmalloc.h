/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mm-internal APIs for vmalloc
 */
#ifndef __MM_VMALLOC_H
#define __MM_VMALLOC_H

#ifdef CONFIG_MMU
void __init vmalloc_init(void);
int __must_check vmap_pages_range_noflush(unsigned long addr, unsigned long end,
	pgprot_t prot, struct page **pages, unsigned int page_shift, gfp_t gfp_mask);
unsigned int get_vm_area_page_order(struct vm_struct *vm);
#else
static inline void vmalloc_init(void)
{
}

static inline
int __must_check vmap_pages_range_noflush(unsigned long addr, unsigned long end,
	pgprot_t prot, struct page **pages, unsigned int page_shift, gfp_t gfp_mask)
{
	return -EINVAL;
}
#endif

void clear_vm_uninitialized_flag(struct vm_struct *vm);

int __must_check __vmap_pages_range_noflush(unsigned long addr,
			       unsigned long end, pgprot_t prot,
			       struct page **pages, unsigned int page_shift);

void vunmap_range_noflush(unsigned long start, unsigned long end);

void __vunmap_range_noflush(unsigned long start, unsigned long end);

#endif /* __MM_VMALLOC_H */

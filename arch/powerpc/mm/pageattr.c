// SPDX-License-Identifier: GPL-2.0

/*
 * MMU-generic set_memory implementation for powerpc
 *
 * Copyright 2019-2021, IBM Corporation.
 */

#include <linux/mm.h>
#include <linux/pageattr.h>
#include <linux/vmalloc.h>
#include <linux/set_memory.h>

#include <asm/mmu.h>
#include <asm/page.h>
#include <asm/pgtable.h>

#include <mm/mmu_decl.h>

void arch_cpa_decode_leaf(struct cpa_leaf *leaf)
{
	pte_t pte = ptep_get(leaf->kpte);
	unsigned long size, entries;

	/*
	 * A 16K or 512K page of 8xx is a contiguous block of entries in one
	 * page table, each mapping its part of the page. An 8M page spans two
	 * tables.
	 */
	if (leaf->level == PGTABLE_LEVEL_PTE) {
		size = __pte_leaf_size(pmdp_get(pmd_off_k(leaf->addr)), pte);
		if (size > PAGE_SIZE && size < PMD_SIZE) {
			entries = size / PAGE_SIZE;
			leaf->kpte = PTR_ALIGN_DOWN(leaf->kpte,
						    entries * sizeof(pte_t));
			leaf->size = size;
			leaf->addr &= ~(size - 1);
			pte = ptep_get(leaf->kpte);
		}
	}

	leaf->pfn = pte_pfn(pte);
	leaf->prot = pte_pgprot(pte);
}

void arch_cpa_modify_leaf(struct cpa_leaf *leaf, pgprot_t prot)
{
	unsigned long old = pgprot_val(leaf->prot), new = pgprot_val(prot);

	pte_update(&init_mm, leaf->addr, leaf->kpte, old & ~new, new & ~old,
		   leaf->size > PAGE_SIZE);
}

void arch_cpa_flush(struct cpa_data *cpa)
{
	/* See ptesync comment in radix__set_pte_at() */
	if (radix_enabled())
		asm volatile("ptesync": : :"memory");

	cpa_flush_tlb_kernel_range(cpa);
}

#define CPA_OP_TRANS(old, new)	{ .set = __pgprot((new) & ~(old)),	\
				  .clr = __pgprot((old) & ~(new)) }

static const struct cpa_op_prot cpa_ops[CPA_OP_NR] = {
	/* Don't clear DIRTY bit */
	[CPA_OP_RO]	= CPA_OP_TRANS(_PAGE_KERNEL_RW & ~_PAGE_DIRTY,
				       _PAGE_KERNEL_RO),
	[CPA_OP_RW]	= CPA_OP_TRANS(_PAGE_KERNEL_RO, _PAGE_KERNEL_RW),
	[CPA_OP_X]	= CPA_OP_TRANS(_PAGE_KERNEL_RO, _PAGE_KERNEL_ROX),
	[CPA_OP_NX]	= CPA_OP_TRANS(_PAGE_KERNEL_ROX, _PAGE_KERNEL_RO),
	/* Don't clear DIRTY bit */
	[CPA_OP_ROX]	= CPA_OP_TRANS(_PAGE_KERNEL_RW & ~_PAGE_DIRTY,
				       _PAGE_KERNEL_ROX),
};

int arch_set_memory_prepare(struct cpa_data *cpa, enum cpa_op op)
{
#ifdef CONFIG_PPC_BOOK3S_64
	unsigned long end = cpa->addr + cpa->numpages * PAGE_SIZE;

	/*
	 * On hash, the linear mapping is not in the Linux page table so
	 * changing its protections has no effect. If in the future the
	 * set_memory_* functions are used on the linear map this will need
	 * to be updated.
	 */
	if (!radix_enabled() && cpa->numpages) {
		int region = get_region_id(cpa->addr);
		int end_region = get_region_id(end - 1);

		if (WARN_ON_ONCE((region != VMALLOC_REGION_ID &&
				  region != IO_REGION_ID) ||
				 end_region != region))
			return -EINVAL;
	}
#endif

	cpa_init_op(cpa, &cpa_ops[op]);

	return 0;
}

#if defined(CONFIG_DEBUG_PAGEALLOC) || defined(CONFIG_KFENCE)
#ifdef CONFIG_ARCH_SUPPORTS_DEBUG_PAGEALLOC
static int change_present(pte_t *ptep, unsigned long addr, void *data)
{
	if (data)
		pte_update(&init_mm, addr, ptep, 0, _PAGE_PRESENT, 0);
	else
		pte_update(&init_mm, addr, ptep, _PAGE_PRESENT, 0, 0);

	return 0;
}

void __kernel_map_pages(struct page *page, int numpages, int enable)
{
	int err;
	unsigned long addr = (unsigned long)page_address(page);
	unsigned long size = numpages * PAGE_SIZE;

	if (PageHighMem(page))
		return;

	if (IS_ENABLED(CONFIG_PPC_BOOK3S_64) && !radix_enabled()) {
		err = hash__kernel_map_pages(page, numpages, enable);
	} else {
		err = apply_to_existing_page_range(&init_mm, addr, size,
						   change_present,
						   (void *)(long)enable);

		/* See ptesync comment in radix__set_pte_at() */
		if (radix_enabled())
			asm volatile("ptesync": : :"memory");

		flush_tlb_kernel_range(addr, addr + size);
	}

	if (err)
		panic("%s: changing memory protections failed\n", __func__);
}
#endif
#endif

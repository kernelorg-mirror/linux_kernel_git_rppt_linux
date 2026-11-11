// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 */
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pageattr.h>

#include <asm/tlbflush.h>
#include <asm/set_memory.h>

struct page_change_data {
	pgprot_t set_mask;
	pgprot_t clear_mask;
};

static int change_page_range(pte_t *ptep, unsigned long addr, void *data)
{
	struct page_change_data *cdata = data;
	pte_t pte = *ptep;

	pte = clear_pte_bit(pte, cdata->clear_mask);
	pte = set_pte_bit(pte, cdata->set_mask);

	set_pte_ext(ptep, pte, 0);
	return 0;
}

/*
 * This function assumes that the range is mapped with PAGE_SIZE pages.
 */
static int __change_memory_common(unsigned long start, unsigned long size,
				pgprot_t set_mask, pgprot_t clear_mask)
{
	struct page_change_data data;
	int ret;

	data.set_mask = set_mask;
	data.clear_mask = clear_mask;

	ret = apply_to_page_range(&init_mm, start, size, change_page_range,
				  &data);

	flush_tlb_kernel_range(start, start + size);
	return ret;
}

/*
 * Only page mappings are changed: a section has a different format and is
 * smaller than the level it is found at.
 */
void arch_cpa_decode_leaf(struct cpa_leaf *leaf)
{
	pte_t pte = ptep_get(leaf->kpte);

	if (WARN_ON_ONCE(leaf->level != PGTABLE_LEVEL_PTE)) {
		leaf->pfn = 0;
		leaf->prot = __pgprot(0);
		return;
	}

	leaf->pfn = pte_pfn(pte);
	leaf->prot = __pgprot(pte_val(pte) & ~(PHYS_MASK & (s32)PAGE_MASK));
}

void arch_cpa_modify_leaf(struct cpa_leaf *leaf, pgprot_t prot)
{
	if (leaf->level == PGTABLE_LEVEL_PTE)
		set_pte_ext(leaf->kpte, pfn_pte(leaf->pfn, prot), 0);
}

void arch_cpa_flush(struct cpa_data *cpa)
{
	if (cpa->state & CPA_ST_FLUSH_ALL) {
		flush_tlb_all();
		return;
	}

	cpa_flush_tlb_kernel_range(cpa);
}

static bool range_in_range(unsigned long start, unsigned long size,
	unsigned long range_start, unsigned long range_end)
{
	return start >= range_start && start < range_end &&
		size <= range_end - start;
}

static const struct cpa_op_prot cpa_ops[CPA_OP_NR] = {
	[CPA_OP_RO]	= { .set = __pgprot(L_PTE_RDONLY) },
	[CPA_OP_RW]	= { .clr = __pgprot(L_PTE_RDONLY) },
	[CPA_OP_X]	= { .clr = __pgprot(L_PTE_XN) },
	[CPA_OP_NX]	= { .set = __pgprot(L_PTE_XN) },
	[CPA_OP_ROX]	= { .set = __pgprot(L_PTE_RDONLY),
			    .clr = __pgprot(L_PTE_XN) },
};

int arch_set_memory_prepare(struct cpa_data *cpa, enum cpa_op op)
{
	unsigned long end = PAGE_ALIGN(cpa->addr) + cpa->numpages * PAGE_SIZE;
	unsigned long start = cpa->addr & PAGE_MASK;
	unsigned long size = end - start;
	bool in_modules, in_vmalloc;

	if (!size)
		return 0;

	/*
	 * The sections of the linear map are copied to every page table, so
	 * only the page mappings of the modules and vmalloc areas can change.
	 */
	in_modules = range_in_range(start, size, MODULES_VADDR, MODULES_END);
	in_vmalloc = range_in_range(start, size, VMALLOC_START, VMALLOC_END);
	if (WARN_ON_ONCE(!in_modules && !in_vmalloc))
		return -EINVAL;

	cpa_init_op(cpa, &cpa_ops[op]);

	return 0;
}

int set_memory_valid(unsigned long addr, int numpages, int enable)
{
	if (enable)
		return __change_memory_common(addr, PAGE_SIZE * numpages,
					      __pgprot(L_PTE_VALID),
					      __pgprot(0));
	else
		return __change_memory_common(addr, PAGE_SIZE * numpages,
					      __pgprot(0),
					      __pgprot(L_PTE_VALID));
}

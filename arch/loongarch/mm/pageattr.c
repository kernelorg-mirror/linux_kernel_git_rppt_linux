// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Loongson Technology Corporation Limited
 */

#include <linux/pageattr.h>
#include <linux/pgtable.h>
#include <asm/set_memory.h>
#include <asm/tlbflush.h>

void arch_cpa_decode_leaf(struct cpa_leaf *leaf)
{
	pte_t pte = ptep_get(leaf->kpte);

	leaf->pfn = pte_pfn(pte);
	leaf->prot = pte_pgprot(pte);
}

void arch_cpa_modify_leaf(struct cpa_leaf *leaf, pgprot_t prot)
{
	set_pte(leaf->kpte, pfn_pte(leaf->pfn, prot));
}

void arch_cpa_flush(struct cpa_data *cpa)
{
	if (cpa->state & CPA_ST_FLUSH_ALL) {
		flush_tlb_all();
		return;
	}

	cpa_flush_tlb_kernel_range(cpa);
}

static const struct cpa_op_prot cpa_ops[CPA_OP_NR] = {
	[CPA_OP_RO]	= { .clr = __pgprot(_PAGE_WRITE | _PAGE_DIRTY) },
	[CPA_OP_RW]	= { .set = __pgprot(_PAGE_WRITE | _PAGE_DIRTY) },
	[CPA_OP_X]	= { .clr = __pgprot(_PAGE_NO_EXEC) },
	[CPA_OP_NX]	= { .set = __pgprot(_PAGE_NO_EXEC) },
	[CPA_OP_ROX]	= { .clr = __pgprot(_PAGE_WRITE | _PAGE_DIRTY |
					   _PAGE_NO_EXEC) },
};

int arch_cpa_prepare(struct cpa_data *cpa, enum cpa_op op)
{
	/* The direct mapping windows below vm_map_base have no page tables */
	if (cpa->addr < vm_map_base)
		return 0;

	cpa_init_op(cpa, &cpa_ops[op]);

	return 0;
}

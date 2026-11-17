// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019 SiFive
 */

#include <linux/pageattr.h>
#include <linux/pgtable.h>
#include <linux/proc_fs.h>
#include <linux/vmalloc.h>
#include <linux/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/bitops.h>
#include <asm/set_memory.h>

#ifdef CONFIG_PROC_FS
void arch_report_meminfo(struct seq_file *m)
{
	unsigned long levels = BIT(PGTABLE_LEVEL_PTE);

	if (IS_ENABLED(CONFIG_64BIT))
		levels |= BIT(PGTABLE_LEVEL_PMD);
	if (pgtable_l4_enabled())
		levels |= BIT(PGTABLE_LEVEL_PUD);

	cpa_report_meminfo(m, levels);
}
#endif

void arch_cpa_decode_leaf(struct cpa_leaf *leaf)
{
	pte_t pte = ptep_get(leaf->kpte);
	unsigned long nr;

	/* The PTEs of a NAPOT block map one naturally aligned range */
	if (leaf->level == PGTABLE_LEVEL_PTE && pte_napot(pte)) {
		nr = pte_leaf_size(pte) / PAGE_SIZE;
		leaf->kpte = PTR_ALIGN_DOWN(leaf->kpte, nr * sizeof(pte_t));
		leaf->size = nr * PAGE_SIZE;
		leaf->addr &= ~(leaf->size - 1);
	}

	leaf->pfn = pte_pfn(pte);
	leaf->prot = __pgprot(pte_val(pte) & ~_PAGE_PFN_MASK);
}

static void set_entry(pte_t *ptep, enum pgtable_level level, pte_t pte)
{
	switch (level) {
	case PGTABLE_LEVEL_PTE:
		set_pte(ptep, pte);
		break;
	case PGTABLE_LEVEL_PMD:
		set_pmd((pmd_t *)ptep, pte_pmd(pte));
		break;
	case PGTABLE_LEVEL_PUD:
		set_pud((pud_t *)ptep, pte_pud(pte));
		break;
	default:
		WARN_ON_ONCE(1);
		break;
	}
}

/*
 * A hart may cache an invalid translation, and only 64-bit can defer the
 * sfence.vma for a new valid mapping to the page fault handler.
 */
#ifdef CONFIG_64BIT
static void mark_new_valid_leaf(struct cpa_leaf *leaf)
{
	mark_new_valid_map();
}
#else
static void mark_new_valid_leaf(struct cpa_leaf *leaf)
{
	flush_tlb_kernel_range(leaf->addr, leaf->addr + leaf->size);
}
#endif

void arch_cpa_modify_leaf(struct cpa_leaf *leaf, pgprot_t prot)
{
	unsigned long nr = leaf->size / pgtable_level_size(leaf->level);
	unsigned long pfn = leaf->pfn;
	unsigned long i;

	/*
	 * Every PTE of a NAPOT block holds the base pfn with the size of the
	 * block encoded in its low bits.
	 */
	if (nr > 1)
		pfn |= nr / 2;

	for (i = 0; i < nr; i++)
		set_entry(leaf->kpte + i, leaf->level, pfn_pte(pfn, prot));

	if (!(pgprot_val(leaf->prot) & _PAGE_PRESENT) &&
	    (pgprot_val(prot) & _PAGE_PRESENT))
		mark_new_valid_leaf(leaf);
}

void arch_cpa_flush(struct cpa_data *cpa)
{
	if (cpa->state & CPA_ST_FLUSH_ALL) {
		flush_tlb_all();
		return;
	}

	cpa_flush_tlb_kernel_range(cpa);
}

#ifdef CONFIG_64BIT
bool arch_cpa_can_split(struct cpa_data *cpa, struct cpa_leaf *leaf)
{
	unsigned long size = pgtable_level_size(leaf->level);

	/*
	 * A top level leaf cannot be split because the change would have to
	 * be propagated to the page tables of all processes.
	 * Sv39 folds the PUD level into the top level.
	 */
	if (!pgtable_l4_enabled() && leaf->level == PGTABLE_LEVEL_PUD)
		return false;

	/* A NAPOT block changes as a whole and is never split */
	return leaf->size == size;
}

int arch_cpa_split_leaf(struct cpa_data *cpa, struct cpa_leaf *leaf,
			void *table)
{
	enum pgtable_level level = leaf->level;
	unsigned long pfn = leaf->pfn, pfninc;
	pgprot_t prot = leaf->prot;
	pte_t *kpte = leaf->kpte;
	pte_t *ptep = table;
	int i;

	pfninc = pgtable_level_size(level - 1) >> PAGE_SHIFT;
	for (i = 0; i < PTRS_PER_PTE; i++, pfn += pfninc)
		set_entry(ptep + i, level - 1, pfn_pte(pfn, prot));

	/* The table must be complete before a walk can find it. */
	smp_wmb();
	set_entry(kpte, level, pfn_pte(virt_to_pfn(table), PAGE_TABLE));
	/* A changed non-leaf entry needs an sfence.vma for all addresses */
	flush_tlb_all();

	return 0;
}

int arch_cpa_process_alias(struct cpa_data *cpa)
{
	unsigned long vaddr = cpa_addr(cpa, cpa->curpage);
	struct cpa_data alias_cpa;

	/* Memory that is not RAM, like ioremap(), has no linear map alias. */
	if (cpa_is_direct_map(vaddr) || !pfn_valid(cpa->pfn))
		return 0;

	alias_cpa = *cpa;
	alias_cpa.addr = (unsigned long)pfn_to_virt(cpa->pfn);
	pgprot_val(alias_cpa.mask_set) &= ~_PAGE_EXEC;
	pgprot_val(alias_cpa.mask_clr) &= ~_PAGE_EXEC;

	return cpa_change_alias(cpa, &alias_cpa);
}
#endif	/* CONFIG_64BIT */

static const struct cpa_op_prot cpa_ops[CPA_OP_NR] = {
	[CPA_OP_RO]	= { .set = __pgprot(_PAGE_READ),
			    .clr = __pgprot(_PAGE_WRITE) },
	[CPA_OP_RW]	= { .set = __pgprot(_PAGE_READ | _PAGE_WRITE) },
	[CPA_OP_X]	= { .set = __pgprot(_PAGE_EXEC) },
	[CPA_OP_NX]	= { .clr = __pgprot(_PAGE_EXEC) },
	[CPA_OP_ROX]	= { .set = __pgprot(_PAGE_READ | _PAGE_EXEC),
			    .clr = __pgprot(_PAGE_WRITE) },
	[CPA_OP_DIRECT_MAP_INVALID] = { .clr = __pgprot(_PAGE_PRESENT) },
	[CPA_OP_DIRECT_MAP_DEFAULT] = { .set = PAGE_KERNEL,
					.clr = __pgprot(_PAGE_EXEC) },
};

int arch_cpa_prepare(struct cpa_data *cpa, enum cpa_op op)
{
	cpa_init_op(cpa, &cpa_ops[op]);

	return 0;
}

#ifdef CONFIG_DEBUG_PAGEALLOC
static int debug_pagealloc_set_page(pte_t *pte, unsigned long addr, void *data)
{
	int enable = *(int *)data;

	unsigned long val = pte_val(ptep_get(pte));

	if (enable)
		val |= _PAGE_PRESENT;
	else
		val &= ~_PAGE_PRESENT;

	set_pte(pte, __pte(val));

	return 0;
}

void __kernel_map_pages(struct page *page, int numpages, int enable)
{
	if (!debug_pagealloc_enabled())
		return;

	unsigned long start = (unsigned long)page_address(page);
	unsigned long size = PAGE_SIZE * numpages;

	apply_to_existing_page_range(&init_mm, start, size, debug_pagealloc_set_page, &enable);

	flush_tlb_kernel_range(start, start + size);
}
#endif

bool kernel_page_present(struct page *page)
{
	unsigned long addr = (unsigned long)page_address(page);
	pgd_t *pgd;
	pud_t *pud;
	p4d_t *p4d;
	pmd_t *pmd, pmdval;
	pte_t *pte;

	pgd = pgd_offset_k(addr);
	if (!pgd_present(pgdp_get(pgd)))
		return false;
	if (pgd_leaf(pgdp_get(pgd)))
		return true;

	p4d = p4d_offset(pgd, addr);
	if (!p4d_present(p4dp_get(p4d)))
		return false;
	if (p4d_leaf(p4dp_get(p4d)))
		return true;

	pud = pud_offset(p4d, addr);
	if (!pud_present(pudp_get(pud)))
		return false;
	if (pud_leaf(pudp_get(pud)))
		return true;

	pmd = pmd_offset(pud, addr);
	pmdval = pmdp_get(pmd);
	if (!pmd_present(pmdval))
		return false;
	if (pmd_leaf(pmdval))
		return pmd_val(pmdval) & _PAGE_PRESENT;

	/* pte_present() is true for PAGE_KERNEL with _PAGE_PRESENT cleared */
	pte = pte_offset_kernel(pmd, addr);
	return pte_val(ptep_get(pte)) & _PAGE_PRESENT;
}

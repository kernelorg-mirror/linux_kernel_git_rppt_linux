// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright IBM Corp. 2011
 * Author(s): Jan Glauber <jang@linux.vnet.ibm.com>
 */
#include <linux/cpufeature.h>
#include <linux/hugetlb.h>
#include <linux/pageattr.h>
#include <linux/proc_fs.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <asm/cacheflush.h>
#include <asm/facility.h>
#include <asm/pgalloc.h>
#include <asm/kfence.h>
#include <asm/page.h>
#include <asm/asm.h>
#include <asm/set_memory.h>

void __storage_key_init_range(unsigned long start, unsigned long end)
{
	unsigned long boundary, size;

	while (start < end) {
		if (cpu_has_edat1()) {
			/* set storage keys for a 1MB frame */
			size = 1UL << 20;
			boundary = (start + size) & ~(size - 1);
			if (boundary <= end) {
				do {
					start = sske_frame(start, PAGE_DEFAULT_KEY);
				} while (start < boundary);
				continue;
			}
		}
		page_set_storage_key(start, PAGE_DEFAULT_KEY, 1);
		start += PAGE_SIZE;
	}
}

#ifdef CONFIG_PROC_FS
atomic_long_t __bootdata_preserved(direct_pages_count[PG_DIRECT_MAP_MAX]);

void arch_report_meminfo(struct seq_file *m)
{
	cpa_report_meminfo(m, BIT(PGTABLE_LEVEL_PTE) | BIT(PGTABLE_LEVEL_PMD) |
			   BIT(PGTABLE_LEVEL_PUD));
}
#endif /* CONFIG_PROC_FS */

static void pgt_set(unsigned long *old, unsigned long new, unsigned long addr,
		    unsigned long dtt)
{
	unsigned long *table, mask;

	mask = 0;
	if (cpu_has_edat2()) {
		switch (dtt) {
		case CRDTE_DTT_REGION3:
			mask = ~(PTRS_PER_PUD * sizeof(pud_t) - 1);
			break;
		case CRDTE_DTT_SEGMENT:
			mask = ~(PTRS_PER_PMD * sizeof(pmd_t) - 1);
			break;
		case CRDTE_DTT_PAGE:
			mask = ~(PTRS_PER_PTE * sizeof(pte_t) - 1);
			break;
		}
		table = (unsigned long *)((unsigned long)old & mask);
		crdte(*old, new, table, dtt, addr, get_lowcore()->kernel_asce.val);
	} else {
		cspg(old, *old, new);
	}
}

static unsigned long cpa_large_to_pte(unsigned long val)
{
	unsigned long prot = 0;

	if (val & _SEGMENT_ENTRY_PRESENT)
		prot |= _PAGE_PRESENT;
	if (val & _SEGMENT_ENTRY_INVALID)
		prot |= _PAGE_INVALID;
	if (val & _SEGMENT_ENTRY_PROTECT)
		prot |= _PAGE_PROTECT;
	if (val & _SEGMENT_ENTRY_NOEXEC)
		prot |= _PAGE_NOEXEC;
	if (val & _SEGMENT_ENTRY_SOFT_DIRTY)
		prot |= _PAGE_SOFT_DIRTY;
	if (val & _SEGMENT_ENTRY_YOUNG)
		prot |= _PAGE_YOUNG;
	if (val & _SEGMENT_ENTRY_DIRTY)
		prot |= _PAGE_DIRTY;
	if (val & _SEGMENT_ENTRY_READ)
		prot |= _PAGE_READ;
	if (val & _SEGMENT_ENTRY_WRITE)
		prot |= _PAGE_WRITE;

	return prot;
}

static unsigned long cpa_pte_to_large(unsigned long prot)
{
	unsigned long val = _SEGMENT_ENTRY_LARGE;

	if (prot & _PAGE_PRESENT)
		val |= _SEGMENT_ENTRY_PRESENT;
	if (prot & _PAGE_INVALID)
		val |= _SEGMENT_ENTRY_INVALID;
	if (prot & _PAGE_PROTECT)
		val |= _SEGMENT_ENTRY_PROTECT;
	if (prot & _PAGE_NOEXEC)
		val |= _SEGMENT_ENTRY_NOEXEC;
	if (prot & _PAGE_SOFT_DIRTY)
		val |= _SEGMENT_ENTRY_SOFT_DIRTY;
	if (prot & _PAGE_YOUNG)
		val |= _SEGMENT_ENTRY_YOUNG;
	if (prot & _PAGE_DIRTY)
		val |= _SEGMENT_ENTRY_DIRTY;
	if (prot & _PAGE_READ)
		val |= _SEGMENT_ENTRY_READ;
	if (prot & _PAGE_WRITE)
		val |= _SEGMENT_ENTRY_WRITE;

	return val;
}

void arch_cpa_decode_leaf(struct cpa_leaf *leaf)
{
	unsigned long val = pte_val(ptep_get(leaf->kpte));
	enum pgtable_level level = leaf->level;

	switch (level) {
	case PGTABLE_LEVEL_PTE:
		leaf->pfn = pte_pfn(__pte(val));
		leaf->prot = __pgprot(val & ~PAGE_MASK);
		break;
	case PGTABLE_LEVEL_PMD:
		leaf->pfn = pmd_pfn(__pmd(val));
		leaf->prot = __pgprot(cpa_large_to_pte(val));
		break;
	case PGTABLE_LEVEL_PUD:
		leaf->pfn = pud_pfn(__pud(val));
		leaf->prot = __pgprot(cpa_large_to_pte(val));
		break;
	default:
		leaf->pfn = 0;
		leaf->prot = __pgprot(0);
		break;
	}
}

void arch_cpa_modify_leaf(struct cpa_leaf *leaf, pgprot_t prot)
{
	unsigned long *entry = (unsigned long *)leaf->kpte;
	unsigned long val = PFN_PHYS(leaf->pfn);
	enum pgtable_level level = leaf->level;
	unsigned long lpaddr = leaf->addr;

	switch (level) {
	case PGTABLE_LEVEL_PTE:
		pgt_set(entry, val | pgprot_val(prot), lpaddr, CRDTE_DTT_PAGE);
		break;
	case PGTABLE_LEVEL_PMD:
		pgt_set(entry, val | cpa_pte_to_large(pgprot_val(prot)),
			lpaddr, CRDTE_DTT_SEGMENT);
		break;
	case PGTABLE_LEVEL_PUD:
		pgt_set(entry, val | _REGION_ENTRY_TYPE_R3 |
			cpa_pte_to_large(pgprot_val(prot)),
			lpaddr, CRDTE_DTT_REGION3);
		break;
	default:
		break;
	}
}

void arch_cpa_flush(struct cpa_data *cpa)
{
}

void *arch_cpa_alloc_pgtable(struct cpa_leaf *leaf)
{
	enum pgtable_level level = leaf->level;
	void *table;

	if (level == PGTABLE_LEVEL_PMD)
		table = vmem_pte_alloc();
	else
		table = vmem_crst_alloc(_SEGMENT_ENTRY_EMPTY);

	return table;
}

void arch_cpa_free_pgtable(struct cpa_leaf *leaf, void *table)
{
	enum pgtable_level level = leaf->level;

	if (level == PGTABLE_LEVEL_PMD)
		page_table_free(&init_mm, table);
	else
		free_pages((unsigned long)table, CRST_ALLOC_ORDER);
}

int arch_cpa_split_leaf(struct cpa_data *cpa, struct cpa_leaf *leaf,
			void *table)
{
	unsigned long *entry = (unsigned long *)leaf->kpte;
	unsigned long prot = pgprot_val(leaf->prot);
	unsigned long addr = PFN_PHYS(leaf->pfn);
	enum pgtable_level level = leaf->level;
	unsigned long lpaddr = leaf->addr;
	unsigned long i;

	switch (level) {
	case PGTABLE_LEVEL_PMD:
		for (i = 0; i < PTRS_PER_PTE; i++, addr += PAGE_SIZE)
			set_pte((pte_t *)table + i, __pte(addr | prot));
		pgt_set(entry, __pa(table) | _SEGMENT_ENTRY, lpaddr,
			CRDTE_DTT_SEGMENT);
		break;
	case PGTABLE_LEVEL_PUD:
		prot = cpa_pte_to_large(prot);
		for (i = 0; i < PTRS_PER_PMD; i++, addr += PMD_SIZE)
			set_pmd((pmd_t *)table + i, __pmd(addr | prot));
		pgt_set(entry, __pa(table) | _REGION3_ENTRY, lpaddr,
			CRDTE_DTT_REGION3);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/*
 * Memory hot-remove splits a region-third entry of the identity map that
 * it removes only a part of. It calls the hooks outside of a request,
 * under vmem_mutex and without cpa_lock.
 */
int split_pud_page(pud_t *pudp, unsigned long addr)
{
	struct cpa_leaf leaf = {
		.kpte = (pte_t *)pudp,
		.level = PGTABLE_LEVEL_PUD,
		.addr = addr,
		.size = PUD_SIZE,
	};
	void *table;
	int rc;

	arch_cpa_decode_leaf(&leaf);
	table = arch_cpa_alloc_pgtable(&leaf);
	if (!table)
		return -ENOMEM;

	rc = arch_cpa_split_leaf(NULL, &leaf, table);
	if (rc) {
		arch_cpa_free_pgtable(&leaf, table);
		return rc;
	}

	direct_map_count_split(PGTABLE_LEVEL_PUD);

	return 0;
}

/*
 * Changes to read-only permissions on kernel VA mappings are also
 * applied to the kernel direct mapping. Execute permissions are
 * intentionally not transferred to keep all allocated pages within
 * the direct mapping non-executable.
 */
#define CPA_ALIAS_PROT	(_PAGE_PROTECT | _PAGE_WRITE | _PAGE_DIRTY | \
			 _PAGE_SOFT_DIRTY)

int arch_cpa_process_alias(struct cpa_data *cpa)
{
	unsigned long set = pgprot_val(cpa->mask_set) & CPA_ALIAS_PROT;
	unsigned long clr = pgprot_val(cpa->mask_clr) & CPA_ALIAS_PROT;
	unsigned long vaddr = cpa_addr(cpa, cpa->curpage);
	struct cpa_data alias_cpa;

	/* The identity map covers the memory below ident_map_size. */
	if (cpa_is_direct_map(vaddr) || PFN_PHYS(cpa->pfn) >= ident_map_size)
		return 0;

	alias_cpa = *cpa;
	alias_cpa.addr = (unsigned long)__va(PFN_PHYS(cpa->pfn));
	alias_cpa.mask_set = __pgprot(set);
	alias_cpa.mask_clr = __pgprot(clr);

	return cpa_change_alias(cpa, &alias_cpa);
}

static const struct cpa_op_prot cpa_ops[CPA_OP_NR] = {
	[CPA_OP_RO]	= { .set = __pgprot(_PAGE_PROTECT),
			    .clr = __pgprot(_PAGE_WRITE) },
	[CPA_OP_RW]	= { .set = __pgprot(_PAGE_WRITE | _PAGE_DIRTY |
					   _PAGE_SOFT_DIRTY),
			    .clr = __pgprot(_PAGE_PROTECT) },
	[CPA_OP_X]	= { .clr = __pgprot(_PAGE_NOEXEC) },
	[CPA_OP_NX]	= { .set = __pgprot(_PAGE_NOEXEC) },
	[CPA_OP_ROX]	= { .set = __pgprot(_PAGE_PROTECT),
			    .clr = __pgprot(_PAGE_WRITE | _PAGE_NOEXEC) },
	[CPA_OP_DIRECT_MAP_INVALID] = { .set = __pgprot(_PAGE_INVALID) },
	[CPA_OP_DIRECT_MAP_DEFAULT] = { .set = __pgprot(__PAGE_KERNEL),
					.clr = __pgprot(~PAGE_MASK) },
};

int arch_cpa_prepare(struct cpa_data *cpa, enum cpa_op op)
{
	cpa_init_op(cpa, &cpa_ops[op]);

	/* _PAGE_NOEXEC needs the instruction-execution-protection facility */
	cpa->mask_set = __pgprot(pgprot_val(cpa->mask_set) & page_noexec_mask);
	cpa->mask_clr = __pgprot(pgprot_val(cpa->mask_clr) & page_noexec_mask);

	return 0;
}

int set_memory_4k(unsigned long addr, unsigned long numpages)
{
	struct cpa_data cpa = {
		.addr = addr,
		.numpages = numpages,
		.flags = CPA_REQ_SPLIT,
	};

	return change_page_attr(&cpa);
}

bool kernel_page_present(struct page *page)
{
	unsigned long addr;
	unsigned int cc;

	addr = (unsigned long)page_address(page);
	asm volatile(
		"	lra	%[addr],0(%[addr])\n"
		CC_IPM(cc)
		: CC_OUT(cc, cc), [addr] "+a" (addr)
		:
		: CC_CLOBBER);
	return CC_TRANSFORM(cc) == 0;
}

#if defined(CONFIG_DEBUG_PAGEALLOC) || defined(CONFIG_KFENCE)

static void ipte_range(pte_t *pte, unsigned long address, int nr)
{
	int i;

	if (test_facility(13)) {
		__ptep_ipte_range(address, nr - 1, pte, IPTE_GLOBAL);
		return;
	}
	for (i = 0; i < nr; i++) {
		__ptep_ipte(address, pte, 0, 0, IPTE_GLOBAL);
		address += PAGE_SIZE;
		pte++;
	}
}

void __kernel_map_pages(struct page *page, int numpages, int enable)
{
	unsigned long address;
	pte_t *ptep, pte;
	int nr, i, j;

	for (i = 0; i < numpages;) {
		address = (unsigned long)page_to_virt(page + i);
		ptep = virt_to_kpte(address);
		nr = (unsigned long)ptep >> ilog2(sizeof(long));
		nr = PTRS_PER_PTE - (nr & (PTRS_PER_PTE - 1));
		nr = min(numpages - i, nr);
		if (enable) {
			for (j = 0; j < nr; j++) {
				pte = ptep_get(ptep);
				pte = clear_pte_bit(pte, __pgprot(_PAGE_INVALID));
				set_pte(ptep, pte);
				address += PAGE_SIZE;
				ptep++;
			}
		} else {
			ipte_range(ptep, address, nr);
		}
		i += nr;
	}
}

#endif /* CONFIG_DEBUG_PAGEALLOC */

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 */
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mem_encrypt.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#include <linux/pageattr.h>

#include <asm/cacheflush.h>
#include <asm/pgtable-prot.h>
#include <asm/set_memory.h>
#include <asm/tlbflush.h>
#include <asm/kfence.h>

void arch_cpa_decode_leaf(struct cpa_leaf *leaf)
{
	enum pgtable_level level = leaf->level;
	pte_t pte = __ptep_get(leaf->kpte);
	pteval_t prot, ns_shared;
	unsigned int nr;

	if (pte_cont(pte)) {
		nr = level == PGTABLE_LEVEL_PTE ? CONT_PTES : CONT_PMDS;
		leaf->kpte = PTR_ALIGN_DOWN(leaf->kpte, nr * sizeof(pte_t));
		leaf->size *= nr;
		leaf->addr &= ~(leaf->size - 1);
		pte = __ptep_get(leaf->kpte);
	}

	/*
	 * The NS shared bit of a Realm is an address bit that selects the
	 * unprotected alias of the same memory.
	 */
	ns_shared = pte_val(pte) & PROT_NS_SHARED;
	pte = __pte(pte_val(pte) & ~ns_shared);

	switch (level) {
	case PGTABLE_LEVEL_PTE:
		leaf->pfn = pte_pfn(pte);
		break;
	case PGTABLE_LEVEL_PMD:
		leaf->pfn = pmd_pfn(pte_pmd(pte));
		break;
	default:
		leaf->pfn = pud_pfn(pte_pud(pte));
		break;
	}

	prot = pgprot_val(pte_pgprot(pte)) | ns_shared;
	leaf->prot = __pgprot((prot & ~PTE_TYPE_MASK) | (prot & PTE_VALID));
}

void arch_cpa_modify_leaf(struct cpa_leaf *leaf, pgprot_t prot)
{
	unsigned int nr = leaf->size / pgtable_level_size(leaf->level);
	enum pgtable_level level = leaf->level;
	pteval_t val = pgprot_val(prot);
	unsigned long pfn = leaf->pfn;
	pte_t *kpte = leaf->kpte;
	unsigned long pfninc;
	unsigned int i;

	pfninc = pgtable_level_size(level) >> PAGE_SHIFT;

	if (level == PGTABLE_LEVEL_PTE)
		val |= PTE_TYPE_PAGE & ~PTE_VALID;
	prot = __pgprot(val);

	/*
	 * The entries of a contiguous block are rewritten one by one, so the
	 * block has mixed permissions until the last store, as any mapping has
	 * until the TLB flush of the request.
	 */
	for (i = 0; i < nr; i++, pfn += pfninc) {
		switch (level) {
		case PGTABLE_LEVEL_PTE:
			__set_pte(kpte + i, pfn_pte(pfn, prot));
			break;
		case PGTABLE_LEVEL_PMD:
			set_pmd((pmd_t *)kpte + i, pfn_pmd(pfn, prot));
			break;
		default:
			set_pud((pud_t *)kpte + i, pfn_pud(pfn, prot));
			break;
		}
	}
}

void arch_cpa_flush(struct cpa_data *cpa)
{
	/*
	 * If the memory is being switched from present-invalid to valid without
	 * changing any other bits then a TLBI isn't required as a non-valid
	 * entry cannot be cached in the TLB.
	 */
	if (pgprot_val(cpa->mask_set) == PTE_PRESENT_VALID_KERNEL &&
	    pgprot_val(cpa->mask_clr) == PTE_PRESENT_INVALID)
		return;

	cpa_flush_tlb_kernel_range(cpa);
}

bool rodata_full __ro_after_init = true;

bool can_set_direct_map(void)
{
	/*
	 * rodata_full, DEBUG_PAGEALLOC and a Realm guest all require linear
	 * map to be mapped at page granularity, so that it is possible to
	 * protect/unprotect single pages.
	 *
	 * KFENCE pool requires page-granular mapping if initialized late.
	 *
	 * Realms need to make pages shared/protected at page granularity.
	 */
	return rodata_full || debug_pagealloc_enabled() ||
		arm64_kfence_can_set_direct_map() || is_realm_world();
}

/*
 * If we are manipulating read-only permissions, apply the same
 * change to the linear mapping of the pages that back this VM area.
 */
int arch_cpa_process_alias(struct cpa_data *cpa)
{
	pteval_t prot = pgprot_val(cpa->mask_set) | pgprot_val(cpa->mask_clr);
	unsigned long vaddr = cpa_addr(cpa, cpa->curpage);
	struct cpa_data alias_cpa;
	int ret;

	if (!rodata_full || !(prot & PTE_RDONLY))
		return 0;

	/* Memory that is not RAM, like ioremap(), has no linear map alias. */
	if (cpa_is_direct_map(vaddr) || !pfn_valid(cpa->pfn))
		return 0;

	alias_cpa = *cpa;
	alias_cpa.addr = (unsigned long)page_address(pfn_to_page(cpa->pfn));
	pgprot_val(alias_cpa.mask_set) &= PTE_RDONLY | PTE_WRITE;
	pgprot_val(alias_cpa.mask_clr) &= PTE_RDONLY | PTE_WRITE;

	ret = cpa_change_alias(cpa, &alias_cpa);
	if (alias_cpa.state & CPA_ST_FLUSHTLB)
		arch_cpa_flush(&alias_cpa);

	return ret;
}

static const struct cpa_op_prot cpa_ops[CPA_OP_NR] = {
	[CPA_OP_RO]	= { .set = __pgprot(PTE_RDONLY),
			    .clr = __pgprot(PTE_WRITE) },
	[CPA_OP_RW]	= { .set = __pgprot(PTE_WRITE),
			    .clr = __pgprot(PTE_RDONLY) },
	[CPA_OP_X]	= { .set = __pgprot(PTE_GP),
			    .clr = __pgprot(PTE_PXN) },
	[CPA_OP_NX]	= { .set = __pgprot(PTE_PXN),
			    .clr = __pgprot(PTE_GP) },
	[CPA_OP_ROX]	= { .set = __pgprot(PTE_RDONLY | PTE_GP),
			    .clr = __pgprot(PTE_WRITE | PTE_PXN) },
	[CPA_OP_DIRECT_MAP_INVALID] = {
		.set = __pgprot(PTE_PRESENT_INVALID),
		.clr = __pgprot(PTE_VALID | PTE_NG),
	},
	[CPA_OP_DIRECT_MAP_DEFAULT] = {
		.set = __pgprot(PTE_VALID | PTE_NG | PTE_WRITE),
		.clr = __pgprot(PTE_PRESENT_INVALID | PTE_RDONLY),
	},
};

int arch_cpa_prepare(struct cpa_data *cpa, enum cpa_op op)
{
	/* Nothing in the linear map is invalid without can_set_direct_map() */
	if ((op == CPA_OP_DIRECT_MAP_INVALID ||
	     op == CPA_OP_DIRECT_MAP_DEFAULT) && !can_set_direct_map())
		return 0;

	cpa_init_op(cpa, &cpa_ops[op]);

	/*
	 * A valid kernel entry has nG only with nG mappings and GP only with
	 * BTI, see PTE_MAYBE_NG and PTE_MAYBE_GP.
	 */
	if (!arm64_use_ng_mappings &&
	    (pgprot_val(cpa->mask_set) & PTE_VALID))
		pgprot_val(cpa->mask_set) &= ~PTE_NG;
	if (!system_supports_bti_kernel())
		pgprot_val(cpa->mask_set) &= ~PTE_GP;

	return 0;
}

void arch_cpa_enter(void)
{
	lazy_mmu_mode_enable();
}

void arch_cpa_leave(void)
{
	lazy_mmu_mode_disable();
}

static int set_valid_pte(pte_t *ptep, unsigned long addr, void *data)
{
	struct cpa_op_prot *prot = data;
	pte_t pte = __ptep_get(ptep);

	/*
	 * Some users clear and set bits which alias each other (e.g. PTE_NG and
	 * PTE_PRESENT_INVALID). It is therefore important that we always clear
	 * first then set.
	 */
	pte = clear_pte_bit(pte, prot->clr);
	pte = set_pte_bit(pte, prot->set);
	__set_pte(ptep, pte);

	return 0;
}

/*
 * KFENCE and debug_pagealloc change the linear map with interrupts
 * disabled. Their pages are mapped with PTEs that nothing splits or
 * collapses.
 */
int set_memory_valid(unsigned long addr, int numpages, int enable)
{
	unsigned long size = PAGE_SIZE * numpages;
	struct cpa_op_prot prot;
	int ret;

	if (enable) {
		prot.set = __pgprot(PTE_PRESENT_VALID_KERNEL);
		prot.clr = __pgprot(PTE_PRESENT_INVALID);
	} else {
		prot.set = __pgprot(PTE_PRESENT_INVALID);
		prot.clr = __pgprot(PTE_PRESENT_VALID_KERNEL);
	}

	ret = apply_to_existing_page_range(&init_mm, addr, size, set_valid_pte,
					   &prot);

	/*
	 * If the memory is being switched from present-invalid to valid without
	 * changing any other bits then a TLBI isn't required as a non-valid
	 * entry cannot be cached in the TLB.
	 */
	if (!enable)
		flush_tlb_kernel_range(addr, addr + size);

	return ret;
}

/*
 * Unlike set_memory_*(), the arm64 users of the core don't purge the
 * lazily freed vmap aliases first.
 */
int arm64_change_page_attr(struct cpa_data *cpa)
{
	int ret;

	arch_cpa_enter();
	ret = change_page_attr_noflush(cpa);
	arch_cpa_leave();

	if (cpa->state & CPA_ST_FLUSHTLB)
		arch_cpa_flush(cpa);

	return ret;
}

static int __change_memory_common(unsigned long start, unsigned long size,
				  pgprot_t set_mask, pgprot_t clear_mask)
{
	struct cpa_data cpa = {
		.addr = start,
		.numpages = size >> PAGE_SHIFT,
		.mask_set = set_mask,
		.mask_clr = clear_mask,
		.flags = CPA_REQ_NO_CHECK_ALIAS,
	};

	return arm64_change_page_attr(&cpa);
}

int __set_memory_enc_dec(unsigned long addr, int numpages, bool encrypt)
{
	unsigned long set_prot = 0, clear_prot = 0;
	phys_addr_t start, end;
	int ret;

	if (!is_realm_world())
		return 0;

	if (!__is_lm_address(addr))
		return -EINVAL;

	start = __virt_to_phys(addr);
	end = start + numpages * PAGE_SIZE;

	if (encrypt)
		clear_prot = PROT_NS_SHARED;
	else
		set_prot = PROT_NS_SHARED;

	/*
	 * Break the mapping before we make any changes to avoid stale TLB
	 * entries or Synchronous External Aborts caused by RIPAS_EMPTY
	 */
	ret = __change_memory_common(addr, PAGE_SIZE * numpages,
				     __pgprot(set_prot | PTE_PRESENT_INVALID),
				     __pgprot(clear_prot | PTE_PRESENT_VALID_KERNEL));

	if (ret)
		return ret;

	if (encrypt)
		ret = rsi_set_memory_range_protected(start, end);
	else
		ret = rsi_set_memory_range_shared(start, end);

	if (ret)
		return ret;

	return __change_memory_common(addr, PAGE_SIZE * numpages,
				      __pgprot(PTE_PRESENT_VALID_KERNEL),
				      __pgprot(PTE_PRESENT_INVALID));
}

#ifdef CONFIG_DEBUG_PAGEALLOC
void __kernel_map_pages(struct page *page, int numpages, int enable)
{
	if (!can_set_direct_map())
		return;

	set_memory_valid((unsigned long)page_address(page), numpages, enable);
}
#endif /* CONFIG_DEBUG_PAGEALLOC */

/*
 * This function is used to determine if a linear map page has been marked as
 * not-valid. Walk the page table and check the PTE_VALID bit.
 *
 * Because this is only called on the kernel linear map,  p?d_sect() implies
 * p?d_present(). When debug_pagealloc is enabled, sections mappings are
 * disabled.
 */
bool kernel_page_present(struct page *page)
{
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;
	pte_t *ptep;
	unsigned long addr = (unsigned long)page_address(page);

	pgdp = pgd_offset_k(addr);
	if (pgd_none(READ_ONCE(*pgdp)))
		return false;

	p4dp = p4d_offset(pgdp, addr);
	if (p4d_none(READ_ONCE(*p4dp)))
		return false;

	pudp = pud_offset(p4dp, addr);
	pud = READ_ONCE(*pudp);
	if (pud_none(pud))
		return false;
	if (pud_leaf(pud))
		return pud_valid(pud);

	pmdp = pmd_offset(pudp, addr);
	pmd = READ_ONCE(*pmdp);
	if (pmd_none(pmd))
		return false;
	if (pmd_leaf(pmd))
		return pmd_valid(pmd);

	ptep = pte_offset_kernel(pmdp, addr);
	return pte_valid(__ptep_get(ptep));
}

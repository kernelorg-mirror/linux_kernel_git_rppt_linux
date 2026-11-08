// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2002 Andi Kleen, SuSE Labs.
 * Thanks to Ben LaHaise for precious feedback.
 */
#include <linux/mm.h>
#include <linux/pageattr.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/cleanup.h>
#include <linux/vmstat.h>
#include <linux/pgalloc.h>
#include <linux/highmem.h>
#include <linux/vmalloc.h>
#include <linux/set_memory.h>

#include <asm/tlbflush.h>

/*
 * Serialize cpa() using cpa_lock so that we don't allow any other cpu, with
 * stale large tlb entries, to change the page attribute in parallel to some
 * other cpu splitting a large page entry along with changing the attribute.
 */
static DEFINE_SPINLOCK(cpa_lock);

#if defined(CONFIG_PROC_FS) || defined(CONFIG_CPA_STATISTICS)
static const char *cpa_size_str(enum pgtable_level level, char *buf, size_t len)
{
	unsigned long size = pgtable_level_size(level);

	if (size >= SZ_1G)
		snprintf(buf, len, "%luG", size >> 30);
	else if (size >= SZ_1M)
		snprintf(buf, len, "%luM", size >> 20);
	else
		snprintf(buf, len, "%luk", size >> 10);

	return buf;
}
#endif

static atomic_long_t cpa_direct_map_pages[PGTABLE_LEVEL_NUM];

void direct_map_count_update(enum pgtable_level level, long pages)
{
	atomic_long_add(pages, &cpa_direct_map_pages[level]);
}

void direct_map_count_split(enum pgtable_level level)
{
	if (atomic_long_dec_if_positive(&cpa_direct_map_pages[level]) < 0)
		return;

	if (system_state == SYSTEM_RUNNING) {
		if (level == PGTABLE_LEVEL_PMD)
			count_vm_event(DIRECT_MAP_LEVEL2_SPLIT);
		else if (level == PGTABLE_LEVEL_PUD)
			count_vm_event(DIRECT_MAP_LEVEL3_SPLIT);
	}
	atomic_long_add(PTRS_PER_PTE, &cpa_direct_map_pages[level - 1]);
}

void direct_map_count_collapse(enum pgtable_level level)
{
	atomic_long_inc(&cpa_direct_map_pages[level]);
	if (system_state == SYSTEM_RUNNING) {
		if (level == PGTABLE_LEVEL_PMD)
			count_vm_event(DIRECT_MAP_LEVEL2_COLLAPSE);
		else if (level == PGTABLE_LEVEL_PUD)
			count_vm_event(DIRECT_MAP_LEVEL3_COLLAPSE);
	}
	atomic_long_sub(PTRS_PER_PTE, &cpa_direct_map_pages[level - 1]);
}

#ifdef CONFIG_PROC_FS
void cpa_report_meminfo(struct seq_file *m, unsigned long levels)
{
	enum pgtable_level level;
	char label[32], sz[16];

	for (level = PGTABLE_LEVEL_PTE; level <= PGTABLE_LEVEL_PGD; level++) {
		if (!(levels & BIT(level)))
			continue;

		snprintf(label, sizeof(label), "DirectMap%s:",
			 cpa_size_str(level, sz, sizeof(sz)));
		seq_printf(m, "%-16s%8lu kB\n", label,
			   atomic_long_read(&cpa_direct_map_pages[level]) *
			   (pgtable_level_size(level) >> 10));
	}
}
#endif

#ifdef CONFIG_CPA_STATISTICS

static unsigned long cpa_checked[PGTABLE_LEVEL_NUM];
static unsigned long cpa_sameprot[PGTABLE_LEVEL_NUM];
static unsigned long cpa_preserved[PGTABLE_LEVEL_NUM];

static inline void cpa_inc_pxd_checked(int level)
{
	cpa_checked[level]++;
}

static inline void cpa_inc_pte_install(void)
{
	data_race(cpa_checked[PGTABLE_LEVEL_PTE]++);
}

static inline void cpa_inc_pxd_sameprot(int level)
{
	cpa_sameprot[level]++;
}

static inline void cpa_inc_pxd_preserved(int level)
{
	cpa_preserved[level]++;
}

static void cpastats_show_level(struct seq_file *m, enum pgtable_level level)
{
	char sz[16];

	cpa_size_str(level, sz, sizeof(sz));
	seq_printf(m, "%s pages checked:     %16lu\n", sz, cpa_checked[level]);
	seq_printf(m, "%s pages sameprot:    %16lu\n", sz, cpa_sameprot[level]);
	seq_printf(m, "%s pages preserved:   %16lu\n", sz,
		   cpa_preserved[level]);
}

static int cpastats_show(struct seq_file *m, void *p)
{
	char sz[16];

	cpastats_show_level(m, PGTABLE_LEVEL_PUD);
	cpastats_show_level(m, PGTABLE_LEVEL_PMD);
	seq_printf(m, "%s pages set-checked: %16lu\n",
		   cpa_size_str(PGTABLE_LEVEL_PTE, sz, sizeof(sz)),
		   cpa_checked[PGTABLE_LEVEL_PTE]);
	return 0;
}

static int cpastats_open(struct inode *inode, struct file *file)
{
	return single_open(file, cpastats_show, NULL);
}

static const struct file_operations cpastats_fops = {
	.open		= cpastats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init cpa_stats_init(void)
{
	debugfs_create_file("cpa_stats", 0400, NULL, NULL, &cpastats_fops);
	return 0;
}
late_initcall(cpa_stats_init);
#else
static inline void cpa_inc_pxd_checked(int level) { }
static inline void cpa_inc_pte_install(void) { }
static inline void cpa_inc_pxd_sameprot(int level) { }
static inline void cpa_inc_pxd_preserved(int level) { }
#endif

unsigned long cpa_addr(struct cpa_data *cpa, unsigned long idx)
{
	if (cpa->flags & CPA_REQ_PAGES_ARRAY) {
		struct page *page = cpa->pages[idx];

		if (unlikely(PageHighMem(page)))
			return 0;

		return (unsigned long)page_address(page);
	}

	return cpa->addr + idx * PAGE_SIZE;
}

static pte_t *cpa_lookup_in_pgd(pgd_t *pgdp, unsigned long address,
				enum pgtable_level *level)
{
	p4d_t *p4dp, p4d;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;

	*level = PGTABLE_LEVEL_PGD;
	if (pgd_none(*pgdp))
		return NULL;

	*level = PGTABLE_LEVEL_P4D;
	p4dp = p4d_offset(pgdp, address);
	p4d = p4dp_get(p4dp);
	if (p4d_none(p4d))
		return NULL;
	if (p4d_leaf(p4d) || !p4d_present(p4d))
		return (pte_t *)p4dp;

	*level = PGTABLE_LEVEL_PUD;
	pudp = pud_offset(p4dp, address);
	pud = pudp_get(pudp);
	if (pud_none(pud))
		return NULL;
	if (pud_leaf(pud) || !pud_present(pud))
		return (pte_t *)pudp;

	*level = PGTABLE_LEVEL_PMD;
	pmdp = pmd_offset(pudp, address);
	pmd = pmdp_get(pmdp);
	if (pmd_none(pmd))
		return NULL;
	if (pmd_leaf(pmd) || !pmd_present(pmd))
		return (pte_t *)pmdp;

	*level = PGTABLE_LEVEL_PTE;
	return pte_offset_kernel(pmdp, address);
}

static pte_t *cpa_lookup(struct cpa_data *cpa, unsigned long address,
			 struct cpa_leaf *leaf)
{
	pgd_t *pgd = cpa_pgd(cpa, address);

	leaf->kpte = cpa_lookup_in_pgd(pgd, address, &leaf->level);
	if (!leaf->kpte || (leaf->level == PGTABLE_LEVEL_PTE &&
			    pte_none(ptep_get(leaf->kpte))))
		return NULL;

	leaf->size = pgtable_level_size(leaf->level);
	leaf->addr = address & pgtable_level_mask(leaf->level);
	arch_cpa_decode_leaf(leaf);

	return leaf->kpte;
}

static void *arch_cpa_alloc_pgtable(struct cpa_data *cpa, struct cpa_leaf *leaf)
{
	if (leaf->level == PGTABLE_LEVEL_PMD)
		return pte_alloc_one_kernel(&init_mm);

	return pmd_alloc_one(&init_mm, leaf->addr);
}

static void arch_cpa_free_pgtable(struct cpa_leaf *leaf, void *table)
{
	if (!table)
		return;

	if (leaf->level == PGTABLE_LEVEL_PMD)
		pte_free_kernel(&init_mm, table);
	else
		pmd_free(&init_mm, table);
}

static int __should_split_large_page(struct cpa_data *cpa,
				     struct cpa_leaf *leaf)
{
	unsigned long address = cpa_addr(cpa, cpa->curpage);
	enum pgtable_level level = leaf->level;
	pgprot_t new_prot, req_prot;
	unsigned long lpaddr = leaf->addr;
	unsigned long old_pfn = leaf->pfn;
	unsigned long psize = leaf->size;
	pgprot_t old_prot = leaf->prot;
	unsigned long numpages, pfn;

	if (level > PGTABLE_LEVEL_PUD)
		return -EINVAL;

	cpa_inc_pxd_checked(level);

	/*
	 * Calculate the number of pages, which fit into this large
	 * page starting at address:
	 */
	numpages = (lpaddr + psize - address) >> PAGE_SHIFT;
	if (numpages < cpa->numpages)
		cpa->numpages = numpages;

	req_prot = cpa_req_prot(cpa, leaf);

	/*
	 * old_pfn points to the large page base pfn. So we need to add the
	 * offset of the virtual address:
	 */
	pfn = old_pfn + ((address & (psize - 1)) >> PAGE_SHIFT);
	cpa->pfn = pfn;

	numpages = psize >> PAGE_SHIFT;

	/*
	 * Optimization: If the requested pgprot is the same as the current
	 * pgprot, then the large page can be preserved and no updates are
	 * required independent of alignment and length of the requested
	 * range.
	 */
	if (pgprot_val(req_prot) == pgprot_val(old_prot)) {
		cpa_inc_pxd_sameprot(level);
		return 0;
	}

	/*
	 * If the requested range does not cover the full page, split it up
	 */
	if (address != lpaddr || cpa->numpages != numpages)
		return 1;

	/*
	 * Check whether the requested pgprot is conflicting with a static
	 * protection requirement in the large page.
	 */
	new_prot = arch_cpa_new_prot(cpa, leaf);

	/*
	 * If there is a conflict, split the large page.
	 *
	 * There used to be a 4k wise evaluation trying really hard to
	 * preserve the large pages, but experimentation has shown, that this
	 * does not help at all. There might be corner cases which would
	 * preserve one large page occasionally, but it's really not worth the
	 * extra code and cycles for the common case.
	 */
	if (pgprot_val(req_prot) != pgprot_val(new_prot))
		return 1;

	/* All checks passed. Update the large page mapping. */
	arch_cpa_modify_leaf(leaf, new_prot);
	cpa->state |= CPA_ST_FLUSHTLB;
	cpa_inc_pxd_preserved(level);
	return 0;
}

static int should_split_large_page(struct cpa_data *cpa, struct cpa_leaf *leaf)
{
	if (cpa->flags & CPA_REQ_SPLIT)
		return 1;

	return __should_split_large_page(cpa, leaf);
}

static int split_large_page(struct cpa_data *cpa, struct cpa_leaf *leaf)
{
	unsigned long address = cpa_addr(cpa, cpa->curpage);
	enum pgtable_level level = leaf->level;
	unsigned long size = leaf->size;
	pte_t *kpte = leaf->kpte;
	struct cpa_leaf tmp;
	void *table = NULL;
	int ret;

	if (size == pgtable_level_size(level)) {
		spin_unlock(&cpa_lock);
		if (cpa->state & CPA_ST_INIT_MM_LOCKED)
			mmap_read_unlock(&init_mm);
		table = arch_cpa_alloc_pgtable(cpa, leaf);
		if (cpa->state & CPA_ST_INIT_MM_LOCKED)
			mmap_read_lock(&init_mm);
		spin_lock(&cpa_lock);
		if (!table)
			return -ENOMEM;
	}

	/*
	 * Check for races, another CPU might have split this page
	 * up for us already:
	 */
	if (cpa_lookup(cpa, address, &tmp) != kpte) {
		arch_cpa_free_pgtable(leaf, table);
		return 0;
	}
	*leaf = tmp;

	ret = arch_cpa_split_leaf(cpa, leaf, table);
	if (ret) {
		arch_cpa_free_pgtable(leaf, table);
		return ret;
	}

	if (table && cpa_is_direct_map(address))
		direct_map_count_split(leaf->level);

	return 0;
}

int cpa_process_fault(struct cpa_data *cpa, struct cpa_leaf *leaf)
{
	unsigned long vaddr = cpa_addr(cpa, cpa->curpage);
	enum pgtable_level level = leaf->level;
	unsigned long numpages;

	/*
	 * Ignore all non primary paths.
	 */
	if (cpa->state & CPA_ST_ALIAS) {
		cpa->numpages = 1;
		return 0;
	}

	/*
	 * Ignore the NULL PTE for kernel identity mapping, as it is expected
	 * to have holes.
	 */
	if (cpa_is_direct_map(vaddr)) {
		numpages = (pgtable_level_size(level) -
			    (vaddr & ~pgtable_level_mask(level))) >> PAGE_SHIFT;
		cpa->numpages = min(cpa->numpages, numpages);
		cpa->pfn = __pa(vaddr) >> PAGE_SHIFT;
		return 0;
	}

	WARN(1, "CPA: called for zero pte. "
		"vaddr = %lx cpa->addr = %lx\n", vaddr, cpa->addr);

	return -EFAULT;
}

static int __change_page_attr(struct cpa_data *cpa)
{
	unsigned long address;
	struct cpa_leaf leaf;
	int do_split, ret;

	address = cpa_addr(cpa, cpa->curpage);
repeat:
	if (!cpa_lookup(cpa, address, &leaf))
		return arch_cpa_process_fault(cpa, &leaf);

	if (leaf.size == PAGE_SIZE) {
		pgprot_t old_prot = leaf.prot;
		unsigned long pfn = leaf.pfn;
		pgprot_t new_prot;

		cpa_inc_pte_install();

		new_prot = arch_cpa_new_prot(cpa, &leaf);
		cpa->pfn = pfn;
		/*
		 * Do we really change anything ?
		 */
		if (pgprot_val(new_prot) != pgprot_val(old_prot)) {
			arch_cpa_modify_leaf(&leaf, new_prot);
			cpa->state |= CPA_ST_FLUSHTLB;
		}
		cpa->numpages = 1;
		return 0;
	}

	/*
	 * Check, whether we can keep the large page intact
	 * and just change the pte:
	 */
	do_split = should_split_large_page(cpa, &leaf);
	/*
	 * When the range fits into the existing large page,
	 * return. cpa->numpages and cpa->state have been updated in
	 * should_split_large_page():
	 */
	if (do_split <= 0)
		return do_split;

	/*
	 * We have to split the large page:
	 */
	if (WARN_ON_ONCE(!arch_cpa_can_split(cpa, &leaf)))
		return -EINVAL;

	ret = split_large_page(cpa, &leaf);
	if (!ret)
		goto repeat;

	return ret;
}

int change_page_attr_noflush(struct cpa_data *cpa)
{
	unsigned long numpages = cpa->numpages;
	unsigned long rempages = numpages;
	int ret = 0;

	/*
	 * No changes, easy!
	 */
	if (!(pgprot_val(cpa->mask_set) | pgprot_val(cpa->mask_clr)) &&
	    !(cpa->flags & CPA_REQ_SPLIT))
		return ret;

	while (rempages) {
		/*
		 * Store the remaining nr of pages for the large page
		 * preservation check.
		 */
		cpa->numpages = rempages;
		/* for array changes, we can't use large page */
		if (cpa->flags & CPA_REQ_PAGES_ARRAY)
			cpa->numpages = 1;

		scoped_guard(spinlock, &cpa_lock)
			ret = __change_page_attr(cpa);
		if (ret)
			goto out;

		if (!(cpa->state & CPA_ST_ALIAS) &&
		    !(cpa->flags & CPA_REQ_NO_CHECK_ALIAS)) {
			ret = arch_cpa_process_alias(cpa);
			if (ret)
				goto out;
		}

		/*
		 * Adjust the number of pages with the result of the
		 * CPA operation. Either a large page has been
		 * preserved or a single page update happened.
		 */
		if (WARN_ON_ONCE(cpa->numpages > rempages || !cpa->numpages)) {
			ret = -EINVAL;
			goto out;
		}
		rempages -= cpa->numpages;
		cpa->curpage += cpa->numpages;
	}

out:
	/* Restore the original numpages */
	cpa->numpages = numpages;
	return ret;
}

void cpa_flush_tlb_kernel_range(struct cpa_data *cpa)
{
	unsigned long addr, i;

	if (cpa->flags & CPA_REQ_PAGES_ARRAY) {
		for (i = 0; i < cpa->numpages; i++) {
			addr = cpa_addr(cpa, i);
			flush_tlb_kernel_range(addr, addr + PAGE_SIZE);
		}
		return;
	}

	flush_tlb_kernel_range(cpa->addr,
			       cpa->addr + cpa->numpages * PAGE_SIZE);
}

int cpa_change_alias(struct cpa_data *cpa, struct cpa_data *alias)
{
	int ret;

	alias->flags &= ~CPA_REQ_PAGES_ARRAY;
	alias->state &= ~CPA_ST_FLUSHTLB;
	alias->state |= CPA_ST_ALIAS;
	alias->curpage = 0;

	ret = change_page_attr_noflush(alias);
	if (alias->state & CPA_ST_FLUSHTLB)
		cpa->state |= CPA_ST_FLUSHTLB | CPA_ST_FLUSH_ALL;

	return ret;
}

int change_page_attr(struct cpa_data *cpa)
{
	int ret;

	/* Ensure we are PAGE_SIZE aligned */
	if (!(cpa->flags & CPA_REQ_PAGES_ARRAY)) {
		/*
		 * CPA_REQ_PAGES_ARRAY implies it is aligned.
		 * No need to check in that case
		 */
		if (cpa->addr & ~PAGE_MASK) {
			cpa->addr &= PAGE_MASK;
			/*
			 * People should not be passing in unaligned addresses:
			 */
			WARN_ON_ONCE(1);
		}
	}

	/* Must avoid aliasing mappings in the highmem code */
	kmap_flush_unused();

	vm_unmap_aliases();

	/* Avoid race with concurrent CPA collapse. */
	cpa->state = CPA_ST_INIT_MM_LOCKED;
	scoped_guard(mmap_read_lock, &init_mm)
		ret = change_page_attr_noflush(cpa);
	cpa->state &= ~CPA_ST_INIT_MM_LOCKED;

	/*
	 * Check whether we really changed something:
	 */
	if (!(cpa->state & CPA_ST_FLUSHTLB))
		return ret;

	/*
	 * On error; flush everything to be sure.
	 */
	if (ret)
		cpa->state |= CPA_ST_FLUSH_ALL;

	arch_cpa_flush(cpa);

	if (ret)
		return ret;

	if (!(cpa->flags & CPA_REQ_COLLAPSE))
		return 0;

	/*
	 * Take the mmap write lock on init_mm to:
	 * - Avoid a use-after-free if raced by ptdump (which takes its own
	 *   write lock on init_mm).
	 * - Serialise concurrent CPA walkers.
	 */
	scoped_guard(mmap_write_lock, &init_mm) {
		guard(spinlock)(&cpa_lock);
		arch_cpa_collapse(cpa);
	}

	return ret;
}

static int set_memory_op(unsigned long addr, int numpages, enum cpa_op op)
{
	struct cpa_data cpa = {
		.addr = addr,
		.numpages = numpages,
	};
	int ret;

	ret = arch_set_memory_prepare(&cpa, op);
	if (ret)
		return ret;

	if (!pgprot_val(cpa.mask_set) && !pgprot_val(cpa.mask_clr) &&
	    !(cpa.flags & CPA_REQ_SPLIT))
		return 0;

	arch_set_memory_enter();
	ret = change_page_attr(&cpa);
	arch_set_memory_leave();

	return ret;
}

int set_memory_x(unsigned long addr, int numpages)
{
	return set_memory_op(addr, numpages, CPA_OP_X);
}

int set_memory_nx(unsigned long addr, int numpages)
{
	return set_memory_op(addr, numpages, CPA_OP_NX);
}

int set_memory_ro(unsigned long addr, int numpages)
{
	return set_memory_op(addr, numpages, CPA_OP_RO);
}

int set_memory_rox(unsigned long addr, int numpages)
{
	return set_memory_op(addr, numpages, CPA_OP_ROX);
}

int set_memory_rw(unsigned long addr, int numpages)
{
	return set_memory_op(addr, numpages, CPA_OP_RW);
}

#ifdef CONFIG_ARCH_HAS_SET_DIRECT_MAP
static int set_direct_map(struct page *page, unsigned int nr, enum cpa_op op)
{
	struct cpa_data cpa = {
		.addr = (unsigned long)page_address(page),
		.numpages = nr,
		.flags = CPA_REQ_NO_CHECK_ALIAS,
	};
	int ret;

	ret = arch_set_memory_prepare(&cpa, op);
	if (ret)
		return ret;

	/*
	 * No alias checking needed for setting present flag. otherwise,
	 * we may need to break large pages for 64-bit kernel text
	 * mappings (this adds to complexity if we want to do this from
	 * atomic context especially). Let's keep it simple!
	 */
	arch_set_memory_enter();
	ret = change_page_attr_noflush(&cpa);
	arch_set_memory_leave();

	return ret;
}

int set_direct_map_invalid_noflush(struct page *page, unsigned int nr)
{
	return set_direct_map(page, nr, CPA_OP_DIRECT_MAP_INVALID);
}

#ifndef set_direct_map_default_noflush
int set_direct_map_default_noflush(struct page *page, unsigned int nr)
{
	return set_direct_map(page, nr, CPA_OP_DIRECT_MAP_DEFAULT);
}
#endif
#endif /* CONFIG_ARCH_HAS_SET_DIRECT_MAP */

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGEATTR_H
#define _LINUX_PAGEATTR_H

#include <linux/mm.h>
#include <linux/pgtable.h>

/**
 * DOC: Kernel Page Table Protection Modifications
 *
 * There are two families of APIs that allow modifying the protection of the
 * kernel mappings:
 *
 * set_memory_*() change the protection of an arbitrary kernel mapping.
 *
 * set_direct_map_*() change the protection of the direct/identity/linear
 * mapping.
 *
 * change_page_attr() and change_page_attr_noflush(), or CPA for short,
 * implement the generic part of these APIs and leave the page table
 * specifics to the architecture.
 *
 * CPA walks the kernel page tables, determines at what level the
 * protection change must be done, splits large mappings if necessary and
 * updates the protection of the requested range.
 *
 * CPA keeps the mappings as large as it can. A large page stays intact
 * when a request changes all of it the same way, and is split only when a
 * request changes a part of it.
 *
 * The same holds for a contiguous block of entries that the TLB may cache as
 * one translation, like, for example, the contiguous bit of arm64.
 *
 * CPA keeps the alias mappings of the memory consistent: when the protection
 * of a page changes, its other mappings, like the direct map, change with it
 * according to architecture requirements.
 *
 * Every step of a request looks up and changes the page tables under
 * cpa_lock. It keeps concurrent requests from splitting or changing the same
 * large mapping at the same time, and a collapse, which holds it as well,
 * from freeing page tables under a CPA lookup. A step drops cpa_lock to
 * allocate a page table for a split and repeats the lookup after retaking the
 * lock.
 *
 * Page table walkers that don't take cpa_lock, like ptdump and text poking,
 * rely on the init_mm mmap lock instead. A collapse frees page tables, so it
 * holds that lock for writing.
 *
 * The architecture describes its page tables with the hooks below: the format
 * of a page table entry, how a large page is split and whether it can be split
 * at all, which mappings are aliases and how the TLB is flushed.
 *
 * The hooks that have a default are optional.
 */

struct seq_file;

/**
 * struct cpa_data - a request to change the protection of kernel mappings
 * @addr: the virtual address of the first page
 * @pgd: the page tables to change, NULL for init_mm
 * @mask_set: the protection bits to set, in the architecture's PTE format
 * @mask_clr: the protection bits to clear, in the architecture's PTE format
 * @numpages: the number of pages; while the walk runs, the number of pages
 *	of the current step
 * @curpage: the index of the first page of the current step
 * @pfn: the pfn of the first page of the current step; for an architecture
 *	that populates holes, the caller sets it to the first pfn to map
 * @flags: CPA_REQ_* request flags set by the caller
 * @state: CPA_ST_* state flags set by change_page_attr()
 */
struct cpa_data {
	unsigned long	addr;
	pgd_t		*pgd;
	pgprot_t	mask_set;
	pgprot_t	mask_clr;
	unsigned long	numpages;
	unsigned long	curpage;
	unsigned long	pfn;
	unsigned int	flags;
	unsigned int	state;
};

/**
 * struct cpa_leaf - a mapping found by the lookup
 * @kpte: the page table entry corresponding to @addr or the first entry in a
 *	contiguous block
 * @level: the level at which the lookup found the mapping
 * @addr: the first virtual address of the mapping
 * @size: the size of the mapping: the size of @level, or of the contiguous
 *	block
 * @pfn: the first pfn of the mapping
 * @prot: the protection of the mapping in the architecture's PTE format, as
 *	decoded by arch_cpa_decode_leaf()
 */
struct cpa_leaf {
	pte_t			*kpte;
	enum pgtable_level	level;
	unsigned long		addr;
	unsigned long		size;
	unsigned long		pfn;
	pgprot_t		prot;
};

/* cpa_data::flags */
#define CPA_REQ_NO_CHECK_ALIAS	0x01 /* Do not search for aliases */
#define CPA_REQ_COLLAPSE	0x02 /* try to collapse large pages */
#define CPA_REQ_SPLIT		0x04 /* split large pages */

/* cpa_data::state */
#define CPA_ST_FLUSHTLB		0x01 /* the TLB needs a flush */
#define CPA_ST_FLUSH_ALL	0x02 /* flush the entire TLB */
#define CPA_ST_ALIAS		0x04 /* the request is for an alias */

#include <asm/pageattr.h>

static inline pgprot_t cpa_req_prot(const struct cpa_data *cpa,
				    const struct cpa_leaf *leaf)
{
	return __pgprot((pgprot_val(leaf->prot) & ~pgprot_val(cpa->mask_clr)) |
			pgprot_val(cpa->mask_set));
}

static inline pgd_t *cpa_pgd(struct cpa_data *cpa, unsigned long address)
{
	if (!cpa->pgd)
		return pgd_offset_k(address);

	return cpa->pgd + pgd_index(address);
}

static inline unsigned long cpa_addr(struct cpa_data *cpa, unsigned long idx)
{
	return cpa->addr + idx * PAGE_SIZE;
}

/* Helpers for the hooks */
int cpa_process_fault(struct cpa_data *cpa);
int cpa_change_alias(struct cpa_data *cpa, struct cpa_data *alias);
void cpa_flush_tlb_kernel_range(struct cpa_data *cpa);

/**
 * change_page_attr() - change the protection of kernel mappings and flush the
 *	TLB
 * @cpa: the request
 *
 * Unmaps the unused kmap and the lazily freed vmap aliases first, as they
 * may map the pages with the old protection. With CPA_REQ_COLLAPSE, also
 * collapses large pages after the flush.
 *
 * On failure, the pages processed before it keep their new protection, and
 * arch_cpa_flush() is called with CPA_ST_FLUSH_ALL if any of them changed.
 *
 * Context: May sleep.
 * Return: 0 on success, -errno on failure.
 */
int change_page_attr(struct cpa_data *cpa);

/**
 * change_page_attr_noflush() - change the protection of kernel mappings
 * @cpa: the request
 *
 * Unlike change_page_attr(), it doesn't unmap the kmap and vmap aliases of
 * the pages, and the caller is responsible for flushing the TLB.
 *
 * Context: May sleep to allocate a page table for a split.
 * Return: 0 on success, -errno on failure.
 */
int change_page_attr_noflush(struct cpa_data *cpa);

/*
 * The direct map counters behind the DirectMap lines of /proc/meminfo.
 * change_page_attr() counts the large pages of the direct map it splits,
 * and the architecture counts every other change of its direct map. The
 * architecture prints the counters with cpa_report_meminfo().
 */
#ifdef CONFIG_PROC_FS
void direct_map_count_update(enum pgtable_level level, long pages);
void direct_map_count_split(enum pgtable_level level);
void direct_map_count_collapse(enum pgtable_level level);
void cpa_report_meminfo(struct seq_file *m, unsigned long levels);
#else
static inline void direct_map_count_update(enum pgtable_level level,
					   long pages) { }
static inline void direct_map_count_split(enum pgtable_level level) { }
static inline void direct_map_count_collapse(enum pgtable_level level) { }
#endif

/**
 * arch_cpa_decode_leaf() - decode a page table entry
 * @leaf: the mapping to decode
 *
 * Fill @leaf->pfn and @leaf->prot from the entry @leaf->kpte at @leaf->level.
 * If the entry is a part of a contiguous block, extend @leaf to the block.
 *
 * @leaf->prot has the same format at every level: the PTE format, without
 * the pfn and without the bits that tell a large entry from a small one.
 * arch_cpa_modify_leaf() converts it back. The entry may be not present,
 * but it is never empty.
 *
 * Context: cpa_lock held, must not sleep.
 */
void arch_cpa_decode_leaf(struct cpa_leaf *leaf);

/**
 * arch_cpa_modify_leaf() - change the protection of a mapping
 * @leaf: the mapping to change
 * @prot: the new protection
 *
 * Store the entry of @leaf, or every entry of its contiguous block, with
 * @prot. The TLB is flushed after the walk, by arch_cpa_flush() or by the
 * caller of change_page_attr_noflush().
 * An architecture whose TLB may cache an entry that is not valid handles an
 * entry that becomes valid here.
 *
 * Context: cpa_lock held, must not sleep.
 */
void arch_cpa_modify_leaf(struct cpa_leaf *leaf, pgprot_t prot);

/**
 * arch_cpa_flush() - flush the TLB for the pages of a request
 * @cpa: the request
 *
 * CPA_ST_FLUSH_ALL in @cpa->state means an alias of the pages changed as
 * well, or the request failed.
 * An architecture without aliases, one that flushes the aliases in
 * arch_cpa_process_alias(), or one whose page table updates flush the TLB by
 * themselves may ignore it.
 */
void arch_cpa_flush(struct cpa_data *cpa);

/**
 * arch_cpa_new_prot() - adjust the protections calculated in cpa_req_prot()
 * with architecture constraints
 * @cpa: the request
 * @leaf: the mapping to change
 *
 * A large page is split when the result differs from cpa_req_prot().
 *
 * Context: cpa_lock held, must not sleep.
 * Return: the new protection of @leaf.
 */
#ifndef arch_cpa_new_prot
static inline pgprot_t arch_cpa_new_prot(struct cpa_data *cpa,
					 struct cpa_leaf *leaf)
{
	return cpa_req_prot(cpa, leaf);
}
#endif

#ifndef arch_cpa_can_split
/**
 * arch_cpa_can_split() - check if a large page can be split
 * @cpa: the request
 * @leaf: the large page
 *
 * Refuse a split before a page table is allocated for it. A request that
 * needs a split the architecture cannot do fails with a warning and
 * -EINVAL. The default refuses every split.
 *
 * An architecture must override both arch_cpa_can_split() and
 * arch_cpa_split_leaf() or none.
 *
 * Context: cpa_lock held, must not sleep.
 * Return: true if arch_cpa_split_leaf() can split @leaf.
 */
static inline bool arch_cpa_can_split(struct cpa_data *cpa,
				      struct cpa_leaf *leaf)
{
	return false;
}

/**
 * arch_cpa_split_leaf() - split a large page
 * @cpa: the request
 * @leaf: the large page
 * @table: the page table page from arch_cpa_alloc_pgtable(), NULL for a
 *	contiguous block
 *
 * Replace @leaf with @table, filled with entries that map the same memory
 * with the same protection. A contiguous block is split by making each of
 * its entries a mapping of its own.
 * In both cases, do the TLB maintenance the CPU requires for the change.
 *
 * Context: cpa_lock held, must not sleep.
 * Return: 0 on success, -errno on failure.
 */
static inline int arch_cpa_split_leaf(struct cpa_data *cpa,
				      struct cpa_leaf *leaf, void *table)
{
	return -EINVAL;
}
#endif

#ifndef arch_cpa_alloc_pgtable
/**
 * arch_cpa_alloc_pgtable() - allocate the page table for a split
 * @leaf: the large page to split
 *
 * An architecture must override both arch_cpa_alloc_pgtable() and
 * arch_cpa_free_pgtable() or none.
 *
 * Context: May sleep.
 * Return: the page table, NULL on failure.
 */
void *arch_cpa_alloc_pgtable(struct cpa_leaf *leaf);

/**
 * arch_cpa_free_pgtable() - free a page table from arch_cpa_alloc_pgtable()
 * @leaf: the large page the table was allocated for
 * @table: the page table
 *
 * Context: cpa_lock held, must not sleep.
 */
void arch_cpa_free_pgtable(struct cpa_leaf *leaf, void *table);
#endif

/**
 * cpa_is_direct_map() - check if an address is in the direct map
 * @addr: the virtual address
 *
 * Return: true if @addr is in the range of the direct map, which may have
 * holes.
 */
#ifndef cpa_is_direct_map
static inline bool cpa_is_direct_map(unsigned long addr)
{
	return addr >= PAGE_OFFSET && addr < (unsigned long)high_memory;
}
#endif

/**
 * arch_cpa_process_fault() - handle a page without a mapping
 * @cpa: the request, its current page has no mapping
 *
 * On entry @cpa->numpages is the number of pages left in the request, and
 * @cpa->pfn is the pfn of the previous step, or the one the caller set. To
 * skip pages, set @cpa->numpages to their number and return 0. Without
 * CPA_ST_ALIAS in @cpa->state, also set @cpa->pfn to the pfn of the first
 * skipped page, which arch_cpa_process_alias() uses next.
 *
 * The default skips a page of an alias and a page in a hole of the direct
 * map, and fails with a warning for any other address.
 *
 * Context: cpa_lock held, must not sleep.
 * Return: 0 to continue the walk, -errno to fail the request.
 */
#ifndef arch_cpa_process_fault
static inline int arch_cpa_process_fault(struct cpa_data *cpa)
{
	return cpa_process_fault(cpa);
}
#endif

/**
 * arch_cpa_process_alias() - change the aliases of the current pages
 * @cpa: the request
 *
 * Change the aliases of the @cpa->numpages pages at @cpa->pfn that the
 * current step of the walk processed. Each alias is changed with
 * cpa_change_alias() on a copy of @cpa with the address and the masks of
 * the alias. cpa_change_alias() sets CPA_ST_FLUSH_ALL in @cpa when the
 * alias changed; an architecture may flush the alias here instead.
 *
 * Context: May sleep to split the large pages of an alias.
 * Return: 0 on success, -errno on failure.
 */
#ifndef arch_cpa_process_alias
static inline int arch_cpa_process_alias(struct cpa_data *cpa)
{
	return 0;
}
#endif

/**
 * arch_cpa_collapse() - collapse the large pages of a request
 * @cpa: the request
 *
 * Replace the page tables of the range of a CPA_REQ_COLLAPSE request with
 * large pages where all their entries map contiguous memory with the same
 * protection. The TLB is already flushed for the request.
 *
 * Context: init_mm write locked, cpa_lock held, must not sleep.
 */
#ifndef arch_cpa_collapse
static inline void arch_cpa_collapse(struct cpa_data *cpa)
{
}
#endif

/* The operations of set_memory_*() and set_direct_map_*_noflush() */
enum cpa_op {
	CPA_OP_RO,
	CPA_OP_RW,
	CPA_OP_X,
	CPA_OP_NX,
	CPA_OP_ROX,
	CPA_OP_DIRECT_MAP_INVALID,
	CPA_OP_DIRECT_MAP_DEFAULT,
	CPA_OP_NR
};

/**
 * struct cpa_op_prot - the masks and flags of an operation
 * @set: the protection bits to set, in the architecture's PTE format
 * @clr: the protection bits to clear, in the architecture's PTE format
 * @req: CPA_REQ_* flags of the request
 */
struct cpa_op_prot {
	pgprot_t	set;
	pgprot_t	clr;
	unsigned int	req;
};

static inline void cpa_init_op(struct cpa_data *cpa,
			       const struct cpa_op_prot *prot)
{
	cpa->mask_set = prot->set;
	cpa->mask_clr = prot->clr;
	cpa->flags |= prot->req;
}

/**
 * arch_cpa_prepare() - prepare the request of an operation
 * @cpa: the request with the address and the number of pages
 * @op: the operation
 *
 * Fill the masks and the flags of @cpa for @op, usually with cpa_init_op()
 * from a table of the operations. A request with empty masks succeeds
 * without changing anything.
 *
 * Return: 0 on success, -errno to refuse the request.
 */
int arch_cpa_prepare(struct cpa_data *cpa, enum cpa_op op);

#ifndef arch_cpa_enter
/**
 * arch_cpa_enter() - start the request of an operation
 *
 * Together with arch_cpa_leave() it brackets the request of set_memory_*() and
 * set_direct_map_*_noflush(), so that an architecture can batch the page table
 * updates of the request, for example in lazy MMU mode. The request may sleep.
 * A direct call of change_page_attr() is not bracketed.
 */
static inline void arch_cpa_enter(void)
{
}
#endif

#ifndef arch_cpa_leave
/**
 * arch_cpa_leave() - end the request of an operation
 */
static inline void arch_cpa_leave(void)
{
}
#endif

#endif /* _LINUX_PAGEATTR_H */

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGEATTR_H
#define _LINUX_PAGEATTR_H

#include <linux/mm.h>
#include <linux/pgtable.h>

/**
 * DOC: generic change_page_attr()
 *
 * change_page_attr() is the core of set_memory_*() and
 * set_direct_map_*_noflush() on the architectures that select
 * GENERIC_PAGEATTR. It changes the protection of the kernel mappings of a
 * range or an array of pages, and of their aliases.
 *
 * It walks the kernel page tables one step at a time. A large page keeps
 * its size when the whole page gets the same new protection, and is split
 * otherwise.
 *
 * The architecture provides the page table format, the aliases and the TLB
 * flush with the hooks below. The hooks that have a default are optional.
 *
 * The walk calls the hooks with cpa_lock held, and they must not sleep.
 * arch_cpa_process_alias() is the exception: it runs without cpa_lock and
 * may sleep to split the large pages of an alias.
 *
 * While the walk runs, @numpages and @pfn of struct cpa_data describe the
 * current step. arch_cpa_flush() and arch_cpa_collapse() run after the walk
 * and see the request.
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
 * @flags: CPA_REQ_* flags set by the caller
 * @state: CPA_ST_* flags set by the core
 * @pages: the pages to change with CPA_REQ_PAGES_ARRAY, @addr is ignored then
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
	struct page	**pages;
};

/**
 * struct cpa_leaf - a mapping found by the lookup
 * @kpte: the page table entry, or the first entry of a contiguous block
 * @level: the level of @kpte
 * @addr: the first virtual address that @kpte maps
 * @size: the size of the mapping: the size of @level, or of the contiguous
 *	block
 * @pfn: the first pfn that @kpte maps
 * @prot: the protection of the mapping in the architecture's PTE format, as
 *	decoded by arch_cpa_decode_leaf()
 *
 * A contiguous block is a run of adjacent entries that the architecture
 * treats as one mapping, like the entries with the contiguous bit on arm64
 * or a 16K or 512K page on powerpc 8xx.
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
#define CPA_REQ_PAGES_ARRAY	0x01
#define CPA_REQ_NO_CHECK_ALIAS	0x02 /* Do not search for aliases */
#define CPA_REQ_COLLAPSE	0x04 /* try to collapse large pages */
#define CPA_REQ_SPLIT		0x08 /* split large pages */

/* cpa_data::state */
#define CPA_ST_FLUSHTLB		0x01
#define CPA_ST_FLUSH_ALL	0x02
#define CPA_ST_ALIAS		0x04
#define CPA_ST_INIT_MM_LOCKED	0x08

#include <asm/pageattr.h>

static inline pgprot_t cpa_req_prot(struct cpa_data *cpa, struct cpa_leaf *leaf)
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

/*
 * The functions below are for the architecture implementation of
 * set_memory_*() and set_direct_map_*(); everything else uses those.
 */
unsigned long cpa_addr(struct cpa_data *cpa, unsigned long idx);
int cpa_process_fault(struct cpa_data *cpa);
int cpa_change_alias(struct cpa_data *cpa, struct cpa_data *alias);
void cpa_flush_tlb_kernel_range(struct cpa_data *cpa);

/**
 * change_page_attr() - change the protection of kernel mappings and flush the
 *	TLB
 * @cpa: the request
 *
 * Changes the page tables under the init_mm read lock and, with
 * CPA_REQ_COLLAPSE, collapses large pages after the flush.
 *
 * Return: 0 on success, -errno on failure.
 */
int change_page_attr(struct cpa_data *cpa);

/**
 * change_page_attr_noflush() - change the protection of kernel mappings
 * @cpa: the request
 *
 * The caller flushes the TLB. Unlike change_page_attr(), it does not take
 * the init_mm lock: every step looks up the address under cpa_lock, which a
 * collapse of large pages holds as well. It may sleep to allocate a page
 * table for a split.
 *
 * Return: 0 on success, -errno on failure.
 */
int change_page_attr_noflush(struct cpa_data *cpa);

void direct_map_count_update(enum pgtable_level level, long pages);
void direct_map_count_split(enum pgtable_level level);
void direct_map_count_collapse(enum pgtable_level level);
void cpa_report_meminfo(struct seq_file *m, unsigned long levels);

/**
 * arch_cpa_decode_leaf() - decode a page table entry
 * @leaf: the mapping to decode
 *
 * Fill @leaf->pfn and @leaf->prot from the entry @leaf->kpte at @leaf->level.
 * If the entry is a part of a contiguous block, extend @leaf to the block.
 */
void arch_cpa_decode_leaf(struct cpa_leaf *leaf);

/**
 * arch_cpa_modify_leaf() - change the protection of a mapping
 * @leaf: the mapping to change
 * @prot: the new protection
 *
 * Store the entry of @leaf, or every entry of its contiguous block, with
 * @prot without flushing the TLB.
 */
void arch_cpa_modify_leaf(struct cpa_leaf *leaf, pgprot_t prot);

/**
 * arch_cpa_flush() - flush the TLB for the pages of a request
 * @cpa: the request
 *
 * CPA_ST_FLUSH_ALL in @cpa->state means an alias of the pages changed as
 * well, or the request failed. An architecture that flushes the aliases in
 * arch_cpa_process_alias(), or whose page table updates flush the TLB by
 * themselves, may ignore it.
 */
void arch_cpa_flush(struct cpa_data *cpa);

/**
 * arch_cpa_new_prot() - compute the protection a request results in
 * @cpa: the request
 * @leaf: the mapping to change
 *
 * A large page is split when the result differs from cpa_req_prot().
 *
 * Return: the new protection of @leaf.
 */
#ifndef arch_cpa_new_prot
static inline pgprot_t arch_cpa_new_prot(struct cpa_data *cpa,
					 struct cpa_leaf *leaf)
{
	return cpa_req_prot(cpa, leaf);
}
#endif

/**
 * arch_cpa_split_leaf() - split a large page
 * @cpa: the request
 * @leaf: the large page
 * @table: the page table from arch_cpa_alloc_pgtable(), NULL for a
 *	contiguous block
 *
 * Replace @leaf with @table, filled with entries that map the same memory
 * with the same protection, and do the TLB maintenance the CPU requires
 * when a table replaces a leaf. A contiguous block is split by making each
 * of its entries a mapping of its own.
 *
 * Return: 0 on success, -errno on failure.
 */
#ifndef arch_cpa_split_leaf
static inline int arch_cpa_split_leaf(struct cpa_data *cpa,
				      struct cpa_leaf *leaf, void *table)
{
	return -EINVAL;
}

static inline bool arch_cpa_can_split(struct cpa_data *cpa,
				      struct cpa_leaf *leaf)
{
	return false;
}
#define arch_cpa_can_split arch_cpa_can_split
#endif

/**
 * arch_cpa_can_split() - check if a large page can be split
 * @cpa: the request
 * @leaf: the large page
 *
 * Called before the page table for the split is allocated. An architecture
 * without arch_cpa_split_leaf() cannot split at all.
 *
 * Return: true if arch_cpa_split_leaf() can split @leaf.
 */
#ifndef arch_cpa_can_split
static inline bool arch_cpa_can_split(struct cpa_data *cpa,
				      struct cpa_leaf *leaf)
{
	return true;
}
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
 * Called with init_mm write locked and cpa_lock held after a successful
 * CPA_REQ_COLLAPSE request was flushed.
 */
#ifndef arch_cpa_collapse
static inline void arch_cpa_collapse(struct cpa_data *cpa)
{
}
#endif

/* Operations of the generic set_memory_*() and set_direct_map_*_noflush() */
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
 * struct cpa_op_prot - the request of a set_memory() operation
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
 * arch_set_memory_prepare() - prepare a request for a set_memory() operation
 * @cpa: the request with the address and the number of pages
 * @op: the operation
 *
 * Fill the masks and the flags of @cpa for @op, usually with cpa_init_op()
 * from a table of the operations. A request with empty masks succeeds
 * without changing anything.
 *
 * Return: 0 on success, -errno to refuse the request.
 */
int arch_set_memory_prepare(struct cpa_data *cpa, enum cpa_op op);

#ifndef arch_set_memory_enter
/**
 * arch_set_memory_enter() - start a request of the generic entry points
 *
 * The generic set_memory_*() and set_direct_map_*_noflush() call it before
 * the request and arch_set_memory_leave() after it. The request between
 * them may sleep. An architecture that calls change_page_attr() itself
 * brackets the request on its own.
 */
static inline void arch_set_memory_enter(void)
{
}
#endif

#ifndef arch_set_memory_leave
/**
 * arch_set_memory_leave() - end a request of the generic entry points
 */
static inline void arch_set_memory_leave(void)
{
}
#endif

#endif /* _LINUX_PAGEATTR_H */

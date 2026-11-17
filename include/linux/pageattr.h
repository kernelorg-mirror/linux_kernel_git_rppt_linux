/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGEATTR_H
#define _LINUX_PAGEATTR_H

#include <linux/mm.h>
#include <linux/pgtable.h>

struct seq_file;

/*
 * The current flushing context - we pass it instead of 5 arguments:
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

int cpa_process_fault(struct cpa_data *cpa);
int cpa_change_alias(struct cpa_data *cpa, struct cpa_data *alias);
void cpa_flush_tlb_kernel_range(struct cpa_data *cpa);

int change_page_attr(struct cpa_data *cpa);

int change_page_attr_noflush(struct cpa_data *cpa);

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

void arch_cpa_decode_leaf(struct cpa_leaf *leaf);

void arch_cpa_modify_leaf(struct cpa_leaf *leaf, pgprot_t prot);

void arch_cpa_flush(struct cpa_data *cpa);

#ifndef arch_cpa_new_prot
static inline pgprot_t arch_cpa_new_prot(struct cpa_data *cpa,
					 struct cpa_leaf *leaf)
{
	return cpa_req_prot(cpa, leaf);
}
#endif

#ifndef arch_cpa_can_split
static inline bool arch_cpa_can_split(struct cpa_data *cpa,
				      struct cpa_leaf *leaf)
{
	return false;
}

static inline int arch_cpa_split_leaf(struct cpa_data *cpa,
				      struct cpa_leaf *leaf, void *table)
{
	return -EINVAL;
}
#endif

#ifndef arch_cpa_alloc_pgtable
void *arch_cpa_alloc_pgtable(struct cpa_leaf *leaf);

void arch_cpa_free_pgtable(struct cpa_leaf *leaf, void *table);
#endif

#ifndef cpa_is_direct_map
static inline bool cpa_is_direct_map(unsigned long addr)
{
	return addr >= PAGE_OFFSET && addr < (unsigned long)high_memory;
}
#endif

#ifndef arch_cpa_process_fault
static inline int arch_cpa_process_fault(struct cpa_data *cpa)
{
	return cpa_process_fault(cpa);
}
#endif

#ifndef arch_cpa_process_alias
static inline int arch_cpa_process_alias(struct cpa_data *cpa)
{
	return 0;
}
#endif

#ifndef arch_cpa_collapse
static inline void arch_cpa_collapse(struct cpa_data *cpa)
{
}
#endif

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

int arch_cpa_prepare(struct cpa_data *cpa, enum cpa_op op);

#endif /* _LINUX_PAGEATTR_H */

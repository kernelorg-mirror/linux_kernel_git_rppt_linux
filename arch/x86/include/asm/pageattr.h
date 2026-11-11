/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_PAGEATTR_H
#define _ASM_X86_PAGEATTR_H

#ifdef CONFIG_PROC_FS
void direct_map_count_update(enum pgtable_level level, long pages);
#else
static inline void direct_map_count_update(enum pgtable_level level,
					   long pages)
{
}
#endif

static inline bool cpa_is_direct_map(unsigned long addr)
{
	return addr >= PAGE_OFFSET &&
	       addr < PAGE_OFFSET + (max_pfn_mapped << PAGE_SHIFT);
}
#define cpa_is_direct_map cpa_is_direct_map

pgprot_t arch_cpa_new_prot(struct cpa_data *cpa, struct cpa_leaf *leaf);
#define arch_cpa_new_prot arch_cpa_new_prot

int arch_cpa_split_leaf(struct cpa_data *cpa, struct cpa_leaf *leaf,
			void *table);
#define arch_cpa_split_leaf arch_cpa_split_leaf

int arch_cpa_process_fault(struct cpa_data *cpa);
#define arch_cpa_process_fault arch_cpa_process_fault

int arch_cpa_process_alias(struct cpa_data *cpa);
#define arch_cpa_process_alias arch_cpa_process_alias

void arch_cpa_collapse(struct cpa_data *cpa);
#define arch_cpa_collapse arch_cpa_collapse

#endif /* _ASM_X86_PAGEATTR_H */

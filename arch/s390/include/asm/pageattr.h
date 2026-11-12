/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_S390_PAGEATTR_H
#define _ASM_S390_PAGEATTR_H

void *arch_cpa_alloc_pgtable(struct cpa_data *cpa, struct cpa_leaf *leaf);
void arch_cpa_free_pgtable(struct cpa_leaf *leaf, void *table);
#define arch_cpa_alloc_pgtable arch_cpa_alloc_pgtable

int arch_cpa_split_leaf(struct cpa_data *cpa, struct cpa_leaf *leaf,
			void *table);
#define arch_cpa_split_leaf arch_cpa_split_leaf

int arch_cpa_process_alias(struct cpa_data *cpa);
#define arch_cpa_process_alias arch_cpa_process_alias

#endif /* _ASM_S390_PAGEATTR_H */

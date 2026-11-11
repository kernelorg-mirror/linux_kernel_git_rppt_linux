/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_RISCV_PAGEATTR_H
#define _ASM_RISCV_PAGEATTR_H

#ifdef CONFIG_64BIT
#define cpa_is_direct_map(addr)	is_linear_mapping(addr)

int arch_cpa_split_leaf(struct cpa_data *cpa, struct cpa_leaf *leaf,
			void *table);
#define arch_cpa_split_leaf arch_cpa_split_leaf

int arch_cpa_process_alias(struct cpa_data *cpa);
#define arch_cpa_process_alias arch_cpa_process_alias
#endif

#endif /* _ASM_RISCV_PAGEATTR_H */

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_PAGEATTR_H
#define __ASM_PAGEATTR_H

#define cpa_is_direct_map(addr)	__is_lm_address(__tag_reset(addr))

int arch_cpa_split_leaf(struct cpa_data *cpa, struct cpa_leaf *leaf,
			void *table);
bool arch_cpa_can_split(struct cpa_data *cpa, struct cpa_leaf *leaf);
#define arch_cpa_can_split arch_cpa_can_split

int arch_cpa_process_alias(struct cpa_data *cpa);
#define arch_cpa_process_alias arch_cpa_process_alias

void arch_cpa_enter(void);
#define arch_cpa_enter arch_cpa_enter

void arch_cpa_leave(void);
#define arch_cpa_leave arch_cpa_leave

int arm64_change_page_attr(struct cpa_data *cpa);

#endif /* __ASM_PAGEATTR_H */

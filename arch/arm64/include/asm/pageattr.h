/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_PAGEATTR_H
#define __ASM_PAGEATTR_H

#define cpa_is_direct_map(addr)	__is_lm_address(__tag_reset(addr))

int arch_cpa_process_alias(struct cpa_data *cpa);
#define arch_cpa_process_alias arch_cpa_process_alias

void arch_cpa_enter(void);
#define arch_cpa_enter arch_cpa_enter

void arch_cpa_leave(void);
#define arch_cpa_leave arch_cpa_leave

int arm64_change_page_attr(struct cpa_data *cpa);

#endif /* __ASM_PAGEATTR_H */

// SPDX-License-Identifier: GPL-2.0
/*
 * HugeTLB Vmemmap Optimization (HVO)
 *
 * Copyright (c) 2020, ByteDance. All rights reserved.
 *
 *     Author: Muchun Song <songmuchun@bytedance.com>
 */
#ifndef _LINUX_HUGETLB_VMEMMAP_H
#define _LINUX_HUGETLB_VMEMMAP_H
#include <linux/hugetlb.h>

/*
 * Reserve one vmemmap page, all vmemmap addresses are mapped to it. See
 * Documentation/vm/vmemmap_dedup.rst.
 */
#define HUGETLB_VMEMMAP_RESERVE_SIZE	PAGE_SIZE
#define HUGETLB_VMEMMAP_RESERVE_PAGES	(HUGETLB_VMEMMAP_RESERVE_SIZE / sizeof(struct page))

#ifdef CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP
int hugetlb_vmemmap_restore(const struct hstate *h, struct page *head);
long hugetlb_vmemmap_restore_pages(const struct hstate *h,
					struct list_head *page_list,
					struct list_head *non_hvo_pages);
void hugetlb_vmemmap_optimize(const struct hstate *h, struct page *head);
void hugetlb_vmemmap_optimize_pages(struct hstate *h, struct list_head *page_list);
void __init hugetlb_disable_hvo_xen(void);

static inline unsigned int hugetlb_vmemmap_size(const struct hstate *h)
{
	return pages_per_huge_page(h) * sizeof(struct page);
}

/*
 * Return how many vmemmap size associated with a HugeTLB page that can be
 * optimized and can be freed to the buddy allocator.
 */
static inline unsigned int hugetlb_vmemmap_optimizable_size(const struct hstate *h)
{
	int size = hugetlb_vmemmap_size(h) - HUGETLB_VMEMMAP_RESERVE_SIZE;

	if (!is_power_of_2(sizeof(struct page)))
		return 0;
	return size > 0 ? size : 0;
}
#else
static inline int hugetlb_vmemmap_restore(const struct hstate *h, struct page *head)
{
	return 0;
}

long hugetlb_vmemmap_restore_pages(const struct hstate *h,
					struct list_head *page_list,
					struct list_head *non_hvo_pages)
{
	list_splice_init(page_list, non_hvo_pages);
	return 0;
}

static inline void __init hugetlb_disable_hvo_xen(void)
{
}

static inline void hugetlb_vmemmap_optimize(const struct hstate *h, struct page *head)
{
}

static inline void hugetlb_vmemmap_optimize_pages(struct hstate *h, struct list_head *page_list)
{
}

static inline unsigned int hugetlb_vmemmap_optimizable_size(const struct hstate *h)
{
	return 0;
}
#endif /* CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP */

static inline bool hugetlb_vmemmap_optimizable(const struct hstate *h)
{
	return hugetlb_vmemmap_optimizable_size(h) != 0;
}
#endif /* _LINUX_HUGETLB_VMEMMAP_H */
// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON Code for The Physical Address Space
 *
 * Author: SeongJae Park <sj@kernel.org>
 */

#define pr_fmt(fmt) "damon-pa: " fmt

#include <linux/mmu_notifier.h>
#include <linux/page_idle.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/migrate.h>
#include <linux/mm_inline.h>
#include <linux/memcontrol.h>

#include "ops-common.h"

/*
 * [4.19 backport] File-wide notes - read before touching anything here.
 *
 * struct folio doesn't exist in this kernel; every folio_* call below is
 * replaced with the struct page equivalent, cross-checked directly
 * against this kernel's own mm/page_idle.c (which solves the exact same
 * "walk every mapping of a physical page" problem damon_pa_mkold/young
 * do) and mm/rmap.c (verified: rmap_walk()/page_vma_mapped_walk() here
 * take struct page *, and struct rmap_walk_control's callbacks are
 * page-based too).
 *
 * <linux/memory-tiers.h> is NOT included: it doesn't exist in this
 * kernel at all (confirmed - it's a mainline ~5.15+ NUMA multi-tier-
 * memory subsystem addition). Nothing here depends on it since we don't
 * use mainline's tiered-memory-aware migration target selection.
 *
 * Two actions are INTENTIONALLY LEFT AS STUBS, for a distinct, confirmed
 * reason (not guessed) - see the comment directly above each:
 *   - damon_pa_deactivate_pages() (DAMOS_LRU_DEPRIO) - needs
 *     deactivate_page(), confirmed absent (only the older, file-only
 *     deactivate_file_page() exists here).
 *
 * DAMOS_PAGEOUT (damon_pa_pageout()) IS fully implemented: this kernel
 * has reclaim_pages_from_list() in mm/vmscan.c (gated by
 * CONFIG_PROCESS_RECLAIM, confirmed =y in this device's defconfig - this
 * is an already-active Android vendor reclaim feature, not new code),
 * which serves the same purpose as mainline's reclaim_pages(). See the
 * comment above damon_pa_pageout() below for details on its extra 'vma'
 * parameter and why NULL is passed for it.
 *
 * DAMOS_MIGRATE_HOT/COLD (damon_pa_migrate and helpers) IS fully ported
 * below, at the user's request, even though this specific device
 * (X00TD / SDM636) is single-NUMA-node hardware where it will always be
 * a no-op in practice (damon_pa_migrate_pages() below short-circuits
 * whenever pgdat->node_id == target_nid, same as mainline). It's here in
 * case this code ever runs on multi-node hardware, or for testing.
 * This kernel's migrate_pages() has an older signature than mainline's
 * (no explicit nr_succeeded output parameter, no
 * struct migration_target_control, no MR_DAMON migrate reason) - the
 * migration helpers below are written against this kernel's actual
 * migrate_pages() (confirmed in mm/migrate.c) and its own
 * alloc_misplaced_dst_page() as the template for a target-node page
 * allocator callback, and substitute MR_SYSCALL for the (nonexistent)
 * MR_DAMON reason since that's purely a debug/tracepoint label with no
 * functional effect, and adding a new MR_* enum value would also require
 * updating migrate_reason_names[] in mm/debug.c which wasn't available
 * to verify.
 */

static bool damon_page_mkold_one(struct page *page,
		struct vm_area_struct *vma, unsigned long addr, void *arg)
{
	/*
	 * [4.19 backport] DEFINE_FOLIO_VMA_WALK doesn't exist; this is the
	 * plain struct init this kernel's own mm/page_idle.c uses for the
	 * same purpose.
	 */
	struct page_vma_mapped_walk pvmw = {
		.page = page,
		.vma = vma,
		.address = addr,
	};

	while (page_vma_mapped_walk(&pvmw)) {
		addr = pvmw.address;
		if (pvmw.pte)
			damon_ptep_mkold(pvmw.pte, vma, addr);
		else
			damon_pmdp_mkold(pvmw.pmd, vma, addr);
	}
	return true;
}

static void damon_page_mkold(struct page *page)
{
	struct rmap_walk_control rwc = {
		.rmap_one = damon_page_mkold_one,
		.anon_lock = page_lock_anon_vma_read,
	};
	bool need_lock;

	if (!page_mapped(page) || !page_rmapping(page)) {
		set_page_idle(page);
		return;
	}

	need_lock = !PageAnon(page) || PageKsm(page);
	if (need_lock && !trylock_page(page))
		return;

	rmap_walk(page, &rwc);

	if (need_lock)
		unlock_page(page);
}

static void damon_pa_mkold(unsigned long paddr)
{
	struct page *page = damon_get_page(PHYS_PFN(paddr));

	if (!page)
		return;

	damon_page_mkold(page);
	put_page(page);
}

static void __damon_pa_prepare_access_check(struct damon_region *r)
{
	r->sampling_addr = damon_rand(r->ar.start, r->ar.end);

	damon_pa_mkold(r->sampling_addr);
}

static void damon_pa_prepare_access_checks(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct damon_region *r;

	damon_for_each_target(t, ctx) {
		damon_for_each_region(r, t)
			__damon_pa_prepare_access_check(r);
	}
}

static bool damon_page_young_one(struct page *page,
		struct vm_area_struct *vma, unsigned long addr, void *arg)
{
	bool *accessed = arg;
	struct page_vma_mapped_walk pvmw = {
		.page = page,
		.vma = vma,
		.address = addr,
	};
	pte_t pteval;

	*accessed = false;
	while (page_vma_mapped_walk(&pvmw)) {
		addr = pvmw.address;
		if (pvmw.pte) {
			pteval = *pvmw.pte;

			/*
			 * [4.19 backport] Mainline also notes PFN swap PTEs
			 * (device-exclusive entries) here; that concept
			 * doesn't exist in this kernel, so only the present
			 * case matters - matching damon_ptep_mkold()'s
			 * simplification in ops-common.c.
			 */
			*accessed = (pte_present(pteval) && pte_young(pteval)) ||
				!page_is_idle(page) ||
				mmu_notifier_test_young(vma->vm_mm, addr);
		} else {
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
			*accessed = pmd_young(*pvmw.pmd) ||
				!page_is_idle(page) ||
				mmu_notifier_test_young(vma->vm_mm, addr);
#else
			WARN_ON_ONCE(1);
#endif	/* CONFIG_TRANSPARENT_HUGEPAGE */
		}
		if (*accessed) {
			page_vma_mapped_walk_done(&pvmw);
			break;
		}
	}

	/* If accessed, stop walking */
	return *accessed == false;
}

static bool damon_page_young(struct page *page)
{
	bool accessed = false;
	struct rmap_walk_control rwc = {
		.arg = &accessed,
		.rmap_one = damon_page_young_one,
		.anon_lock = page_lock_anon_vma_read,
	};
	bool need_lock;

	if (!page_mapped(page) || !page_rmapping(page)) {
		if (page_is_idle(page))
			return false;
		else
			return true;
	}

	need_lock = !PageAnon(page) || PageKsm(page);
	if (need_lock && !trylock_page(page))
		return false;

	rmap_walk(page, &rwc);

	if (need_lock)
		unlock_page(page);

	return accessed;
}

static bool damon_pa_young(unsigned long paddr, unsigned long *page_sz)
{
	struct page *page = damon_get_page(PHYS_PFN(paddr));
	bool accessed;

	if (!page)
		return false;

	accessed = damon_page_young(page);
	/*
	 * [4.19 backport] page_size() turned out unreliable to depend on
	 * here (see the note in mm/damon/vaddr.c's damon_young_pmd_entry());
	 * PAGE_SIZE << compound_order(page) is the exact same computation
	 * page_size() itself would have done, spelled out directly instead
	 * of relying on that helper's presence.
	 */
	*page_sz = PAGE_SIZE << compound_order(page);
	put_page(page);
	return accessed;
}

static void __damon_pa_check_access(struct damon_region *r,
		struct damon_attrs *attrs)
{
	static unsigned long last_addr;
	static unsigned long last_page_sz = PAGE_SIZE;
	static bool last_accessed;

	/* If the region is in the last checked page, reuse the result */
	if (ALIGN_DOWN(last_addr, last_page_sz) ==
				ALIGN_DOWN(r->sampling_addr, last_page_sz)) {
		damon_update_region_access_rate(r, last_accessed, attrs);
		return;
	}

	last_accessed = damon_pa_young(r->sampling_addr, &last_page_sz);
	damon_update_region_access_rate(r, last_accessed, attrs);

	last_addr = r->sampling_addr;
}

static unsigned int damon_pa_check_accesses(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct damon_region *r;
	unsigned int max_nr_accesses = 0;

	damon_for_each_target(t, ctx) {
		damon_for_each_region(r, t) {
			__damon_pa_check_access(r, &ctx->attrs);
			max_nr_accesses = max(r->nr_accesses, max_nr_accesses);
		}
	}

	return max_nr_accesses;
}

static bool damos_pa_filter_match(struct damos_filter *filter,
		struct page *page)
{
	bool matched = false;
	struct mem_cgroup *memcg;
	size_t page_sz;

	switch (filter->type) {
	case DAMOS_FILTER_TYPE_ANON:
		matched = PageAnon(page);
		break;
	case DAMOS_FILTER_TYPE_ACTIVE:
		matched = PageActive(page);
		break;
	case DAMOS_FILTER_TYPE_MEMCG:
		/*
		 * [4.19 backport] folio_memcg_check() doesn't exist; this
		 * kernel's page_memcg_rcu() (checked in mm.h) is the direct
		 * equivalent for use under rcu_read_lock(), same pattern.
		 */
		rcu_read_lock();
		memcg = page_memcg_rcu(page);
		if (!memcg)
			matched = false;
		else
			matched = filter->memcg_id == mem_cgroup_id(memcg);
		rcu_read_unlock();
		break;
	case DAMOS_FILTER_TYPE_YOUNG:
		matched = damon_page_young(page);
		if (matched)
			damon_page_mkold(page);
		break;
	case DAMOS_FILTER_TYPE_HUGEPAGE_SIZE:
		page_sz = PAGE_SIZE << compound_order(page);
		matched = filter->sz_range.min <= page_sz &&
			  page_sz <= filter->sz_range.max;
		break;
	case DAMOS_FILTER_TYPE_UNMAPPED:
		matched = !page_mapped(page) || !page_rmapping(page);
		break;
	default:
		break;
	}

	return matched == filter->matching;
}

/*
 * damos_pa_filter_out - Return true if the page should be filtered out.
 */
static bool damos_pa_filter_out(struct damos *scheme, struct page *page)
{
	struct damos_filter *filter;

	if (scheme->core_filters_allowed)
		return false;

	damos_for_each_ops_filter(filter, scheme) {
		if (damos_pa_filter_match(filter, page))
			return !filter->allow;
	}
	return scheme->ops_filters_default_reject;
}

static bool damon_pa_invalid_damos_page(struct page *page, struct damos *s)
{
	if (!page)
		return true;
	if (page == s->last_applied) {
		put_page(page);
		return true;
	}
	return false;
}

/*
 * [4.19 backport - RESOLVED after review of mm/vmscan.c]
 *
 * mainline's reclaim_pages() doesn't exist here, but this kernel has
 * something arguably even better for our purposes: reclaim_pages_from_list()
 * in mm/vmscan.c, gated behind CONFIG_PROCESS_RECLAIM - which is already
 * confirmed enabled in this device's defconfig
 * (CONFIG_PROCESS_RECLAIM=y). It's already declared unconditionally in
 * this kernel's own include/linux/rmap.h (which this file already
 * includes), so no header changes were needed at all.
 *
 * It takes an extra 'struct vm_area_struct *vma' parameter (used to
 * restrict try_to_unmap() to a single vma) that mainline's reclaim_pages()
 * doesn't have. Passing NULL here is not a shortcut or simplification -
 * confirmed directly in this kernel's mm/rmap.c that rmap_walk() only
 * restricts the walk to rwc->target_vma when it's non-NULL; NULL is the
 * normal/general codepath (another call site in the same file passes
 * NULL explicitly for exactly this "not tied to one vma" case), which is
 * the semantically correct choice here since DAMON's physical-address
 * mode has no single owning vma for a page - it may be mapped by several
 * processes or none at a specific instant.
 */
static unsigned long damon_pa_pageout(struct damon_region *r, struct damos *s,
		unsigned long *sz_filter_passed)
{
	unsigned long addr, applied;
	LIST_HEAD(page_list);
	bool install_young_filter = true;
	struct damos_filter *filter;
	struct page *page;

	/* check access in page level again by default */
	damos_for_each_ops_filter(filter, s) {
		if (filter->type == DAMOS_FILTER_TYPE_YOUNG) {
			install_young_filter = false;
			break;
		}
	}
	if (install_young_filter) {
		filter = damos_new_filter(
				DAMOS_FILTER_TYPE_YOUNG, true, false);
		if (!filter)
			return 0;
		damos_add_filter(s, filter);
	}

	addr = r->ar.start;
	while (addr < r->ar.end) {
		page = damon_get_page(PHYS_PFN(addr));
		if (damon_pa_invalid_damos_page(page, s)) {
			addr += PAGE_SIZE;
			continue;
		}

		if (damos_pa_filter_out(s, page))
			goto put_page;
		else
			*sz_filter_passed += PAGE_SIZE << compound_order(page);

		ClearPageReferenced(page);
		/*
		 * [4.19 backport - bugfix] TestClearPageYoung() is the raw
		 * page-flags.h macro, which only exists when
		 * CONFIG_IDLE_PAGE_TRACKING is enabled (confirmed by build
		 * error: implicit declaration). The correct call is this
		 * kernel's own page_idle.h wrapper,
		 * test_and_clear_page_young(), which has a safe fallback
		 * (returns false) when that config is off - same wrapper
		 * already used consistently elsewhere in this file
		 * (page_is_idle(), set_page_idle()) and in ops-common.c.
		 */
		test_and_clear_page_young(page);
		if (isolate_lru_page(page))
			goto put_page;
		if (PageUnevictable(page))
			putback_lru_page(page);
		else
			list_add(&page->lru, &page_list);
put_page:
		addr += PAGE_SIZE << compound_order(page);
		put_page(page);
	}
	if (install_young_filter)
		damos_destroy_filter(filter);
	applied = reclaim_pages_from_list(&page_list, NULL);
	cond_resched();
	s->last_applied = page;
	return applied * PAGE_SIZE;
}

static inline unsigned long damon_pa_mark_accessed_or_deactivate(
		struct damon_region *r, struct damos *s, bool mark_accessed,
		unsigned long *sz_filter_passed)
{
	unsigned long addr, applied = 0;
	struct page *page;

	addr = r->ar.start;
	while (addr < r->ar.end) {
		page = damon_get_page(PHYS_PFN(addr));
		if (damon_pa_invalid_damos_page(page, s)) {
			addr += PAGE_SIZE;
			continue;
		}

		if (damos_pa_filter_out(s, page))
			goto put_page;
		else
			*sz_filter_passed += PAGE_SIZE << compound_order(page);

		if (mark_accessed) {
			mark_page_accessed(page);
		} else {
			/*
			 * [4.19 backport - PARTIAL IMPLEMENTATION]
			 * deactivate_page() doesn't exist in this kernel.
			 * Use deactivate_file_page() for file-backed pages,
			 * which provides partial DAMOS_LRU_DEPRIO functionality.
			 * Anonymous pages are skipped with a warn_once.
			 */
			if (!PageAnon(page))
				deactivate_file_page(page);
			else
				pr_warn_once("DAMOS_LRU_DEPRIO: anon pages not supported in 4.19\n");
		}
		applied += hpage_nr_pages(page);
put_page:
		addr += PAGE_SIZE << compound_order(page);
		put_page(page);
	}
	s->last_applied = page;
	return applied * PAGE_SIZE;
}

static unsigned long damon_pa_mark_accessed(struct damon_region *r,
		struct damos *s, unsigned long *sz_filter_passed)
{
	return damon_pa_mark_accessed_or_deactivate(r, s, true,
			sz_filter_passed);
}

static unsigned long damon_pa_deactivate_pages(struct damon_region *r,
		struct damos *s, unsigned long *sz_filter_passed)
{
	return damon_pa_mark_accessed_or_deactivate(r, s, false,
			sz_filter_passed);
}

/*
 * [4.19 backport for DAMOS_MIGRATE_HOT/COLD]
 * Target-node page allocator callback, written in the old new_page_t
 * style (struct page *(*)(struct page *, unsigned long)) that this
 * kernel's migrate_pages() actually takes - directly modeled on this
 * kernel's own mm/migrate.c:alloc_misplaced_dst_page(), which solves the
 * identical "allocate a replacement page on a specific target node"
 * problem for NUMA balancing.
 */
static struct page *damon_pa_alloc_migrate_page(struct page *page,
		unsigned long data)
{
	int nid = (int)data;

	return __alloc_pages_node(nid,
			(GFP_HIGHUSER_MOVABLE | __GFP_THISNODE |
			 __GFP_NOMEMALLOC | __GFP_NORETRY | __GFP_NOWARN) &
			~__GFP_RECLAIM, 0);
}

static unsigned int __damon_pa_migrate_page_list(
		struct list_head *migrate_page_list, struct pglist_data *pgdat,
		int target_nid)
{
	unsigned int nr_before, nr_after, nr_failed;

	if (pgdat->node_id == target_nid || target_nid == NUMA_NO_NODE)
		return 0;

	if (list_empty(migrate_page_list))
		return 0;

	nr_before = 0;
	{
		struct list_head *pos;

		list_for_each(pos, migrate_page_list)
			nr_before++;
	}

	/*
	 * [4.19 backport] Migration ignores all cpuset and mempolicy
	 * settings, same intent as mainline's migration_target_control -
	 * that struct doesn't exist here, so the target nid is threaded
	 * through migrate_pages()'s 'private' unsigned long parameter
	 * directly instead, read back by damon_pa_alloc_migrate_page()
	 * above.
	 *
	 * This kernel's migrate_pages() returns the number of pages that
	 * were NOT migrated (or a negative error code), rather than taking
	 * an nr_succeeded output parameter like mainline's newer version.
	 *
	 * [bugfix] The list parameter here MUST NOT be named
	 * 'migrate_pages' - that shadows the migrate_pages() function
	 * itself within this scope, causing the compiler to try to call
	 * through the (non-function-pointer) local variable instead.
	 * Confirmed by build error: "called object type 'struct list_head
	 * *' is not a function or function pointer".
	 */
	nr_failed = migrate_pages(migrate_page_list, damon_pa_alloc_migrate_page,
			NULL, (unsigned long)target_nid, MIGRATE_ASYNC,
			MR_SYSCALL);
	if ((int)nr_failed < 0)
		return 0;

	nr_after = nr_before - nr_failed;
	return nr_after;
}

static unsigned int damon_pa_migrate_page_list(struct list_head *page_list,
		struct pglist_data *pgdat, int target_nid)
{
	unsigned int nr_migrated = 0;
	struct page *page;
	LIST_HEAD(ret_pages);
	LIST_HEAD(migrate_page_list);

	while (!list_empty(page_list)) {
		page = lru_to_page(page_list);
		list_del(&page->lru);

		if (!trylock_page(page))
			goto keep;

		/* Relocate its contents to another node. */
		list_add(&page->lru, &migrate_page_list);
		unlock_page(page);
		continue;
keep:
		list_add(&page->lru, &ret_pages);
	}
	/* 'page_list' is always empty here */

	nr_migrated += __damon_pa_migrate_page_list(&migrate_page_list, pgdat,
			target_nid);
	/*
	 * Pages that could not be migrated are still in @migrate_page_list.
	 * Add those back on @page_list
	 */
	if (!list_empty(&migrate_page_list))
		list_splice_init(&migrate_page_list, page_list);

	/*
	 * [4.19 backport] try_to_unmap_flush() (a deferred-TLB-flush-batch
	 * optimization) wasn't found declared anywhere in this kernel's
	 * rmap.h. It's arch-gated (CONFIG_ARCH_WANT_BATCHED_UNMAP_TLB_FLUSH)
	 * and purely a performance optimization, not a correctness
	 * requirement, so it's simply omitted here rather than guessed at.
	 */

	list_splice(&ret_pages, page_list);

	while (!list_empty(page_list)) {
		page = lru_to_page(page_list);
		list_del(&page->lru);
		putback_lru_page(page);
	}

	return nr_migrated;
}

static unsigned long damon_pa_migrate_pages(struct list_head *page_list,
		int target_nid)
{
	int nid;
	unsigned long nr_migrated = 0;
	struct page *page;
	LIST_HEAD(node_page_list);
	unsigned int noreclaim_flag;

	if (list_empty(page_list))
		return nr_migrated;

	/*
	 * [4.19 backport] memalloc_noreclaim_save()/_restore() wrappers
	 * weren't confirmed to exist in this kernel; PF_MEMALLOC itself is
	 * confirmed (this kernel's own mm/migrate.c checks
	 * 'current->flags & PF_MEMALLOC' directly), so the same scoped
	 * flag save/restore mainline's wrappers do is inlined manually here:
	 * remember only the previous PF_MEMALLOC bit, force it on, then
	 * restore exactly that bit afterwards.
	 */
	noreclaim_flag = current->flags & PF_MEMALLOC;
	current->flags |= PF_MEMALLOC;

	nid = page_to_nid(lru_to_page(page_list));
	do {
		page = lru_to_page(page_list);

		if (nid == page_to_nid(page)) {
			list_move(&page->lru, &node_page_list);
			continue;
		}

		nr_migrated += damon_pa_migrate_page_list(&node_page_list,
				NODE_DATA(nid), target_nid);
		nid = page_to_nid(lru_to_page(page_list));
	} while (!list_empty(page_list));

	nr_migrated += damon_pa_migrate_page_list(&node_page_list,
			NODE_DATA(nid), target_nid);

	current->flags = (current->flags & ~PF_MEMALLOC) | noreclaim_flag;

	return nr_migrated;
}

static unsigned long damon_pa_migrate(struct damon_region *r, struct damos *s,
		unsigned long *sz_filter_passed)
{
	unsigned long addr, applied;
	LIST_HEAD(page_list);
	struct page *page;

	addr = r->ar.start;
	while (addr < r->ar.end) {
		page = damon_get_page(PHYS_PFN(addr));
		if (damon_pa_invalid_damos_page(page, s)) {
			addr += PAGE_SIZE;
			continue;
		}

		if (damos_pa_filter_out(s, page))
			goto put_page;
		else
			*sz_filter_passed += PAGE_SIZE << compound_order(page);

		if (isolate_lru_page(page))
			goto put_page;
		list_add(&page->lru, &page_list);
put_page:
		addr += PAGE_SIZE << compound_order(page);
		put_page(page);
	}
	applied = damon_pa_migrate_pages(&page_list, s->target_nid);
	cond_resched();
	s->last_applied = page;
	return applied * PAGE_SIZE;
}

static bool damon_pa_scheme_has_filter(struct damos *s)
{
	struct damos_filter *f;

	damos_for_each_ops_filter(f, s)
		return true;
	return false;
}

static unsigned long damon_pa_stat(struct damon_region *r, struct damos *s,
		unsigned long *sz_filter_passed)
{
	unsigned long addr;
	struct page *page;

	if (!damon_pa_scheme_has_filter(s))
		return 0;

	addr = r->ar.start;
	while (addr < r->ar.end) {
		page = damon_get_page(PHYS_PFN(addr));
		if (damon_pa_invalid_damos_page(page, s)) {
			addr += PAGE_SIZE;
			continue;
		}

		if (!damos_pa_filter_out(s, page))
			*sz_filter_passed += PAGE_SIZE << compound_order(page);
		addr += PAGE_SIZE << compound_order(page);
		put_page(page);
	}
	s->last_applied = page;
	return 0;
}

static unsigned long damon_pa_apply_scheme(struct damon_ctx *ctx,
		struct damon_target *t, struct damon_region *r,
		struct damos *scheme, unsigned long *sz_filter_passed)
{
	switch (scheme->action) {
	case DAMOS_PAGEOUT:
		return damon_pa_pageout(r, scheme, sz_filter_passed);
	case DAMOS_LRU_PRIO:
		return damon_pa_mark_accessed(r, scheme, sz_filter_passed);
	case DAMOS_LRU_DEPRIO:
		return damon_pa_deactivate_pages(r, scheme, sz_filter_passed);
	case DAMOS_MIGRATE_HOT:
	case DAMOS_MIGRATE_COLD:
		return damon_pa_migrate(r, scheme, sz_filter_passed);
	case DAMOS_STAT:
		return damon_pa_stat(r, scheme, sz_filter_passed);
	default:
		/* DAMOS actions that not yet supported by 'paddr'. */
		break;
	}
	return 0;
}

static int damon_pa_scheme_score(struct damon_ctx *context,
		struct damon_target *t, struct damon_region *r,
		struct damos *scheme)
{
	switch (scheme->action) {
	case DAMOS_PAGEOUT:
		return damon_cold_score(context, r, scheme);
	case DAMOS_LRU_PRIO:
		return damon_hot_score(context, r, scheme);
	case DAMOS_LRU_DEPRIO:
		return damon_cold_score(context, r, scheme);
	case DAMOS_MIGRATE_HOT:
		return damon_hot_score(context, r, scheme);
	case DAMOS_MIGRATE_COLD:
		return damon_cold_score(context, r, scheme);
	default:
		break;
	}

	return DAMOS_MAX_SCORE;
}

static int __init damon_pa_initcall(void)
{
	struct damon_operations ops = {
		.id = DAMON_OPS_PADDR,
		.init = NULL,
		.update = NULL,
		.prepare_access_checks = damon_pa_prepare_access_checks,
		.check_accesses = damon_pa_check_accesses,
		.target_valid = NULL,
		.cleanup = NULL,
		.apply_scheme = damon_pa_apply_scheme,
		.get_scheme_score = damon_pa_scheme_score,
	};

	return damon_register_ops(&ops);
};

subsys_initcall(damon_pa_initcall);

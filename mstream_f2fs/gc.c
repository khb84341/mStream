/*
 * fs/f2fs/gc.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/init.h>
#include <linux/f2fs_fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/freezer.h>

#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "gc.h"
#include <trace/events/f2fs.h>

static int gc_thread_func(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct f2fs_gc_kthread *gc_th = sbi->gc_thread;
	wait_queue_head_t *wq = &sbi->gc_thread->gc_wait_queue_head;
	long wait_ms;

	wait_ms = gc_th->min_sleep_time;

	set_freezable();
	do {
		wait_event_interruptible_timeout(*wq,
				kthread_should_stop() || freezing(current),
				msecs_to_jiffies(wait_ms));

		if (try_to_freeze())
			continue;
		if (kthread_should_stop())
			break;

		if (sbi->sb->s_writers.frozen >= SB_FREEZE_WRITE) {
			increase_sleep_time(gc_th, &wait_ms);
			continue;
		}

#ifdef CONFIG_F2FS_FAULT_INJECTION
		if (time_to_inject(sbi, FAULT_CHECKPOINT)) {
			f2fs_show_injection_info(FAULT_CHECKPOINT);
			f2fs_stop_checkpoint(sbi, false);
		}
#endif

		/*
		 * [GC triggering condition]
		 * 0. GC is not conducted currently.
		 * 1. There are enough dirty segments.
		 * 2. IO subsystem is idle by checking the # of writeback pages.
		 * 3. IO subsystem is idle by checking the # of requests in
		 *    bdev's request list.
		 *
		 * Note) We have to avoid triggering GCs frequently.
		 * Because it is possible that some segments can be
		 * invalidated soon after by user update or deletion.
		 * So, I'd like to wait some time to collect dirty segments.
		 */
		if (!mutex_trylock(&sbi->gc_mutex))
			continue;

		if (!is_idle(sbi)) {
			increase_sleep_time(gc_th, &wait_ms);
			mutex_unlock(&sbi->gc_mutex);
			continue;
		}

		if (has_enough_invalid_blocks(sbi))
			decrease_sleep_time(gc_th, &wait_ms);
		else
			increase_sleep_time(gc_th, &wait_ms);

		stat_inc_bggc_count(sbi);

		/* if return value is not zero, no victim was selected */
		if (f2fs_gc(sbi, test_opt(sbi, FORCE_FG_GC), true, NULL_SEGNO))
			wait_ms = gc_th->no_gc_sleep_time;

		trace_f2fs_background_gc(sbi->sb, wait_ms,
				prefree_segments(sbi), free_segments(sbi));

		/* balancing f2fs's metadata periodically */
		f2fs_balance_fs_bg(sbi);

	} while (!kthread_should_stop());
	return 0;
}

int start_gc_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_gc_kthread *gc_th;
	dev_t dev = sbi->sb->s_bdev->bd_dev;
	int err = 0;

	gc_th = f2fs_kmalloc(sbi, sizeof(struct f2fs_gc_kthread), GFP_KERNEL);
	if (!gc_th) {
		err = -ENOMEM;
		goto out;
	}

	gc_th->min_sleep_time = DEF_GC_THREAD_MIN_SLEEP_TIME;
	gc_th->max_sleep_time = DEF_GC_THREAD_MAX_SLEEP_TIME;
	gc_th->no_gc_sleep_time = DEF_GC_THREAD_NOGC_SLEEP_TIME;

	gc_th->gc_idle = 0;

	sbi->gc_thread = gc_th;
	init_waitqueue_head(&sbi->gc_thread->gc_wait_queue_head);
	sbi->gc_thread->f2fs_gc_task = kthread_run(gc_thread_func, sbi,
			"f2fs_gc-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(gc_th->f2fs_gc_task)) {
		err = PTR_ERR(gc_th->f2fs_gc_task);
		kfree(gc_th);
		sbi->gc_thread = NULL;
	}
out:
	return err;
}

void stop_gc_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_gc_kthread *gc_th = sbi->gc_thread;
	if (!gc_th)
		return;
	kthread_stop(gc_th->f2fs_gc_task);
	kfree(gc_th);
	sbi->gc_thread = NULL;
}

static int select_gc_type(struct f2fs_gc_kthread *gc_th, int gc_type)
{
	int gc_mode = (gc_type == BG_GC) ? GC_CB : GC_GREEDY;

	if (gc_th && gc_th->gc_idle) {
		if (gc_th->gc_idle == 1)
			gc_mode = GC_CB;
		else if (gc_th->gc_idle == 2)
			gc_mode = GC_GREEDY;
	}
	return gc_mode;
}

static void select_policy(struct f2fs_sb_info *sbi, int gc_type,
			int type, struct victim_sel_policy *p)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);

	if (p->alloc_mode == SSR) {
		p->gc_mode = GC_GREEDY;
		p->dirty_segmap = dirty_i->dirty_segmap[type];
		p->max_search = dirty_i->nr_dirty[type];
		p->ofs_unit = 1;
	} else {
#if (defined F2FS_FORCE_GC_CB || defined F2FS_GC_DELAYED)
		p->gc_mode = GC_CB;
#else
		p->gc_mode = select_gc_type(sbi->gc_thread, gc_type);
#endif
		p->dirty_segmap = dirty_i->dirty_segmap[DIRTY];
		p->max_search = dirty_i->nr_dirty[DIRTY];
		p->ofs_unit = sbi->segs_per_sec;
	}

	/* we need to check every dirty segments in the FG_GC case */
	if (gc_type != FG_GC && p->max_search > sbi->max_victim_search)
		p->max_search = sbi->max_victim_search;

	/* let's select beginning hot/small space first */
/*
	if (type == CURSEG_HOT_DATA || IS_NODESEG(type))
		p->offset = 0;
	else
*/
		p->offset = SIT_I(sbi)->last_victim[p->gc_mode];

}

static unsigned int get_max_cost(struct f2fs_sb_info *sbi,
				struct victim_sel_policy *p)
{
	/* SSR allocates in a segment unit */
	if (p->alloc_mode == SSR)
		return sbi->blocks_per_seg;
	if (p->gc_mode == GC_GREEDY)
		return 2 * sbi->blocks_per_seg * p->ofs_unit;
	else if (p->gc_mode == GC_CB)
		return UINT_MAX;
	else /* No other gc_mode */
		return 0;
}

static unsigned int check_bg_victims(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned int secno;

	/*
	 * If the gc_type is FG_GC, we can select victim segments
	 * selected by background GC before.
	 * Those segments guarantee they have small valid blocks.
	 */
	for_each_set_bit(secno, dirty_i->victim_secmap, MAIN_SECS(sbi)) {
		if (sec_usage_check(sbi, secno))
			continue;

		if (no_fggc_candidate(sbi, secno))
			continue;

		clear_bit(secno, dirty_i->victim_secmap);
		return GET_SEG_FROM_SEC(sbi, secno);
	}
	return NULL_SEGNO;
}

#ifdef STREAM_GC_STATIC
static unsigned int find_kmeans_num(struct f2fs_sb_info *sbi, unsigned int seq)
{
	unsigned int i;
	for (i = 0; i < sbi->kmeans_count; i++) {
		if (sbi->kmeans_history[i].seq > seq) {
//			printk("seq: %u kmeans seq:%d\n", seq, sbi->kmeans_history[i].seq); 
			break; 
		}
	}

	if (i > 0) {
		i = i - 1;
	}

	return i; 
}

#ifdef DYNAMIC_GC_W
static unsigned int set_age_for_stream_gc(struct f2fs_sb_info *sbi)
{
	int util = utilization(sbi);
#if 1
	if (util <= 78)
		sbi->GC_AGE_START = 44;
	else 
		sbi->GC_AGE_START = 100 - util + 22;

	sbi->GC_AGE_SUM = 44 - sbi->GC_AGE_START;
#endif
#if 0
	if (util <= 82)
		sbi->GC_AGE_START = 36;
	else 
		sbi->GC_AGE_START = 100 - util + 18

	sbi->GC_AGE_SUM = 36 - sbi->GC_AGE_START;
#endif
#if 0
	if (util <= 74)
		sbi->GC_AGE_START = 52;
	else 
		sbi->GC_AGE_START = 100 - util + 26

	sbi->GC_AGE_SUM = 52 - sbi->GC_AGE_START;
#endif
	sbi->GC_AGE_START *= 5;
	sbi->GC_AGE_SUM *= 5;
	return 0;
}
#endif
#endif
static unsigned int get_cb_cost(struct f2fs_sb_info *sbi, unsigned int segno)
{
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned int secno = GET_SEC_FROM_SEG(sbi, segno);
	unsigned int start = GET_SEG_FROM_SEC(sbi, secno);
	unsigned long long mtime = 0;
	unsigned int vblocks;
#if (defined F2FS_GC_DELAYED)
	unsigned int age = 0;
#else
	unsigned char age = 0;
#endif
	unsigned char u;
	unsigned int i;
	unsigned long long cur_seq = user_data_blocks(sbi) / 512;

#if (defined F2FS_GC_DELAYED)
	unsigned long long max_lifetime, centroid, lifetime, min_lifetime;
	unsigned long long hot_lifetime, hot_centroid;
	struct seg_entry *se = get_seg_entry(sbi, segno);
	int type = se->type;
#ifdef STREAM_GC_STATIC
	int kmeans_num = 0;
#endif

#if (NR_DATABASE == 1)
	if (type == CURSEG_DB_DATA) {
		type = CURSEG_HOT_DATA;
	}
#else
	if (0) {}
#endif 
	else if (type == CURSEG_COLD_DATA)
		type = NR_CLUSTER - 1;

#endif

	for (i = 0; i < sbi->segs_per_sec; i++) {
#if (defined F2FS_FORCE_GC_CB || defined F2FS_GC_DELAYED)
		mtime += (sbi->seg_lifetime[start + i] >> sbi->log_blocks_per_seg);
#else
		mtime += get_seg_entry(sbi, start + i)->mtime;
#endif
	}

	vblocks = get_valid_blocks(sbi, segno, true);
	mtime = div_u64(mtime, sbi->segs_per_sec);
#ifdef F2FS_GC_DELAYED
	lifetime = cur_seq - mtime;

#ifdef STREAM_GC_STATIC
	kmeans_num = find_kmeans_num(sbi, mtime);
	hot_lifetime = sbi->kmeans_history[kmeans_num].q3[CURSEG_HOT_DATA];
#else
	hot_lifetime = sbi->kmeans_max[CURSEG_HOT_DATA];
#endif
	hot_centroid = sbi->kmeans_center[CURSEG_HOT_DATA];

 	if (IS_NODESEG(type)) {
		max_lifetime = sbi->kmeans_max[CURSEG_WARM_DATA];
		centroid = sbi->kmeans_center[CURSEG_WARM_DATA];
		min_lifetime = 0;
	}
#ifdef STREAM_GC_STATIC
	else if (type == CURSEG_HOT_DATA) {
		max_lifetime = hot_lifetime;
		min_lifetime = 0;
	}
	else if (type == CURSEG_COLD_DATA) {
//		max_lifetime = hot_lifetime;
		max_lifetime = 0;
		min_lifetime = 0;	
	}
	else if (type == CURSEG_COLD_DATA-1) {
		max_lifetime = sbi->kmeans_history[kmeans_num].q3[type];
		min_lifetime = sbi->kmeans_history[kmeans_num].q1[type];

		if (lifetime < min_lifetime) {
			max_lifetime = hot_lifetime;
			min_lifetime = 0; 
		}
	}
	else if (type > CURSEG_HOT_DATA) {
		max_lifetime = sbi->kmeans_history[kmeans_num].q3[type];
		min_lifetime = sbi->kmeans_history[kmeans_num].q1[type];
	}
	else {
		max_lifetime = sbi->kmeans_history[kmeans_num].q3[type];
		min_lifetime = sbi->kmeans_history[kmeans_num].q1[type];
	}
#else
	else {
		max_lifetime = sbi->kmeans_max[type] * STREAM_WEIGHT;
		centroid = sbi->kmeans_center[type] * STREAM_WEIGHT;
	}
#endif

	if (vblocks == sbi->blocks_per_seg * sbi->segs_per_sec) {
		return UINT_MAX;
	}

	if (mtime < sit_i->min_mtime)
		sit_i->min_mtime = mtime;
	if (mtime > sit_i->max_mtime)
		sit_i->max_mtime = mtime;

	if (lifetime >= max_lifetime) {
#ifdef DYNAMIC_GC_W
		age = sbi->GC_AGE_START + sbi->GC_AGE_SUM; // loose, stream-aware
#else
		age = GC_AGE_START + GC_AGE_SUM; // loose, stream-aware
#endif
	} 
#ifdef STREAM_GC_PILOT
	else if (lifetime >= centroid) {
		unsigned long long denominator = max_lifetime - centroid;
		unsigned long long numerator = lifetime - centroid; 
	
		if (denominator > 0) {
			age = GC_AGE_SUM + div64_u64(100 * numerator, denominator);
		} else
			age = GC_AGE_SUM;
	}
#endif
#ifdef STREAM_GC_STATIC
	else if (lifetime < min_lifetime) {
#ifdef DYNAMIC_GC_W
		age = sbi->GC_AGE_START + sbi->GC_AGE_SUM; // loose, stream-aware
#else
		age = GC_AGE_START + GC_AGE_SUM;
#endif
	}
#endif
	else {
#ifdef STREAM_GC_STATIC
#ifdef DYNAMIC_GC_W
		age = sbi->GC_AGE_START;
#else
		age = GC_AGE_START;
#endif
#elif (defined STREAM_GC_PILOT)
		age = GC_AGE_START - div64_u64(GC_AGE_START * lifetime,
					centroid);
		if (type >= CURSEG_COLD_DATA) {
			unsigned char hot_age = 0;
			if (lifetime >= hot_lifetime) {
				hot_age = 100;
			} else if (lifetime >= hot_centroid) {
				unsigned long long denominator = hot_lifetime - hot_centroid;
				unsigned long long numerator = lifetime - hot_centroid; 
				if (denominator > 0) {
					hot_age = div64_u64(100 * numerator, denominator);
				} else
					hot_age = 0;
			} else {
				hot_age = 0;
			}
			if (age > hot_age)
				age = hot_age;
		}
		else if (type == CURSEG_HOT_DATA)
			age = 0;
		age = GC_AGE_SUM + age;
#else
		age = GC_AGE_SUM + div64_u64(100 * lifetime,
					max_lifetime);
#endif
	}
	vblocks = div_u64(vblocks, sbi->segs_per_sec);
	u = (vblocks * 100) >> sbi->log_blocks_per_seg;

	return UINT_MAX - ((100 * (100 - u) * age) / (100 + u));
#else
	vblocks = div_u64(vblocks, sbi->segs_per_sec);
	u = (vblocks * 100) >> sbi->log_blocks_per_seg;

	/* Handle if the system time has changed by the user */

	if (mtime < sit_i->min_mtime)
		sit_i->min_mtime = mtime;
	if (mtime > sit_i->max_mtime)
		sit_i->max_mtime = mtime;
	if (sit_i->max_mtime != sit_i->min_mtime)
		age = 100 - div64_u64(100 * (mtime - sit_i->min_mtime),
				sit_i->max_mtime - sit_i->min_mtime);

	return UINT_MAX - ((100 * (100 - u) * age) / (100 + u));
#endif
}

static unsigned int get_greedy_cost(struct f2fs_sb_info *sbi,
						unsigned int segno)
{
	unsigned int valid_blocks =
			get_valid_blocks(sbi, segno, true);

	return IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
				valid_blocks * 1 : valid_blocks;
}

static unsigned int get_ssr_cost(struct f2fs_sb_info *sbi,
						unsigned int segno)
{
	struct seg_entry *se = get_seg_entry(sbi, segno);

	return se->ckpt_valid_blocks > se->valid_blocks ?
				se->ckpt_valid_blocks : se->valid_blocks;
}

static inline unsigned int get_gc_cost(struct f2fs_sb_info *sbi,
			unsigned int segno, struct victim_sel_policy *p)
{
	if (p->alloc_mode == SSR)
		return get_ssr_cost(sbi, segno);

	/* alloc_mode == LFS */
	if (p->gc_mode == GC_GREEDY)
		return get_greedy_cost(sbi, segno);
	else
		return get_cb_cost(sbi, segno);
}

static unsigned int count_bits(const unsigned long *addr,
				unsigned int offset, unsigned int len)
{
	unsigned int end = offset + len, sum = 0;

	while (offset < end) {
		if (test_bit(offset++, addr))
			++sum;
	}
	return sum;
}

/*
 * This function is called from two paths.
 * One is garbage collection and the other is SSR segment selection.
 * When it is called during GC, it just gets a victim segment
 * and it does not remove it from dirty seglist.
 * When it is called from SSR segment selection, it finds a segment
 * which has minimum valid blocks and removes it from dirty seglist.
 */
static int get_victim_by_default(struct f2fs_sb_info *sbi,
		unsigned int *result, int gc_type, int type, char alloc_mode)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	struct sit_info *sm = SIT_I(sbi);
	struct victim_sel_policy p;
	unsigned int secno, last_victim;
	unsigned int last_segment = MAIN_SEGS(sbi);
	unsigned int nsearched = 0;

	mutex_lock(&dirty_i->seglist_lock);

	p.alloc_mode = alloc_mode;
	select_policy(sbi, gc_type, type, &p);

	p.min_segno = NULL_SEGNO;
	p.min_cost = get_max_cost(sbi, &p);

	if (*result != NULL_SEGNO) {
		if (IS_DATASEG(get_seg_entry(sbi, *result)->type) &&
			get_valid_blocks(sbi, *result, false) &&
			!sec_usage_check(sbi, GET_SEC_FROM_SEG(sbi, *result)))
			p.min_segno = *result;
		goto out;
	}

	if (p.max_search == 0)
		goto out;

	last_victim = sm->last_victim[p.gc_mode];
	if (p.alloc_mode == LFS && gc_type == FG_GC) {
		p.min_segno = check_bg_victims(sbi);
		if (p.min_segno != NULL_SEGNO)
			goto got_it;
	}

#ifdef F2FS_FGROUP
#ifdef DYNAMIC_GC_W
	set_age_for_stream_gc(sbi);
#endif
#endif

	while (1) {
		unsigned long cost;
		unsigned int segno;

		segno = find_next_bit(p.dirty_segmap, last_segment, p.offset);
		if (segno >= last_segment) {
			if (sm->last_victim[p.gc_mode]) {
				last_segment =
					sm->last_victim[p.gc_mode];
				sm->last_victim[p.gc_mode] = 0;
				p.offset = 0;
				continue;
			}
			break;
		}

#ifdef F2FS_NODE_AREA
/*
		if (p.alloc_mode == LFS) {
			secno = GET_SEC_FROM_SEG(sbi, segno);
			if(secno < sbi->node_area_secs) {
				p.offset = sbi->node_area_secs * sbi->segs_per_sec;
				continue;
			}
		}
*/
#else
#endif

		p.offset = segno + p.ofs_unit;
		if (p.ofs_unit > 1) {
			p.offset -= segno % p.ofs_unit;
			nsearched += count_bits(p.dirty_segmap,
						p.offset - p.ofs_unit,
						p.ofs_unit);
		} else {
			nsearched++;
		}

		secno = GET_SEC_FROM_SEG(sbi, segno);

		if (sec_usage_check(sbi, secno))
			goto next;
		if (gc_type == BG_GC && test_bit(secno, dirty_i->victim_secmap))
			goto next;
		if (gc_type == FG_GC && p.alloc_mode == LFS &&
					no_fggc_candidate(sbi, secno))
			goto next;

		cost = get_gc_cost(sbi, segno, &p);

		if (p.min_cost > cost) {
			p.min_segno = segno;
			p.min_cost = cost;
		}
next:
		if (nsearched >= p.max_search) {
			if (!sm->last_victim[p.gc_mode] && segno <= last_victim)
				sm->last_victim[p.gc_mode] = last_victim + 1;
			else
				sm->last_victim[p.gc_mode] = segno + 1;
			sm->last_victim[p.gc_mode] %= MAIN_SEGS(sbi);
			break;
		}
	}
	if (p.min_segno != NULL_SEGNO) {
		got_it:
		if (p.alloc_mode == LFS) {
			secno = GET_SEC_FROM_SEG(sbi, p.min_segno);
			if (gc_type == FG_GC)
				sbi->cur_victim_sec = secno;
			else
				set_bit(secno, dirty_i->victim_secmap);
		}
		*result = (p.min_segno / p.ofs_unit) * p.ofs_unit;

		trace_f2fs_get_victim(sbi->sb, type, gc_type, &p,
				sbi->cur_victim_sec,
				prefree_segments(sbi), free_segments(sbi));
	}
out:
	mutex_unlock(&dirty_i->seglist_lock);

	return (p.min_segno == NULL_SEGNO) ? 0 : 1;
}

static const struct victim_selection default_v_ops = {
	.get_victim = get_victim_by_default,
};

static struct inode *find_gc_inode(struct gc_inode_list *gc_list, nid_t ino)
{
	struct inode_entry *ie;

	ie = radix_tree_lookup(&gc_list->iroot, ino);
	if (ie)
		return ie->inode;
	return NULL;
}

static void add_gc_inode(struct gc_inode_list *gc_list, struct inode *inode)
{
	struct inode_entry *new_ie;

	if (inode == find_gc_inode(gc_list, inode->i_ino)) {
		iput(inode);
		return;
	}
	new_ie = f2fs_kmem_cache_alloc(inode_entry_slab, GFP_NOFS);
	new_ie->inode = inode;

	f2fs_radix_tree_insert(&gc_list->iroot, inode->i_ino, new_ie);
	list_add_tail(&new_ie->list, &gc_list->ilist);
}

static void put_gc_inode(struct gc_inode_list *gc_list)
{
	struct inode_entry *ie, *next_ie;
	list_for_each_entry_safe(ie, next_ie, &gc_list->ilist, list) {
		radix_tree_delete(&gc_list->iroot, ie->inode->i_ino);
		iput(ie->inode);
		list_del(&ie->list);
		kmem_cache_free(inode_entry_slab, ie);
	}
}

static int check_valid_map(struct f2fs_sb_info *sbi,
				unsigned int segno, int offset)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct seg_entry *sentry;
	int ret;

	mutex_lock(&sit_i->sentry_lock);
	sentry = get_seg_entry(sbi, segno);
	ret = f2fs_test_bit(offset, sentry->cur_valid_map);
	mutex_unlock(&sit_i->sentry_lock);
	return ret;
}

/*
 * This function compares node address got in summary with that in NAT.
 * On validity, copy that node with cold status, otherwise (invalid node)
 * ignore that.
 */
static void gc_node_segment(struct f2fs_sb_info *sbi,
		struct f2fs_summary *sum, unsigned int segno, int gc_type)
{
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;

	start_addr = START_BLOCK(sbi, segno);

next_step:
	entry = sum;

	for (off = 0; off < sbi->blocks_per_seg; off++, entry++) {
		nid_t nid = le32_to_cpu(entry->nid);
		struct page *node_page;
		struct node_info ni;

		/* stop BG_GC if there is not enough free sections. */
		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0))
			return;

		if (check_valid_map(sbi, segno, off) == 0)
			continue;

		if (phase == 0) {
			ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}

		if (phase == 1) {
			ra_node_page(sbi, nid);
			continue;
		}

		/* phase == 2 */
		node_page = get_node_page(sbi, nid);
		if (IS_ERR(node_page))
			continue;

		/* block may become invalid during get_node_page */
		if (check_valid_map(sbi, segno, off) == 0) {
			f2fs_put_page(node_page, 1);
			continue;
		}

		get_node_info(sbi, nid, &ni);
		if (ni.blk_addr != start_addr + off) {
			f2fs_put_page(node_page, 1);
			continue;
		}
		else {
#ifdef CONFIG_F2FS_MULTI_TYPE
			struct seg_entry *sentry = get_seg_entry(sbi, segno);
			int type = sentry->type;
			stat_inc_gc_mtype(sbi, type);
#else
	error
#endif
		}

		move_node_page(node_page, gc_type);
		stat_inc_node_blk_count(sbi, 1, gc_type);
	}

	if (++phase < 3)
		goto next_step;
}

/*
 * Calculate start block index indicating the given node offset.
 * Be careful, caller should give this node offset only indicating direct node
 * blocks. If any node offsets, which point the other types of node blocks such
 * as indirect or double indirect node blocks, are given, it must be a caller's
 * bug.
 */
block_t start_bidx_of_node(unsigned int node_ofs, struct inode *inode)
{
	unsigned int indirect_blks = 2 * NIDS_PER_BLOCK + 4;
	unsigned int bidx;

	if (node_ofs == 0)
		return 0;

	if (node_ofs <= 2) {
		bidx = node_ofs - 1;
	} else if (node_ofs <= indirect_blks) {
		int dec = (node_ofs - 4) / (NIDS_PER_BLOCK + 1);
		bidx = node_ofs - 2 - dec;
	} else {
		int dec = (node_ofs - indirect_blks - 3) / (NIDS_PER_BLOCK + 1);
		bidx = node_ofs - 5 - dec;
	}
	return bidx * ADDRS_PER_BLOCK + ADDRS_PER_INODE(inode);
}

static bool is_alive(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct node_info *dni, block_t blkaddr, unsigned int *nofs)
{
	struct page *node_page;
	nid_t nid;
	unsigned int ofs_in_node;
	block_t source_blkaddr;

	nid = le32_to_cpu(sum->nid);
	ofs_in_node = le16_to_cpu(sum->ofs_in_node);

	node_page = get_node_page(sbi, nid);
	if (IS_ERR(node_page))
		return false;

	get_node_info(sbi, nid, dni);

	if (sum->version != dni->version) {
		f2fs_msg(sbi->sb, KERN_WARNING,
				"%s: valid data with mismatched node version.",
				__func__);
		set_sbi_flag(sbi, SBI_NEED_FSCK);
	}

	*nofs = ofs_of_node(node_page);
	source_blkaddr = datablock_addr(node_page, ofs_in_node);
	f2fs_put_page(node_page, 1);

	if (source_blkaddr != blkaddr)
		return false;
	return true;
}

static void move_encrypted_block(struct inode *inode, block_t bidx,
							unsigned int segno, int off)
{
	struct f2fs_io_info fio = {
		.sbi = F2FS_I_SB(inode),
		.type = DATA,
		.temp = COLD,
		.op = REQ_OP_READ,
		.op_flags = 0,
		.encrypted_page = NULL,
		.in_list = false,
	};
	struct dnode_of_data dn;
	struct f2fs_summary sum;
	struct node_info ni;
	struct page *page;
	block_t newaddr;
	int err;

	/* do not read out */
	page = f2fs_grab_cache_page(inode->i_mapping, bidx, false);
	if (!page)
		return;

	if (!check_valid_map(F2FS_I_SB(inode), segno, off))
		goto out;

	if (f2fs_is_atomic_file(inode))
		goto out;

	set_new_dnode(&dn, inode, NULL, NULL, 0);
	err = get_dnode_of_data(&dn, bidx, LOOKUP_NODE);
	if (err)
		goto out;

	if (unlikely(dn.data_blkaddr == NULL_ADDR)) {
		ClearPageUptodate(page);
		goto put_out;
	}

	/*
	 * don't cache encrypted data into meta inode until previous dirty
	 * data were writebacked to avoid racing between GC and flush.
	 */
	f2fs_wait_on_page_writeback(page, DATA, true);

	get_node_info(fio.sbi, dn.nid, &ni);
	set_summary(&sum, dn.nid, dn.ofs_in_node, ni.version);

	/* read page */
	fio.page = page;
	fio.new_blkaddr = fio.old_blkaddr = dn.data_blkaddr;

	allocate_data_block(fio.sbi, NULL, fio.old_blkaddr, &newaddr,
					&sum, CURSEG_COLD_DATA, NULL, false);

	fio.encrypted_page = pagecache_get_page(META_MAPPING(fio.sbi), newaddr,
					FGP_LOCK | FGP_CREAT, GFP_NOFS);
	if (!fio.encrypted_page) {
		err = -ENOMEM;
		goto recover_block;
	}

	err = f2fs_submit_page_bio(&fio);
	if (err)
		goto put_page_out;

	/* write page */
	lock_page(fio.encrypted_page);

	if (unlikely(fio.encrypted_page->mapping != META_MAPPING(fio.sbi))) {
		err = -EIO;
		goto put_page_out;
	}
	if (unlikely(!PageUptodate(fio.encrypted_page))) {
		err = -EIO;
		goto put_page_out;
	}

	set_page_dirty(fio.encrypted_page);
	f2fs_wait_on_page_writeback(fio.encrypted_page, DATA, true);
	if (clear_page_dirty_for_io(fio.encrypted_page))
		dec_page_count(fio.sbi, F2FS_DIRTY_META);

	set_page_writeback(fio.encrypted_page);

	/* allocate block address */
	f2fs_wait_on_page_writeback(dn.node_page, NODE, true);

	fio.op = REQ_OP_WRITE;
	fio.op_flags = REQ_SYNC;
	fio.new_blkaddr = newaddr;
	f2fs_submit_page_write(&fio);

	f2fs_update_data_blkaddr(&dn, newaddr);
	set_inode_flag(inode, FI_APPEND_WRITE);
	if (page->index == 0)
		set_inode_flag(inode, FI_FIRST_BLOCK_WRITTEN);
put_page_out:
	f2fs_put_page(fio.encrypted_page, 1);
recover_block:
	if (err)
		__f2fs_replace_block(fio.sbi, &sum, newaddr, fio.old_blkaddr,
								true, true);
put_out:
	f2fs_put_dnode(&dn);
out:
	f2fs_put_page(page, 1);
}

static void move_data_page(struct inode *inode, block_t bidx, int gc_type,
							unsigned int segno, int off)
{
	struct page *page;

	page = get_lock_data_page(inode, bidx, true);
	if (IS_ERR(page))
		return;

	if (!check_valid_map(F2FS_I_SB(inode), segno, off))
		goto out;

	if (f2fs_is_atomic_file(inode))
		goto out;
	else {
#ifdef CONFIG_F2FS_MULTI_TYPE
		struct seg_entry *sentry = get_seg_entry(F2FS_I_SB(inode), segno);
		int type = sentry->type;
		stat_inc_gc_mtype(F2FS_I_SB(inode), type);
#else
error
#endif
	}

	if (gc_type == BG_GC) {
		if (PageWriteback(page))
			goto out;
		set_page_dirty(page);
		set_cold_data(page);
	} else {
		struct f2fs_io_info fio = {
			.sbi = F2FS_I_SB(inode),
			.type = DATA,
			.temp = COLD,
			.op = REQ_OP_WRITE,
			.op_flags = REQ_SYNC,
			.old_blkaddr = NULL_ADDR,
			.page = page,
			.encrypted_page = NULL,
			.need_lock = LOCK_REQ,
		};
		bool is_dirty = PageDirty(page);
		int err;

retry:
		set_page_dirty(page);
		f2fs_wait_on_page_writeback(page, DATA, true);
		if (clear_page_dirty_for_io(page)) {
			inode_dec_dirty_pages(inode);
			remove_dirty_inode(inode);
		}

		set_cold_data(page);

		err = do_write_data_page(&fio);
		if (err == -ENOMEM && is_dirty) {
			congestion_wait(BLK_RW_ASYNC, HZ/50);
			goto retry;
		}
	}
out:
	f2fs_put_page(page, 1);
}

/*
 * This function tries to get parent node of victim data block, and identifies
 * data block validity. If the block is valid, copy that with cold status and
 * modify parent node.
 * If the parent node is not valid or the data block address is different,
 * the victim data block is ignored.
 */
static void gc_data_segment(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;

	start_addr = START_BLOCK(sbi, segno);

next_step:
	entry = sum;

	for (off = 0; off < sbi->blocks_per_seg; off++, entry++) {
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;
		nid_t nid = le32_to_cpu(entry->nid);

		/* stop BG_GC if there is not enough free sections. */
		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0))
			return;

		if (check_valid_map(sbi, segno, off) == 0)
			continue;

		if (phase == 0) {
			ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}

		if (phase == 1) {
			ra_node_page(sbi, nid);
			continue;
		}

		/* Get an inode by ino with checking validity */
		if (!is_alive(sbi, entry, &dni, start_addr + off, &nofs))
			continue;

		if (phase == 2) {
			ra_node_page(sbi, dni.ino);
			continue;
		}

		ofs_in_node = le16_to_cpu(entry->ofs_in_node);

		if (phase == 3) {
			inode = f2fs_iget(sb, dni.ino);
			if (IS_ERR(inode) || is_bad_inode(inode))
				continue;

			/* if encrypted inode, let's go phase 3 */
			if (f2fs_encrypted_inode(inode) &&
						S_ISREG(inode->i_mode)) {
				add_gc_inode(gc_list, inode);
				continue;
			}

			start_bidx = start_bidx_of_node(nofs, inode);
			data_page = get_read_data_page(inode,
					start_bidx + ofs_in_node, REQ_RAHEAD,
					true);
			if (IS_ERR(data_page)) {
				iput(inode);
				continue;
			}

			f2fs_put_page(data_page, 0);
			add_gc_inode(gc_list, inode);
			continue;
		}

		/* phase 4 */
		inode = find_gc_inode(gc_list, dni.ino);
		if (inode) {
			struct f2fs_inode_info *fi = F2FS_I(inode);
			bool locked = false;

			if (S_ISREG(inode->i_mode)) {
				if (!down_write_trylock(&fi->dio_rwsem[READ]))
					continue;
				if (!down_write_trylock(
						&fi->dio_rwsem[WRITE])) {
					up_write(&fi->dio_rwsem[READ]);
					continue;
				}
				locked = true;
			}

			start_bidx = start_bidx_of_node(nofs, inode)
								+ ofs_in_node;
			if (f2fs_encrypted_inode(inode) && S_ISREG(inode->i_mode))
				move_encrypted_block(inode, start_bidx, segno, off);
			else
				move_data_page(inode, start_bidx, gc_type, segno, off);

			if (locked) {
				up_write(&fi->dio_rwsem[WRITE]);
				up_write(&fi->dio_rwsem[READ]);
			}

			stat_inc_data_blk_count(sbi, 1, gc_type);
		}
	}

	if (++phase < 5)
		goto next_step;
}

static int __get_victim(struct f2fs_sb_info *sbi, unsigned int *victim,
			int gc_type)
{
	struct sit_info *sit_i = SIT_I(sbi);
	int ret;

	mutex_lock(&sit_i->sentry_lock);
	ret = DIRTY_I(sbi)->v_ops->get_victim(sbi, victim, gc_type,
					      NO_CHECK_TYPE, LFS);
	mutex_unlock(&sit_i->sentry_lock);
	return ret;
}

static int do_garbage_collect(struct f2fs_sb_info *sbi,
				unsigned int start_segno,
				struct gc_inode_list *gc_list, int gc_type)
{
	struct page *sum_page;
	struct f2fs_summary_block *sum;
	struct blk_plug plug;
	unsigned int segno = start_segno;
	unsigned int end_segno = start_segno + sbi->segs_per_sec;
	int sec_freed = 0;
	unsigned char type = IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
						SUM_TYPE_DATA : SUM_TYPE_NODE;
#ifdef F2FS_TRACE_ENABLE
	unsigned int seg_lifetime = 0;
#endif

	/* readahead multi ssa blocks those have contiguous address */
	if (sbi->segs_per_sec > 1)
		ra_meta_pages(sbi, GET_SUM_BLOCK(sbi, segno),
					sbi->segs_per_sec, META_SSA, true);

	/* reference all summary page */
	while (segno < end_segno) {
#ifdef F2FS_TRACE_ENABLE
		seg_lifetime += (sbi->seg_lifetime[segno] >> sbi->log_blocks_per_seg);
#endif
		sum_page = get_sum_page(sbi, segno++);
		unlock_page(sum_page);
	}

	blk_start_plug(&plug);
#ifdef CONFIG_F2FS_MULTI_TYPE
	{
		struct seg_entry *sentry = get_seg_entry(sbi, start_segno);
		int test_type = sentry->type;
#ifdef F2FS_TRACE_ENABLE
		unsigned int cost = get_valid_blocks(sbi, start_segno, true);
		unsigned int cur_seq = user_data_blocks(sbi)/ 512;
		unsigned int free = 0;
		int i;
			
		seg_lifetime = div_u64(seg_lifetime, sbi->segs_per_sec);

		for (i = CURSEG_HOT_DATA; i <= CURSEG_COLD_DATA; i++) {
			struct curseg_info *curseg = CURSEG_I(sbi, i);
			block_t blkaddr = NEXT_FREE_BLKADDR(sbi, curseg); 
			blkaddr = blkaddr % 4096;
			free += blkaddr;
		}
		trace_printk("[F2FS-GC]\t%d\t%u\t%u\t%u\t%u\n", test_type, seg_lifetime, cur_seq, cost, free);
#endif
		stat_inc_gc_calls_mtype(sbi, test_type);
	}
#else
error
#endif
	for (segno = start_segno; segno < end_segno; segno++) {

		/* find segment summary of victim */
		sum_page = find_get_page(META_MAPPING(sbi),
					GET_SUM_BLOCK(sbi, segno));
		f2fs_put_page(sum_page, 0);

		if (get_valid_blocks(sbi, segno, false) == 0 ||
				!PageUptodate(sum_page) ||
				unlikely(f2fs_cp_error(sbi)))
			goto next;

		sum = page_address(sum_page);
		f2fs_bug_on(sbi, type != GET_SUM_TYPE((&sum->footer)));

		/*
		 * this is to avoid deadlock:
		 * - lock_page(sum_page)         - f2fs_replace_block
		 *  - check_valid_map()            - mutex_lock(sentry_lock)
		 *   - mutex_lock(sentry_lock)     - change_curseg()
		 *                                  - lock_page(sum_page)
		 */
		if (type == SUM_TYPE_NODE) {
#ifdef CONFIG_F2FS_MULTI_TYPE
			sbi->cur_node_gc = segno;
#endif
			gc_node_segment(sbi, sum->entries, segno, gc_type);
		}
		else {
			gc_data_segment(sbi, sum->entries, gc_list, segno,
								gc_type);
		}

		stat_inc_seg_count(sbi, type, gc_type);
next:
		f2fs_put_page(sum_page, 0);
	}

	if (gc_type == FG_GC)
		f2fs_submit_merged_write(sbi,
				(type == SUM_TYPE_NODE) ? NODE : DATA);

	blk_finish_plug(&plug);

	if (gc_type == FG_GC &&
		get_valid_blocks(sbi, start_segno, true) == 0)
		sec_freed = 1;

	stat_inc_call_count(sbi->stat_info);
#ifdef CONFIG_F2FS_MULTI_TYPE
	sbi->cur_node_gc = sbi->total_sections * sbi->segs_per_sec;
#endif
	return sec_freed;
}

int f2fs_gc(struct f2fs_sb_info *sbi, bool sync,
			bool background, unsigned int segno)
{
	int gc_type = sync ? FG_GC : BG_GC;
	int sec_freed = 0;
	int ret;
	struct cp_control cpc;
	unsigned int init_segno = segno;
	struct gc_inode_list gc_list = {
		.ilist = LIST_HEAD_INIT(gc_list.ilist),
		.iroot = RADIX_TREE_INIT(GFP_NOFS),
	};
#ifdef GC_TIME
	u64 start_time, end_time, exec_time;
	start_time = ktime_get();
#endif
	cpc.reason = __get_cp_reason(sbi);
gc_more:
	if (unlikely(!(sbi->sb->s_flags & MS_ACTIVE))) {
		ret = -EINVAL;
		goto stop;
	}
	if (unlikely(f2fs_cp_error(sbi))) {
		ret = -EIO;
		goto stop;
	}

	if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0)) {
		/*
		 * For example, if there are many prefree_segments below given
		 * threshold, we can make them free by checkpoint. Then, we
		 * secure free segments which doesn't need fggc any more.
		 */
		if (prefree_segments(sbi)) {
			ret = write_checkpoint(sbi, &cpc);
			if (ret)
				goto stop;
		}
		if (has_not_enough_free_secs(sbi, 0, 0))
			gc_type = FG_GC;
	}

	ret = -EINVAL;
	/* f2fs_balance_fs doesn't need to do BG_GC in critical path. */
	if (gc_type == BG_GC && !background)
		goto stop;
	if (!__get_victim(sbi, &segno, gc_type))
		goto stop;
	ret = 0;

	if (do_garbage_collect(sbi, segno, &gc_list, gc_type) &&
			gc_type == FG_GC)
		sec_freed++;

	if (gc_type == FG_GC)
		sbi->cur_victim_sec = NULL_SEGNO;

	if (!sync) {
		if (has_not_enough_free_secs(sbi, sec_freed, 0)) {
			segno = NULL_SEGNO;
			goto gc_more;
		}

		if (gc_type == FG_GC)
			ret = write_checkpoint(sbi, &cpc);
	}
stop:
	SIT_I(sbi)->last_victim[ALLOC_NEXT] = 0;
	SIT_I(sbi)->last_victim[FLUSH_DEVICE] = init_segno;
	mutex_unlock(&sbi->gc_mutex);

	put_gc_inode(&gc_list);

#ifdef GC_TIME
	end_time = ktime_get();
	exec_time = ktime_to_ns(ktime_sub(end_time, start_time));
	sbi->gc_time += exec_time;
#endif

	if (sync)
		ret = sec_freed ? 0 : -EAGAIN;
	return ret;
}

void build_gc_manager(struct f2fs_sb_info *sbi)
{
	u64 main_count, resv_count, ovp_count;

	DIRTY_I(sbi)->v_ops = &default_v_ops;

	/* threshold of # of valid blocks in a section for victims of FG_GC */
	main_count = SM_I(sbi)->main_segments << sbi->log_blocks_per_seg;
	resv_count = SM_I(sbi)->reserved_segments << sbi->log_blocks_per_seg;
	ovp_count = SM_I(sbi)->ovp_segments << sbi->log_blocks_per_seg;

	sbi->fggc_threshold = div64_u64((main_count - ovp_count) *
				BLKS_PER_SEC(sbi), (main_count - resv_count));

	/* give warm/cold data area from slower device */
	if (sbi->s_ndevs && sbi->segs_per_sec == 1)
		SIT_I(sbi)->last_victim[ALLOC_NEXT] =
				GET_SEGNO(sbi, FDEV(0).end_blk) + 1;
}

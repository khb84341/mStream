/*
 * Copyright (C) 2016 CNEX Labs
 * Initial release: Javier Gonzalez <javier@cnexlabs.com>
 *                  Matias Bjorling <matias@cnexlabs.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * pblk-gc.c - pblk's garbage collector
 */

#include "pblk.h"
#include <linux/delay.h>

static void pblk_gc_free_gc_rq(struct pblk_gc_rq *gc_rq)
{
	vfree(gc_rq->data);
	kfree(gc_rq);
}

static int pblk_gc_write(struct pblk *pblk)
{
	struct pblk_gc *gc = &pblk->gc;
	struct pblk_gc_rq *gc_rq, *tgc_rq;
	LIST_HEAD(w_list);

//	printk("JJY: pblk_gc_write 1\n");
	spin_lock(&gc->w_lock);
	if (list_empty(&gc->w_list)) {
		spin_unlock(&gc->w_lock);
		return 1;
	}

	list_cut_position(&w_list, &gc->w_list, gc->w_list.prev);
	gc->w_entries = 0;
	spin_unlock(&gc->w_lock);

	list_for_each_entry_safe(gc_rq, tgc_rq, &w_list, list) {
		pblk_write_gc_to_cache(pblk, gc_rq->data, gc_rq->lba_list,
				gc_rq->nr_secs, gc_rq->secs_to_gc,
				gc_rq->line, PBLK_IOTYPE_GC);

		list_del(&gc_rq->list);
		kref_put(&gc_rq->line->ref, pblk_line_put);
		pblk_gc_free_gc_rq(gc_rq);
	}

	return 0;
}

static void pblk_gc_writer_kick(struct pblk_gc *gc)
{
	wake_up_process(gc->gc_writer_ts);
}

/*
 * Responsible for managing all memory related to a gc request. Also in case of
 * failure
 */
static int pblk_gc_move_valid_secs(struct pblk *pblk, struct pblk_gc_rq *gc_rq)
{
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;
	struct pblk_gc *gc = &pblk->gc;
	struct pblk_line *line = gc_rq->line;
	void *data;
	unsigned int secs_to_gc;
	int ret = 0;

//	printk("JJY: move valid 1\n");
	data = vmalloc(gc_rq->nr_secs * geo->sec_size);
	if (!data) {
		ret = -ENOMEM;
		goto out;
	}

//	printk("JJY: move valid 2\n");
	/* Read from GC victim block */
	if (pblk_submit_read_gc(pblk, gc_rq->lba_list, data, gc_rq->nr_secs,
							&secs_to_gc, line)) {
		ret = -EFAULT;
		goto free_data;
	}

//	printk("JJY: move valid 3\n");
	if (!secs_to_gc)
		goto free_rq;

	gc_rq->data = data;
	gc_rq->secs_to_gc = secs_to_gc;

//	printk("JJY: move valid 4\n");
retry:
	spin_lock(&gc->w_lock);
	if (gc->w_entries >= PBLK_GC_W_QD) {
		spin_unlock(&gc->w_lock);
		pblk_gc_writer_kick(&pblk->gc);
		usleep_range(128, 256);
		goto retry;
	}
	gc->w_entries++;
	list_add_tail(&gc_rq->list, &gc->w_list);
	spin_unlock(&gc->w_lock);

//	printk("JJY: move valid 5\n");
	pblk_gc_writer_kick(&pblk->gc);

	return 0;

free_rq:
	kfree(gc_rq);
free_data:
	vfree(data);
out:
	kref_put(&line->ref, pblk_line_put);
	return ret;
}

static void pblk_put_line_back(struct pblk *pblk, struct pblk_line *line)
{
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	struct list_head *move_list;

	spin_lock(&line->lock);
	WARN_ON(line->state != PBLK_LINESTATE_GC);
	line->state = PBLK_LINESTATE_CLOSED;
#ifdef CONFIG_PBLK_MULTIMAP
	if (pblk->lstream[line->pstream].map_type != BLOCK_MAP)
		pblk->gc_target++;
#endif

	move_list = pblk_line_gc_list(pblk, line);
	spin_unlock(&line->lock);

	if (move_list) {
		spin_lock(&l_mg->gc_lock);
		list_add_tail(&line->list, move_list);
		spin_unlock(&l_mg->gc_lock);
	}
}

static void pblk_gc_line_ws(struct work_struct *work)
{
	struct pblk_line_ws *line_rq_ws = container_of(work,
						struct pblk_line_ws, ws);
	struct pblk *pblk = line_rq_ws->pblk;
	struct pblk_gc *gc = &pblk->gc;
	struct pblk_line *line = line_rq_ws->line;
	struct pblk_gc_rq *gc_rq = line_rq_ws->priv;

//	printk("JJY: pblk_gc_line_ws 1\n");
	up(&gc->gc_sem);

//	printk("JJY: pblk_gc_line_ws 2\n");
	if (pblk_gc_move_valid_secs(pblk, gc_rq)) {
		pr_err("pblk: could not GC all sectors: line:%d (%d/%d)\n",
						line->id, *line->vsc,
						gc_rq->nr_secs);
	}

	mempool_free(line_rq_ws, pblk->line_ws_pool);
}

static void pblk_gc_line_prepare_ws(struct work_struct *work)
{
	struct pblk_line_ws *line_ws = container_of(work, struct pblk_line_ws,
									ws);
	struct pblk *pblk = line_ws->pblk;
	struct pblk_line *line = line_ws->line;
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	struct pblk_line_meta *lm = &pblk->lm;
	struct pblk_gc *gc = &pblk->gc;
	struct line_emeta *emeta_buf;
	struct pblk_line_ws *line_rq_ws;
	struct pblk_gc_rq *gc_rq;
	__le64 *lba_list;
	int sec_left, nr_secs, bit;
	int ret;
#ifdef CONFIG_PBLK_MULTIMAP
	int nr_page;
#endif

//	printk("JJY: pblk_gc_line_prepare_ws 1\n");
	emeta_buf = pblk_malloc(lm->emeta_len[0], l_mg->emeta_alloc_type,
								GFP_KERNEL);
	if (!emeta_buf) {
		pr_err("pblk: cannot use GC emeta\n");
		return;
	}

	ret = pblk_line_read_emeta(pblk, line, emeta_buf);
	if (ret) {
		pr_err("pblk: line %d read emeta failed (%d)\n", line->id, ret);
		goto fail_free_emeta;
	}

	/* If this read fails, it means that emeta is corrupted. For now, leave
	 * the line untouched. TODO: Implement a recovery routine that scans and
	 * moves all sectors on the line.
	 */
	lba_list = pblk_recov_get_lba_list(pblk, emeta_buf);
	if (!lba_list) {
		pr_err("pblk: could not interpret emeta (line %d)\n", line->id);
		goto fail_free_emeta;
	}

	sec_left = pblk_line_vsc(line);
	if (sec_left < 0) {
		pr_err("pblk: corrupted GC line (%d)\n", line->id);
		goto fail_free_emeta;
	}

	bit = -1;
next_rq:
	gc_rq = kmalloc(sizeof(struct pblk_gc_rq), GFP_KERNEL);
	if (!gc_rq)
		goto fail_free_emeta;

	nr_secs = 0;
#ifdef CONFIG_PBLK_MULTIMAP
	nr_page = 0;
#endif
	do {
		bit = find_next_zero_bit(line->invalid_bitmap, lm->sec_per_line,
								bit + 1);
		if (bit > line->emeta_ssec)
			break;

#ifdef CONFIG_PBLK_MULTIMAP
		if(pblk_get_maptype(pblk, le64_to_cpu(lba_list[bit])) == PAGE_MAP)
		{
			nr_page++; 
			if ((nr_secs >= 12))
			{
				if (le64_to_cpu(lba_list[bit]) % 4 == 3) {
					gc_rq->lba_list[nr_secs++] = le64_to_cpu(lba_list[bit]);
					break;
				}
			}
		}
#endif 
		gc_rq->lba_list[nr_secs++] = le64_to_cpu(lba_list[bit]);
	} while (nr_secs < pblk->max_write_pgs);

	if (unlikely(!nr_secs)) {
		kfree(gc_rq);
		goto out;
	}

	gc_rq->nr_secs = nr_secs;
	gc_rq->line = line;

	line_rq_ws = mempool_alloc(pblk->line_ws_pool, GFP_KERNEL);
	if (!line_rq_ws)
		goto fail_free_gc_rq;

	line_rq_ws->pblk = pblk;
	line_rq_ws->line = line;
	line_rq_ws->priv = gc_rq;

	down(&gc->gc_sem);
	kref_get(&line->ref);

//	printk("JJY: pblk_gc_line_prepare_ws 2\n");
	INIT_WORK(&line_rq_ws->ws, pblk_gc_line_ws);
	queue_work(gc->gc_line_reader_wq, &line_rq_ws->ws);

	sec_left -= nr_secs;
	if (sec_left > 0)
		goto next_rq;

out:
	pblk_mfree(emeta_buf, l_mg->emeta_alloc_type);
	mempool_free(line_ws, pblk->line_ws_pool);

	kref_put(&line->ref, pblk_line_put);
	atomic_dec(&gc->inflight_gc);

	return;

fail_free_gc_rq:
	kfree(gc_rq);
fail_free_emeta:
	pblk_mfree(emeta_buf, l_mg->emeta_alloc_type);
	pblk_put_line_back(pblk, line);
	kref_put(&line->ref, pblk_line_put);
	mempool_free(line_ws, pblk->line_ws_pool);
	atomic_dec(&gc->inflight_gc);

	pr_err("pblk: Failed to GC line %d\n", line->id);
}

static int pblk_gc_line(struct pblk *pblk, struct pblk_line *line)
{
	struct pblk_gc *gc = &pblk->gc;
	struct pblk_line_ws *line_ws;

	pr_debug("pblk: line '%d' being reclaimed for GC\n", line->id);

//	printk("JJY: pblk_gc_line 1\n");
	line_ws = mempool_alloc(pblk->line_ws_pool, GFP_KERNEL);
	if (!line_ws)
		return -ENOMEM;

	line_ws->pblk = pblk;
	line_ws->line = line;

//	printk("JJY: pblk_gc_line 2\n");
	INIT_WORK(&line_ws->ws, pblk_gc_line_prepare_ws);
	queue_work(gc->gc_reader_wq, &line_ws->ws);

	return 0;
}

static int pblk_gc_read(struct pblk *pblk)
{
	struct pblk_gc *gc = &pblk->gc;
	struct pblk_line *line;

//	printk("JJY: gc_read 1\n");
	spin_lock(&gc->r_lock);
	if (list_empty(&gc->r_list)) {
		spin_unlock(&gc->r_lock);
		return 1;
	}

	line = list_first_entry(&gc->r_list, struct pblk_line, list);
	list_del(&line->list);
	spin_unlock(&gc->r_lock);

	pblk_gc_kick(pblk);

//	printk("JJY: gc_read 2\n");
	if (pblk_gc_line(pblk, line))
		pr_err("pblk: failed to GC line %d\n", line->id);

	return 0;
}

static void pblk_gc_reader_kick(struct pblk_gc *gc)
{
	wake_up_process(gc->gc_reader_ts);
}

#ifdef PBLK_GC_STREAM
static unsigned int find_kmeans_num(struct pblk *pblk, unsigned int seq)
{
    unsigned int i;
    for (i = 0; i < pblk->kmeans_count; i++) {
        if (pblk->kmeans_history[i].seq > seq) {
            break;
        }
    }

    if (i > 0) {
        i = i - 1;
    }
    return i;
}


#define F2FS_GC_RATE (2)
static unsigned int get_cb_age(struct pblk *pblk, struct pblk_line *line)
{
	unsigned int kmeans_num = find_kmeans_num(pblk, line->seq_nr);
	unsigned long lifetime = atomic_long_read(&pblk->g_writes)  - line->ctime;
	int cluster = line->pstream;
	unsigned int hot_lifetime = pblk->kmeans_history[kmeans_num].max[0];
	int node_stream = 0; 
	unsigned int max_lifetime, min_lifetime;
	unsigned int vblocks = le32_to_cpu(*line->vsc);
	unsigned int age;

	lifetime = lifetime / 512;

	if (cluster < DATA_START_STREAM)
		node_stream = 1;
	else 
		cluster = cluster - DATA_START_STREAM;
	
	if (node_stream == 1) {
		max_lifetime = pblk->kmeans_history[kmeans_num].max[1];
		min_lifetime = 0;
	} else {
		if (cluster == 0) {		// HOT
			max_lifetime = hot_lifetime;
			min_lifetime = 0;
		}
		else if (cluster == (NR_DATA-1)) {
			max_lifetime = 0;
			min_lifetime = 0;
		}
		else if (cluster == (NR_DATA-2)) {
			max_lifetime = pblk->kmeans_history[kmeans_num].max[cluster];
			min_lifetime = pblk->kmeans_history[kmeans_num].min[cluster];
			if (lifetime < min_lifetime) {
				max_lifetime = hot_lifetime;
				min_lifetime = 0;	
			}

		}
		else {
			max_lifetime = pblk->kmeans_history[kmeans_num].max[cluster];
			min_lifetime = pblk->kmeans_history[kmeans_num].min[cluster];
		}
	}

	if (vblocks == 4096)
		return 0;
	
	if (lifetime >= max_lifetime) {
		age = GC_AGE_START + GC_AGE_SUM;
	}
	else if (lifetime < min_lifetime) {
		age = GC_AGE_START + GC_AGE_SUM;
	}
	else {
		age = GC_AGE_START;
	}

	return age;
}
#endif

static struct pblk_line *pblk_gc_get_victim_line(struct pblk *pblk,
						 struct list_head *group_list)
{
	struct pblk_line *line, *victim;
	int line_vsc, victim_vsc;
#ifdef PBLK_FORCE_GC_CB
	unsigned int global_seq = pblk->l_mg.d_seq_nr;
	unsigned int line_cost, max_cost, age, u;
#endif
	victim = list_first_entry(group_list, struct pblk_line, list);
#ifdef PBLK_FORCE_GC_CB
	victim_vsc = le32_to_cpu(*victim->vsc);

	u = (victim_vsc * 100) >> 12;

	if (pblk->min_seq > victim->seq_nr)
		pblk->min_seq = victim->seq_nr;
	if (pblk->max_seq < victim->seq_nr)
		pblk->max_seq = victim->seq_nr;

	if (victim_vsc == 0)
		return victim; 

#ifdef PBLK_GC_STREAM
	age = get_cb_age(pblk, victim);
#else
	if (global_seq != pblk->min_seq) {
		age = 100 - div64_u64(100 * (victim->seq_nr - pblk->min_seq), 
					global_seq - pblk->min_seq);
	} else {
		printk("pblk->max_seq=%u pblk->min_seq=%u\n", global_seq, pblk->min_seq);
		age = 0;
	}
#endif
	max_cost = ((100 * (100 - u) * age) / (100 + u));

//	age = global_seq - victim->seq_nr;
//	max_cost = age * (4096 - victim_vsc) / (victim_vsc << 1);

#endif
	list_for_each_entry(line, group_list, list) {
		line_vsc = le32_to_cpu(*line->vsc);
		if (line_vsc == 0)
			return line; 
		if (line->pstream >= DATA_START_STREAM) {
			if (pblk->lstream[line->pstream].invalid_line == line->id)
				continue;
		}
		if (pblk->lstream[line->pstream].map_type == BLOCK_MAP) {
			continue;
		}
#ifdef PBLK_FORCE_GC_CB
		u = (line_vsc * 100) >> 12;

		if (pblk->min_seq > line->seq_nr)
			pblk->min_seq = line->seq_nr;
		if (pblk->max_seq < line->seq_nr)
			pblk->max_seq = line->seq_nr;

		if (line_vsc == 0)
			return line;
#ifdef PBLK_GC_STREAM
		age = get_cb_age(pblk, line);
#else
		if (global_seq != pblk->min_seq) {
			age = 100 - div64_u64(100 * (line->seq_nr - pblk->min_seq),
					global_seq - pblk->min_seq);
		}
		else {
			age = 0; 
			printk("pblk->max_seq=%u pblk->min_seq=%u\n", pblk->max_seq, pblk->min_seq);
		}
#endif
		line_cost = ((100 * (100 - u) * age) / (100 + u));

//		age = global_seq - line->seq_nr;
//		line_cost = age * (4096 - line_vsc) / (line_vsc << 1);


		if (pblk->lstream[victim->pstream].map_type == BLOCK_MAP) {
			victim = line;
			max_cost = line_cost;
			continue;
		}

		if (line_cost > max_cost) {
			victim = line;
			max_cost = line_cost;
		}
#else
		if (pblk->lstream[victim->pstream].map_type == BLOCK_MAP) {
			victim = line;
			continue;
		}
		line_vsc = le32_to_cpu(*line->vsc);
		victim_vsc = le32_to_cpu(*victim->vsc);
		if (line_vsc < victim_vsc)
			victim = line;
#endif
	}
	return victim;
}

static bool pblk_gc_should_run(struct pblk_gc *gc, struct pblk_rl *rl)
{
	unsigned int nr_blocks_free, nr_blocks_need;
#ifdef CONFIG_PBLK_MULTIMAP
	unsigned gc_target = rl->pblk->gc_target;
#endif
	nr_blocks_need = pblk_rl_high_thrs(rl);
	nr_blocks_free = pblk_rl_nr_free_blks(rl);

//	printk("JJY: should run %d %d %d\n", gc->gc_active, nr_blocks_need, nr_blocks_free);
	/* This is not critical, no need to take lock here */
#ifdef CONFIG_PBLK_MULTIMAP
	return ((gc->gc_active) && (nr_blocks_need > nr_blocks_free) && (gc_target > 0));
#else
	return ((gc->gc_active) && (nr_blocks_need > nr_blocks_free));
#endif
}

/*
 * Lines with no valid sectors will be returned to the free list immediately. If
 * GC is activated - either because the free block count is under the determined
 * threshold, or because it is being forced from user space - only lines with a
 * high count of invalid sectors will be recycled.
 */
// JJY: TODO: rl
static void pblk_gc_run(struct pblk *pblk)
{
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	struct pblk_gc *gc = &pblk->gc;
	struct pblk_line *line;
	struct list_head *group_list;
	bool run_gc;
	int inflight_gc, gc_group = 0, prev_group = 0;

	do {
		spin_lock(&l_mg->gc_lock);
		if (list_empty(&l_mg->gc_full_list)) {
			spin_unlock(&l_mg->gc_lock);
			break;
		}

		line = list_first_entry(&l_mg->gc_full_list,
							struct pblk_line, list);

		spin_lock(&line->lock);
		WARN_ON(line->state != PBLK_LINESTATE_CLOSED);
		line->state = PBLK_LINESTATE_GC;
#ifdef CONFIG_PBLK_MULTIMAP
		if (pblk->lstream[line->pstream].map_type != BLOCK_MAP)
			pblk->gc_target--;
#endif

		spin_unlock(&line->lock);

		list_del(&line->list);
		spin_unlock(&l_mg->gc_lock);

		kref_put(&line->ref, pblk_line_put);
	} while (1);

	run_gc = pblk_gc_should_run(&pblk->gc, &pblk->rl);
//	printk("JJY: inflight %d %d\n", gc->inflight_gc, PBLK_GC_L_QD);
	if (!run_gc || (atomic_read(&gc->inflight_gc) >= PBLK_GC_L_QD))
		return;


next_gc_group:
	group_list = l_mg->gc_lists[gc_group++];

//	printk("JJY: pblk_gc_run 1\n");
	do {
		spin_lock(&l_mg->gc_lock);
		if (list_empty(group_list)) {
			spin_unlock(&l_mg->gc_lock);
			break;
		}
//		printk("JJY: pblk_gc_run 2\n");

		line = pblk_gc_get_victim_line(pblk, group_list);
		if (pblk->lstream[line->pstream].map_type == BLOCK_MAP) {
			int line_vsc = le32_to_cpu(*line->vsc);
			if (line_vsc > 0) {
				printk("BLOCK MAP select: %d\n", line_vsc);
				spin_unlock(&l_mg->gc_lock);
				break;
			}
		}
		spin_lock(&line->lock);
		WARN_ON(line->state != PBLK_LINESTATE_CLOSED);
		line->state = PBLK_LINESTATE_GC;
#ifdef CONFIG_PBLK_MULTIMAP
		if (pblk->lstream[line->pstream].map_type != BLOCK_MAP)
			pblk->gc_target--;
#endif
		spin_unlock(&line->lock);

		list_del(&line->list);
		spin_unlock(&l_mg->gc_lock);

		if (line != NULL) {
			int line_vsc = le32_to_cpu(*line->vsc);
			trace_printk("[PBLK-GC]\t%d\t%d\n", line_vsc, line->pstream);
		}

		spin_lock(&gc->r_lock);
		list_add_tail(&line->list, &gc->r_list);
		spin_unlock(&gc->r_lock);

		inflight_gc = atomic_inc_return(&gc->inflight_gc);
		pblk_gc_reader_kick(gc);

		prev_group = 1;

		/* No need to queue up more GC lines than we can handle */
		run_gc = pblk_gc_should_run(&pblk->gc, &pblk->rl);
		if (!run_gc || inflight_gc >= PBLK_GC_L_QD)
			break;
//		printk("JJY: pblk_gc_run 3\n");
	} while (1);

	if (!prev_group && pblk->rl.rb_state > gc_group &&
						gc_group < PBLK_GC_NR_LISTS)
		goto next_gc_group;
//	printk("JJY: pblk_gc_run 4\n");
}

void pblk_gc_kick(struct pblk *pblk)
{
	struct pblk_gc *gc = &pblk->gc;

	wake_up_process(gc->gc_ts);
	pblk_gc_writer_kick(gc);
	pblk_gc_reader_kick(gc);
	mod_timer(&gc->gc_timer, jiffies + msecs_to_jiffies(GC_TIME_MSECS));
}

static void pblk_gc_timer(unsigned long data)
{
	struct pblk *pblk = (struct pblk *)data;

	pblk_gc_kick(pblk);
}

static int pblk_gc_ts(void *data)
{
	struct pblk *pblk = data;

	while (!kthread_should_stop()) {
		pblk_gc_run(pblk);
		set_current_state(TASK_INTERRUPTIBLE);
		io_schedule();
	}

	return 0;
}

static int pblk_gc_writer_ts(void *data)
{
	struct pblk *pblk = data;

	while (!kthread_should_stop()) {
		if (!pblk_gc_write(pblk)) {
			continue;
		}
		set_current_state(TASK_INTERRUPTIBLE);
		io_schedule();
	}

	return 0;
}

static int pblk_gc_reader_ts(void *data)
{
	struct pblk *pblk = data;

//	printk("JJY: pblk_gc_reader_ts 1\n");
	while (!kthread_should_stop()) {
//		printk("JJY: pblk_gc_reader_ts 2\n");
		if (!pblk_gc_read(pblk)) {
//			printk("JJY: pblk_gc_reader_ts 3\n");
			continue;
		}
//		printk("JJY: pblk_gc_reader_ts 4\n");
		set_current_state(TASK_INTERRUPTIBLE);
		io_schedule();
	}

	return 0;
}

static void pblk_gc_start(struct pblk *pblk)
{
	pblk->gc.gc_active = 1;
	pr_debug("pblk: gc start\n");
}

void pblk_gc_should_start(struct pblk *pblk)
{
	struct pblk_gc *gc = &pblk->gc;

//	printk("JJY: should start\n");
	if (gc->gc_enabled && !gc->gc_active) {
//		printk("JJY: should start %d %d\n", gc->gc_enabled, gc->gc_active);
		pblk_gc_start(pblk);
	}

	pblk_gc_kick(pblk);
}

/*
 * If flush_wq == 1 then no lock should be held by the caller since
 * flush_workqueue can sleep
 */
static void pblk_gc_stop(struct pblk *pblk, int flush_wq)
{
	pblk->gc.gc_active = 0;
	pr_debug("pblk: gc stop\n");
}

void pblk_gc_should_stop(struct pblk *pblk)
{
	struct pblk_gc *gc = &pblk->gc;

	if (gc->gc_active && !gc->gc_forced)
		pblk_gc_stop(pblk, 0);
}

void pblk_gc_sysfs_state_show(struct pblk *pblk, int *gc_enabled,
			      int *gc_active)
{
	struct pblk_gc *gc = &pblk->gc;

	spin_lock(&gc->lock);
	*gc_enabled = gc->gc_enabled;
	*gc_active = gc->gc_active;
	spin_unlock(&gc->lock);
}

int pblk_gc_sysfs_force(struct pblk *pblk, int force)
{
	struct pblk_gc *gc = &pblk->gc;

	if (force < 0 || force > 1)
		return -EINVAL;

	spin_lock(&gc->lock);
	gc->gc_forced = force;

	if (force)
		gc->gc_enabled = 1;
	else
		gc->gc_enabled = 0;
	spin_unlock(&gc->lock);

	pblk_gc_should_start(pblk);

	return 0;
}

int pblk_gc_init(struct pblk *pblk)
{
	struct pblk_gc *gc = &pblk->gc;
	int ret;

	gc->gc_ts = kthread_create(pblk_gc_ts, pblk, "pblk-gc-ts");
	if (IS_ERR(gc->gc_ts)) {
		pr_err("pblk: could not allocate GC main kthread\n");
		return PTR_ERR(gc->gc_ts);
	}

	gc->gc_writer_ts = kthread_create(pblk_gc_writer_ts, pblk,
							"pblk-gc-writer-ts");
	if (IS_ERR(gc->gc_writer_ts)) {
		pr_err("pblk: could not allocate GC writer kthread\n");
		ret = PTR_ERR(gc->gc_writer_ts);
		goto fail_free_main_kthread;
	}

	gc->gc_reader_ts = kthread_create(pblk_gc_reader_ts, pblk,
							"pblk-gc-reader-ts");
	if (IS_ERR(gc->gc_reader_ts)) {
		pr_err("pblk: could not allocate GC reader kthread\n");
		ret = PTR_ERR(gc->gc_reader_ts);
		goto fail_free_writer_kthread;
	}

	setup_timer(&gc->gc_timer, pblk_gc_timer, (unsigned long)pblk);
	mod_timer(&gc->gc_timer, jiffies + msecs_to_jiffies(GC_TIME_MSECS));

	gc->gc_active = 0;
	gc->gc_forced = 0;
	gc->gc_enabled = 1;
	gc->w_entries = 0;
	atomic_set(&gc->inflight_gc, 0);

	/* Workqueue that reads valid sectors from a line and submit them to the
	 * GC writer to be recycled.
	 */
	gc->gc_line_reader_wq = alloc_workqueue("pblk-gc-line-reader-wq",
			WQ_MEM_RECLAIM | WQ_UNBOUND, PBLK_GC_MAX_READERS);
	if (!gc->gc_line_reader_wq) {
		pr_err("pblk: could not allocate GC line reader workqueue\n");
		ret = -ENOMEM;
		goto fail_free_reader_kthread;
	}

	/* Workqueue that prepare lines for GC */
	gc->gc_reader_wq = alloc_workqueue("pblk-gc-line_wq",
					WQ_MEM_RECLAIM | WQ_UNBOUND, 1);
	if (!gc->gc_reader_wq) {
		pr_err("pblk: could not allocate GC reader workqueue\n");
		ret = -ENOMEM;
		goto fail_free_reader_line_wq;
	}

	spin_lock_init(&gc->lock);
	spin_lock_init(&gc->w_lock);
	spin_lock_init(&gc->r_lock);

	sema_init(&gc->gc_sem, 128);

	INIT_LIST_HEAD(&gc->w_list);
	INIT_LIST_HEAD(&gc->r_list);

	return 0;

fail_free_reader_line_wq:
	destroy_workqueue(gc->gc_line_reader_wq);
fail_free_reader_kthread:
	kthread_stop(gc->gc_reader_ts);
fail_free_writer_kthread:
	kthread_stop(gc->gc_writer_ts);
fail_free_main_kthread:
	kthread_stop(gc->gc_ts);

	return ret;
}

void pblk_gc_exit(struct pblk *pblk)
{
	struct pblk_gc *gc = &pblk->gc;

	flush_workqueue(gc->gc_reader_wq);
	flush_workqueue(gc->gc_line_reader_wq);

	del_timer(&gc->gc_timer);
	pblk_gc_stop(pblk, 1);

	if (gc->gc_ts)
		kthread_stop(gc->gc_ts);

	if (gc->gc_reader_wq)
		destroy_workqueue(gc->gc_reader_wq);

	if (gc->gc_line_reader_wq)
		destroy_workqueue(gc->gc_line_reader_wq);

	if (gc->gc_writer_ts)
		kthread_stop(gc->gc_writer_ts);

	if (gc->gc_reader_ts)
		kthread_stop(gc->gc_reader_ts);
}

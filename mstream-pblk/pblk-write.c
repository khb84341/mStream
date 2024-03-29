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
 * pblk-write.c - pblk's write path from write buffer to media
 */

#include "pblk.h"

static unsigned long pblk_end_w_bio(struct pblk *pblk, struct nvm_rq *rqd,
				    struct pblk_c_ctx *c_ctx)
{
	struct nvm_tgt_dev *dev = pblk->dev;
	struct bio *original_bio;
	unsigned long ret;
	int i;
	u8 nrb = rqd->nrb;
	struct pblk_rb *rb = &pblk->rb_ctx[nrb].rwb;

	i = 0;
	do {
		struct pblk_w_ctx *w_ctx;

		w_ctx = pblk_rb_w_ctx(rb, c_ctx->sentry + i);
		while ((original_bio = bio_list_pop(&w_ctx->bios)))
			bio_endio(original_bio);

		i++;

#ifdef GC_TIME
		if (nrb == GC_NRB) {
			if (w_ctx->write_time > 0) {
				u64 end_time = ktime_get();
				u64 exec_time = ktime_to_ns(ktime_sub(end_time, w_ctx->write_time));
				pblk->gc_time += exec_time;
			}
		}
		w_ctx->write_time = 0;
#endif
	} while (i < c_ctx->nr_valid);

	if (c_ctx->nr_padded)
		pblk_bio_free_pages(pblk, rqd->bio, c_ctx->nr_valid,
							c_ctx->nr_padded);

#ifdef CONFIG_NVM_DEBUG
	atomic_long_add(rqd->nr_ppas, &pblk->sync_writes);
#endif

	ret = pblk_rb_sync_advance(rb, c_ctx->nr_valid);

	nvm_dev_dma_free(dev->parent, rqd->meta_list, rqd->dma_meta_list);

	bio_put(rqd->bio);
	pblk_free_rqd(pblk, rqd, WRITE);

	return ret;
}

static unsigned long pblk_end_queued_w_bio(struct pblk *pblk,
					   struct nvm_rq *rqd,
					   struct pblk_c_ctx *c_ctx)
{
	list_del(&c_ctx->list);
	return pblk_end_w_bio(pblk, rqd, c_ctx);
}

static void pblk_complete_write(struct pblk *pblk, struct nvm_rq *rqd,
				struct pblk_c_ctx *c_ctx)
{
	struct pblk_c_ctx *c, *r;
	unsigned long flags;
	unsigned long pos;
	u8 nrb = rqd->nrb;
	struct pblk_rb *rb = &pblk->rb_ctx[nrb].rwb;

#ifdef CONFIG_NVM_DEBUG
	atomic_long_sub(c_ctx->nr_valid, &pblk->inflight_writes);
#endif

	pblk_up_rq(pblk, rqd->ppa_list, rqd->nr_ppas, c_ctx->lun_bitmap);

// 	printk("JJY: complete cpu %d rb %d pos %d\n", smp_processor_id(), nrb, c_ctx->sentry);
	pos = pblk_rb_sync_init(rb, &flags);
	if (pos == c_ctx->sentry) {
		pos = pblk_end_w_bio(pblk, rqd, c_ctx);
//	 	printk("JJY: complete1 cpu %d rb %d pos %d\n", smp_processor_id(), nrb, c_ctx->sentry);

retry:
		list_for_each_entry_safe(c, r, &pblk->rb_ctx[nrb].compl_list, list) {
			rqd = nvm_rq_from_c_ctx(c);
//		 	printk("JJY: complete list cpu %d rb %d pos %d\n", smp_processor_id(),rqd->nrb, c->sentry);
			if (c->sentry == pos) {
				pos = pblk_end_queued_w_bio(pblk, rqd, c);
				goto retry;
			}
		}
	} else {
		WARN_ON(nvm_rq_from_c_ctx(c_ctx) != rqd);
		list_add_tail(&c_ctx->list, &pblk->rb_ctx[nrb].compl_list);
	}
	pblk_rb_sync_end(&pblk->rb_ctx[nrb].rwb, &flags);
}

/* When a write fails, we are not sure whether the block has grown bad or a page
 * range is more susceptible to write errors. If a high number of pages fail, we
 * assume that the block is bad and we mark it accordingly. In all cases, we
 * remap and resubmit the failed entries as fast as possible; if a flush is
 * waiting on a completion, the whole stack would stall otherwise.
 */
// JJY: TODO: rb
static void pblk_end_w_fail(struct pblk *pblk, struct nvm_rq *rqd)
{
	void *comp_bits = &rqd->ppa_status;
	struct pblk_c_ctx *c_ctx = nvm_rq_to_pdu(rqd);
	struct pblk_rec_ctx *recovery;
	struct ppa_addr *ppa_list = rqd->ppa_list;
	int nr_ppas = rqd->nr_ppas;
	unsigned int c_entries;
	int bit, ret;
	u8 nrb = rqd->nrb;
	struct pblk_rb *rb = &pblk->rb_ctx[nrb].rwb;

	if (unlikely(nr_ppas == 1))
		ppa_list = &rqd->ppa_addr;

	recovery = mempool_alloc(pblk->rec_pool, GFP_ATOMIC);
	if (!recovery) {
		pr_err("pblk: could not allocate recovery context\n");
		return;
	}
	INIT_LIST_HEAD(&recovery->failed);

	bit = -1;
	while ((bit = find_next_bit(comp_bits, nr_ppas, bit + 1)) < nr_ppas) {
		struct pblk_rb_entry *entry;
		struct ppa_addr ppa;

		/* Logic error */
		if (bit > c_ctx->nr_valid) {
			WARN_ONCE(1, "pblk: corrupted write request\n");
			mempool_free(recovery, pblk->rec_pool);
			goto out;
		}

		ppa = ppa_list[bit];
		entry = pblk_rb_sync_scan_entry(rb, &ppa);
		if (!entry) {
			pr_err("pblk: could not scan entry on write failure\n");
			mempool_free(recovery, pblk->rec_pool);
			goto out;
		}

		/* The list is filled first and emptied afterwards. No need for
		 * protecting it with a lock
		 */
		list_add_tail(&entry->index, &recovery->failed);
	}

	c_entries = find_first_bit(comp_bits, nr_ppas);
	ret = pblk_recov_setup_rq(pblk, rb, c_ctx, recovery, comp_bits, c_entries);
	if (ret) {
		pr_err("pblk: could not recover from write failure\n");
		mempool_free(recovery, pblk->rec_pool);
		goto out;
	}

	INIT_WORK(&recovery->ws_rec, pblk_submit_rec);
	queue_work(pblk->close_wq, &recovery->ws_rec);

out:
	pblk_complete_write(pblk, rqd, c_ctx);
}

static void pblk_end_io_write(struct nvm_rq *rqd)
{
	struct pblk *pblk = rqd->private;
	struct pblk_c_ctx *c_ctx = nvm_rq_to_pdu(rqd);

	if (rqd->error) {
		pblk_log_write_err(pblk, rqd);
		return pblk_end_w_fail(pblk, rqd);
	}
#ifdef CONFIG_NVM_DEBUG
	else
		WARN_ONCE(rqd->bio->bi_status, "pblk: corrupted write error\n");
#endif

	pblk_complete_write(pblk, rqd, c_ctx);
	atomic_dec(&pblk->inflight_io);
}

static void pblk_end_io_write_meta(struct nvm_rq *rqd)
{
	struct pblk *pblk = rqd->private;
	struct nvm_tgt_dev *dev = pblk->dev;
	struct pblk_g_ctx *m_ctx = nvm_rq_to_pdu(rqd);
	struct pblk_line *line = m_ctx->private;
	struct pblk_emeta *emeta = line->emeta;
	int sync;

	pblk_up_page(pblk, rqd->ppa_list, rqd->nr_ppas);

	if (rqd->error) {
		pblk_log_write_err(pblk, rqd);
		pr_err("pblk: metadata I/O failed. Line %d\n", line->id);
	}
#ifdef CONFIG_NVM_DEBUG
	else
		WARN_ONCE(rqd->bio->bi_status, "pblk: corrupted write error\n");
#endif

	sync = atomic_add_return(rqd->nr_ppas, &emeta->sync);
	if (sync == emeta->nr_entries)
		pblk_line_run_ws(pblk, line, NULL, pblk_line_close_ws,
								pblk->close_wq);

	bio_put(rqd->bio);
	nvm_dev_dma_free(dev->parent, rqd->meta_list, rqd->dma_meta_list);
	pblk_free_rqd(pblk, rqd, READ);

	atomic_dec(&pblk->inflight_io);
}

static int pblk_alloc_w_rq(struct pblk *pblk, struct nvm_rq *rqd,
			   unsigned int nr_secs,
			   nvm_end_io_fn(*end_io))
{
	struct nvm_tgt_dev *dev = pblk->dev;

	/* Setup write request */
	rqd->opcode = NVM_OP_PWRITE;
	rqd->nr_ppas = nr_secs;
	rqd->flags = pblk_set_progr_mode(pblk, WRITE);
	rqd->private = pblk;
	rqd->end_io = end_io;

	rqd->meta_list = nvm_dev_dma_alloc(dev->parent, GFP_KERNEL,
							&rqd->dma_meta_list);
	if (!rqd->meta_list)
		return -ENOMEM;

	rqd->ppa_list = rqd->meta_list + pblk_dma_meta_size;
	rqd->dma_ppa_list = rqd->dma_meta_list + pblk_dma_meta_size;

	return 0;
}

static int pblk_setup_w_rq(struct pblk *pblk, struct nvm_rq *rqd,
			   struct pblk_c_ctx *c_ctx, struct ppa_addr *erase_ppa, unsigned int nrb)
{
	struct pblk_line_meta *lm = &pblk->lm;
	struct pblk_line *e_line = pblk_line_get_erase(pblk, nrb);
	unsigned int valid = c_ctx->nr_valid;
	unsigned int padded = c_ctx->nr_padded;
	unsigned int nr_secs = valid + padded;
	unsigned long *lun_bitmap;
	int ret = 0;

	lun_bitmap = kzalloc(lm->lun_bitmap_len, GFP_KERNEL);
	if (!lun_bitmap)
		return -ENOMEM;
	c_ctx->lun_bitmap = lun_bitmap;

	ret = pblk_alloc_w_rq(pblk, rqd, nr_secs, pblk_end_io_write);
	if (ret) {
		kfree(lun_bitmap);
		return ret;
	}

	if (likely(!e_line || !atomic_read(&e_line->left_eblks)))
		pblk_map_rq(pblk, rqd, c_ctx->sentry, lun_bitmap, valid, 0, nrb);
	else
		pblk_map_erase_rq(pblk, rqd, c_ctx->sentry, lun_bitmap,
							valid, erase_ppa, nrb);

	return 0;
}

int pblk_setup_w_rec_rq(struct pblk *pblk, struct nvm_rq *rqd,
			struct pblk_c_ctx *c_ctx)
{
	struct pblk_line_meta *lm = &pblk->lm;
	unsigned long *lun_bitmap;
	int ret;

	lun_bitmap = kzalloc(lm->lun_bitmap_len, GFP_KERNEL);
	if (!lun_bitmap)
		return -ENOMEM;

	c_ctx->lun_bitmap = lun_bitmap;

	ret = pblk_alloc_w_rq(pblk, rqd, rqd->nr_ppas, pblk_end_io_write);
	if (ret)
		return ret;

	pblk_map_rq(pblk, rqd, c_ctx->sentry, lun_bitmap, c_ctx->nr_valid, 0, rqd->nrb);

	rqd->ppa_status = (u64)0;
	rqd->flags = pblk_set_progr_mode(pblk, WRITE);

	return ret;
}

static int pblk_calc_secs_to_sync(struct pblk *pblk, unsigned int secs_avail,
				  unsigned int secs_to_flush)
{
	int secs_to_sync;

	secs_to_sync = pblk_calc_secs(pblk, secs_avail, secs_to_flush);

#ifdef CONFIG_NVM_DEBUG
	if ((!secs_to_sync && secs_to_flush)
			|| (secs_to_sync < 0)
			|| (secs_to_sync > secs_avail && !secs_to_flush)) {
		pr_err("pblk: bad sector calculation (a:%d,s:%d,f:%d)\n",
				secs_avail, secs_to_sync, secs_to_flush);
	}
#endif

	return secs_to_sync;
}

static inline int pblk_valid_meta_ppa(struct pblk *pblk,
				      struct pblk_line *meta_line,
				      struct ppa_addr *ppa_list, int nr_ppas)
{
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;
	struct pblk_line *data_line;
	struct ppa_addr ppa, ppa_opt;
	u64 paddr;
	int i;

	data_line = &pblk->lines[pblk_dev_ppa_to_line(ppa_list[0])];
	paddr = pblk_lookup_page(pblk, meta_line);
	ppa = addr_to_gen_ppa(pblk, paddr, 0);

	if (test_bit(pblk_ppa_to_pos(geo, ppa), data_line->blk_bitmap))
		return 1;

	/* Schedule a metadata I/O that is half the distance from the data I/O
	 * with regards to the number of LUNs forming the pblk instance. This
	 * balances LUN conflicts across every I/O.
	 *
	 * When the LUN configuration changes (e.g., due to GC), this distance
	 * can align, which would result on a LUN deadlock. In this case, modify
	 * the distance to not be optimal, but allow metadata I/Os to succeed.
	 */
	ppa_opt = addr_to_gen_ppa(pblk, paddr + data_line->meta_distance, 0);
	if (unlikely(ppa_opt.ppa == ppa.ppa)) {
		data_line->meta_distance--;
		return 0;
	}

	for (i = 0; i < nr_ppas; i += pblk->min_write_pgs)
		if (ppa_list[i].g.ch == ppa_opt.g.ch &&
					ppa_list[i].g.lun == ppa_opt.g.lun)
			return 1;

	if (test_bit(pblk_ppa_to_pos(geo, ppa_opt), data_line->blk_bitmap)) {
		for (i = 0; i < nr_ppas; i += pblk->min_write_pgs)
			if (ppa_list[i].g.ch == ppa.g.ch &&
						ppa_list[i].g.lun == ppa.g.lun)
				return 0;

		return 1;
	}

	return 0;
}

int pblk_submit_meta_io(struct pblk *pblk, struct pblk_line *meta_line, unsigned int nrb)
{
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	struct pblk_line_meta *lm = &pblk->lm;
	struct pblk_emeta *emeta = meta_line->emeta;
	struct pblk_g_ctx *m_ctx;
	struct bio *bio;
	struct nvm_rq *rqd;
	void *data;
	u64 paddr;
	int rq_ppas = pblk->min_write_pgs;
	int id = meta_line->id;
	int rq_len;
	int i, j;
	int ret;

	rqd = pblk_alloc_rqd(pblk, READ);
	if (IS_ERR(rqd)) {
		pr_err("pblk: cannot allocate write req.\n");
		return PTR_ERR(rqd);
	}
	m_ctx = nvm_rq_to_pdu(rqd);
	m_ctx->private = meta_line;

	rq_len = rq_ppas * geo->sec_size;
	data = ((void *)emeta->buf) + emeta->mem;

	bio = pblk_bio_map_addr(pblk, data, rq_ppas, rq_len,
					l_mg->emeta_alloc_type, GFP_KERNEL);
	if (IS_ERR(bio)) {
		ret = PTR_ERR(bio);
		goto fail_free_rqd;
	}
	bio->bi_iter.bi_sector = 0; /* internal bio */
	bio_set_op_attrs(bio, REQ_OP_WRITE, 0);
	rqd->bio = bio;

	ret = pblk_alloc_w_rq(pblk, rqd, rq_ppas, pblk_end_io_write_meta);
	if (ret)
		goto fail_free_bio;

	for (i = 0; i < rqd->nr_ppas; ) {
		spin_lock(&meta_line->lock);
		paddr = __pblk_alloc_page(pblk, meta_line, rq_ppas);
		spin_unlock(&meta_line->lock);
		for (j = 0; j < rq_ppas; j++, i++, paddr++)
			rqd->ppa_list[i] = addr_to_gen_ppa(pblk, paddr, id);
	}

	emeta->mem += rq_len;
	if (emeta->mem >= lm->emeta_len[0]) {
		spin_lock(&l_mg->close_lock[nrb]);
		list_del(&meta_line->list);
		WARN(!bitmap_full(meta_line->map_bitmap, lm->sec_per_line),
				"pblk: corrupt meta line %d\n", meta_line->id);
		spin_unlock(&l_mg->close_lock[nrb]);
	}

	pblk_down_page(pblk, rqd->ppa_list, rqd->nr_ppas);

	ret = pblk_submit_io(pblk, rqd);
	if (ret) {
		pr_err("pblk: emeta I/O submission failed: %d\n", ret);
		goto fail_rollback;
	}

	return NVM_IO_OK;

fail_rollback:
	pblk_up_page(pblk, rqd->ppa_list, rqd->nr_ppas);
	spin_lock(&l_mg->close_lock[nrb]);
	pblk_dealloc_page(pblk, meta_line, rq_ppas);
	list_add(&meta_line->list, &meta_line->list);
	spin_unlock(&l_mg->close_lock[nrb]);

	nvm_dev_dma_free(dev->parent, rqd->meta_list, rqd->dma_meta_list);
fail_free_bio:
	if (likely(l_mg->emeta_alloc_type == PBLK_VMALLOC_META))
		bio_put(bio);
fail_free_rqd:
	pblk_free_rqd(pblk, rqd, READ);
	return ret;
}

static int pblk_sched_meta_io(struct pblk *pblk, struct ppa_addr *prev_list,
			       int prev_n, unsigned int nrb)
{
	struct pblk_line_meta *lm = &pblk->lm;
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	struct pblk_line *meta_line;

	spin_lock(&l_mg->close_lock[nrb]);
retry:
	if (list_empty(&l_mg->emeta_list[nrb])) {
		spin_unlock(&l_mg->close_lock[nrb]);
		return 0;
	}

	meta_line = list_first_entry(&l_mg->emeta_list[nrb], struct pblk_line, list);
	if (bitmap_full(meta_line->map_bitmap, lm->sec_per_line)) {
		goto retry;
	}
	spin_unlock(&l_mg->close_lock[nrb]);

	if (!pblk_valid_meta_ppa(pblk, meta_line, prev_list, prev_n))
		return 0;

	return pblk_submit_meta_io(pblk, meta_line, nrb);
}

static int pblk_submit_io_set(struct pblk *pblk, struct nvm_rq *rqd, unsigned int nrb)
{
	struct pblk_c_ctx *c_ctx = nvm_rq_to_pdu(rqd);
	struct ppa_addr erase_ppa;
	int err;

	ppa_set_empty(&erase_ppa);

	//printk("submit_io_set 1 nrb=%d\nn", nrb);
	/* Assign lbas to ppas and populate request structure */
	err = pblk_setup_w_rq(pblk, rqd, c_ctx, &erase_ppa, nrb);
	if (err) {
		pr_err("pblk: could not setup write request: %d\n", err);
		return NVM_IO_ERR;
	}
	//printk("submit_io_set 2 nrb=%d\nn", nrb);

	if (likely(ppa_empty(erase_ppa))) {
		/* Submit metadata write for previous data line */
		err = pblk_sched_meta_io(pblk, rqd->ppa_list, rqd->nr_ppas, nrb);
		if (err) {
			pr_err("pblk_submit_io_set");
			pr_err("pblk: metadata I/O submission failed: %d", err);
			return NVM_IO_ERR;
		}
			//printk("submit_io_set 3 nrb=%d\nn", nrb);

		/* Submit data write for current data line */
		err = pblk_submit_io(pblk, rqd);
		if (err) {
			pr_err("pblk: data I/O submission failed: %d\n", err);
			return NVM_IO_ERR;
		}
	} else {
		//printk("submit_io_set 4 nrb=%d\nn", nrb);

		/* Submit data write for current data line */
		err = pblk_submit_io(pblk, rqd);
		if (err) {
			pr_err("pblk: data I/O submission failed: %d\n", err);
			return NVM_IO_ERR;
		}
			//printk("submit_io_set 5 nrb=%d\nn", nrb);

		/* Submit available erase for next data line */
		if (pblk_blk_erase_async(pblk, erase_ppa)) {
			struct pblk_line *e_line = pblk_line_get_erase(pblk, nrb);
			struct nvm_tgt_dev *dev = pblk->dev;
			struct nvm_geo *geo = &dev->geo;
			int bit;

			atomic_inc(&e_line->left_eblks);
			bit = pblk_ppa_to_pos(geo, erase_ppa);
			WARN_ON(!test_and_clear_bit(bit, e_line->erase_bitmap));
		}
	}

	//printk("submit_io_set 6 nrb=%d\nn", nrb);

	return NVM_IO_OK;
}

static void pblk_free_write_rqd(struct pblk *pblk, struct nvm_rq *rqd)
{
	struct pblk_c_ctx *c_ctx = nvm_rq_to_pdu(rqd);
	struct bio *bio = rqd->bio;

	if (c_ctx->nr_padded)
		pblk_bio_free_pages(pblk, bio, c_ctx->nr_valid,
							c_ctx->nr_padded);
}


static int pblk_submit_write(struct pblk *pblk, u8 nrb)
{
	struct bio *bio;
	struct nvm_rq *rqd;
	unsigned int secs_avail, secs_to_sync, secs_to_com;
	unsigned int secs_to_flush;
	unsigned long pos;
#ifdef CONFIG_PBLK_MULTIMAP
	unsigned int nr_user;
	int flags, i;
	struct pblk_rb *rb = &pblk->rb_ctx[nrb].rwb;
#endif

	/* If there are no sectors in the cache, flushes (bios without data)
	 * will be cleared on the cache threads
	 */
//	printk("pblk_submit_write:%d  nrb  = %d\n", 1, nrb);

	secs_avail = pblk_rb_read_count(rb);
	if (!secs_avail)
		return 1;
//	printk("pblk_submit_write:0 nrb=%u\n", nrb);

	secs_to_flush = pblk_rb_sync_point_count(rb);
#ifdef CONFIG_PBLK_MULTIMAP
	secs_avail = pblk_rb_read_count(rb);
	nr_user = READ_ONCE(rb->nr_user);
//	printk("pblk_submit_write1: nrb=%d secs_avail=%u secs_to_flush:%u nr_user=%u\n", nrb, secs_avail, secs_to_flush, nr_user);
	if (secs_avail > nr_user)
		secs_avail = secs_avail - nr_user;
	else
		secs_avail = 0;
	// [TODO] flush
#endif

	if (secs_avail < (pblk->min_write_pgs + 8))
		return 1;

	/*
	if (pblk->lstream[nrb].map_type == PAGE_MAP)
	{
		pos = READ_ONCE(rb->subm);

		for (i = 1; i < secs_avail; i++)
		{
			int temp_pos = (pos + secs_avail - i) & (rb->nr_entries - 1);
			entry = &rb->entries[temp_pos];
			flags = READ_ONCE(entry->w_ctx.flags);

			if (flags & PBLK_WRITTEN_DATA) {
				printk("written data secs_avail: %d i:%d temp_pos:%d\n", secs_avail, i, temp_pos);
				break;
			}
		}
		secs_avail = secs_avail - i;
		secs_avail = secs_avail - (secs_avail % 4);
	}

	if (secs_avail < (pblk->min_write_pgs + 8))
		return 1;
		*/
//	if (!secs_to_flush && (secs_avail < (pblk->min_write_pgs + 8)))
//		return 1;
	
	#ifdef CONFIG_PBLK_MULTIMAP
	if (nrb != GC_NRB) {
		struct pblk_rl *rl = &pblk->rl;
		if (pblk_rl_nr_free_blks(rl) <= 1 + atomic_long_read(&pblk->inflight_ws)) {
			if (atomic_long_read(&pblk->inflight_ws) > MAX_PSTREAM)
				printk("inflight_ws error:%lu\n", atomic_long_read(&pblk->inflight_ws));
			return 1;
		}
	}
	atomic_long_inc(&pblk->inflight_ws);
	#endif


	rqd = pblk_alloc_rqd(pblk, WRITE);
	rqd->nrb = nrb;
	if (IS_ERR(rqd)) {
		pr_err("pblk: cannot allocate write req.\n");
#ifdef CONFIG_PBLK_MULTIMAP
		atomic_long_dec(&pblk->inflight_ws);
#endif
		return 1;
	}

	bio = bio_alloc(GFP_KERNEL, pblk->max_write_pgs);
	if (!bio) {
		pr_err("pblk: cannot allocate write bio\n");
		goto fail_free_rqd;
	}
	bio->bi_iter.bi_sector = 0; /* internal bio */
	bio_set_op_attrs(bio, REQ_OP_WRITE, 0);
	rqd->bio = bio;

	secs_to_sync = pblk_calc_secs_to_sync(pblk, secs_avail, secs_to_flush);
	if (secs_to_sync > pblk->max_write_pgs) {
		pr_err("pblk: bad buffer sync calculation\n");
		goto fail_put_bio;
	}

	secs_to_com = (secs_to_sync > secs_avail) ? secs_avail : secs_to_sync;
	if (pblk->lstream[nrb].map_type == PAGE_MAP) {
		if (secs_to_com % 4 != 0)
			printk("secs_avail:%u secs_to_sync:%u secs_to_com:%u\n", 
					secs_avail, secs_to_sync, secs_to_com);
	}
	pos = pblk_rb_read_commit(rb, secs_to_com);

//	printk("pblk_submit_write:%d secs_to_com:%u\n", 4, secs_to_com);

	if (pblk_rb_read_to_bio(rb, rqd, bio, pos, secs_to_sync,
							secs_avail)) {
		pr_err("pblk: corrupted write bio\n");
		goto fail_put_bio;
	}
// 	printk("JJY: writer cpu %d rb %d pos %d\n", smp_processor_id(), nrb, pos);
	//printk("pblk_submit_write:5 nrb=%d\n", nrb);

	if (pblk_submit_io_set(pblk, rqd, nrb)) {
		if (nrb == 2) {
			printk("goto fail_free_bio\n");
		}
		goto fail_free_bio;
	}
	//printk("pblk_submit_write:6 nrb=%d\n", nrb);

// Debugging message: 210205
/*
	if (nrb == 2) {
		printk("secs_to_com: %d secs_to_sync: %d secs_avail:%d, nr_ppa:%d\n", secs_to_com, secs_to_sync, secs_avail, rqd->nr_ppas);
		for (i = 0; i < secs_to_com; i++) {
			int index = (pos + i) & (rb->nr_entries - 1);
			struct pblk_rb_entry *entry = &rb->entries[index];
			int loff = (entry->w_ctx.lba) & (4096-1);
			struct ppa_addr ppa = rqd->ppa_list[i];
			int poff = 0;
	        poff |= (ppa.g.pg << pblk->ppaf.pg_offset);
	        poff |= (ppa.g.lun << pblk->ppaf.lun_offset);
    	    poff |= (ppa.g.ch << pblk->ppaf.ch_offset);
    	    poff |= (ppa.g.pl << pblk->ppaf.pln_offset);
	        poff |= (ppa.g.sec << pblk->ppaf.sec_offset);

			printk("WRITE_THREAD: index:%d lba: %d, loff: %d ppa:%d\n", index, entry->w_ctx.lba, loff, poff);
		}
	}
*/
	

#ifdef CONFIG_NVM_DEBUG
#ifdef CONFIG_PBLK_MULTIMAP
	atomic_long_add(secs_to_sync, &pblk->sub_writes[nrb]);
#else
	atomic_long_add(secs_to_sync, &pblk->sub_writes);
#endif
#endif

#ifdef CONFIG_PBLK_MULTIMAP
	atomic_long_dec(&pblk->inflight_ws);
#endif

	return 0;

fail_free_bio:
	pblk_free_write_rqd(pblk, rqd);
fail_put_bio:
	bio_put(bio);
fail_free_rqd:
	pblk_free_rqd(pblk, rqd, WRITE);
#ifdef CONFIG_PBLK_MULTIMAP
	atomic_long_dec(&pblk->inflight_ws);
#endif

	return 1;
}

int pblk_write_ts(void *data)
{
	struct pblk_rb_ctx *rb_ctx = (struct pblk_rb_ctx *) data;
	struct pblk *pblk = rb_ctx->rwb.pblk;
	u8 nrb = rb_ctx->index;

	while (!kthread_should_stop()) {
		if (!pblk_submit_write(pblk, nrb))
			continue;
		set_current_state(TASK_INTERRUPTIBLE);
		io_schedule();
	}

	return 0;
}



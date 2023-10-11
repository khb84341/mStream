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
 * pblk-cache.c - pblk's write cache
 */

#include "pblk.h"

static int __pblk_write_to_cache(struct pblk *pblk, struct bio *bio, unsigned long flags, sector_t lba, int nr_entries)
{
	struct pblk_w_ctx w_ctx;
	unsigned int bpos, pos;
	int i, ret;
	unsigned int nrb;
	int user_rb_option = pblk->user_rb_option;
#ifndef CONFIG_PBLK_MULTIMAP
	int gc_rb_option = pblk->gc_rb_option;
#else
	int nr_entries_padding;
	int pos_index = 0;
	struct ppa_addr r_ppa;
	int nr_real_entries = 0;
	int reserved_ret;
	int force_reserved = 0;
	int start_i = 0;
	user_rb_option = 0;
#endif 

	/* Update the write buffer head (mem) with the entries that we can
	 * write. The write in itself cannot fail, so there is no need to
	 * rollback from here on.
	 */
#ifndef CONFIG_PBLK_MULTIMAP
retry:
	if (user_rb_option == 1) {
		preempt_disable();
		nrb = smp_processor_id();
	} else if (user_rb_option == 2) {
		nrb = pblk_rb_random(pblk->nr_rwb);
	} else if (user_rb_option == 3) {
		nrb = pblk_rb_remain_max(pblk, nr_entries);
	}
#else
#ifdef EXT4_TEST
	nrb = pblk_get_lstream(pblk, lba);
	if (nrb == DIR_STREAM) { 
		if (bio->bi_write_hint > 0)
			nrb = bio->bi_write_hint;
	}
#else
	nrb = pblk_get_lstream(pblk, lba);
#endif
	if (nrb > MAX_LSTREAM)
		nrb = 0;

//	printk("[pblk_write_to_cache:1] nrb=%d\n", nrb);
#endif

#ifndef CONFIG_PBLK_MULTIMAP
retry:
	ret = pblk_rb_may_write_user(&pblk->rb_ctx[nrb].rwb, nrb, bio, nr_entries, &bpos);
#else
	//printk("nrb:%d lba:%lu nr_padding(b):%u nr_entries:%u\n", nrb, lba, nr_entries_padding, nr_entries);
	//printk("nrb:%d lba:%lu nr_entries:%u\n", nrb, lba, nr_entries);

	nr_entries_padding = (((lba + nr_entries + 3) >> 2) - (lba >> 2)) << 2;

	if (nr_entries == 0)
		nr_entries_padding = nr_entries;
	else if ((pblk->lstream[nrb]).map_type != PAGE_MAP)
		nr_entries_padding = nr_entries;
	else {
		int found = 0;
		sector_t start_lba = lba >> 2;
		start_lba = start_lba << 2;

//		sector_t start_lba = lba;
	#ifdef CONFIG_PBLK_MULTIMAP
		spin_lock(&pblk->rb_ctx[nrb].lock);
	#endif	
		
		for (i = 0; i < nr_entries; i++)
		{
			reserved_ret = pblk_get_reserved_addr(pblk, lba + i, &r_ppa);
			if ((reserved_ret == 1) && (nrb != r_ppa.c.nrb))
			{
				reserved_ret = 0;
			}
			if ((i == 0) && (force_reserved == 1))
				reserved_ret = 0;

			//printk("before user write) lba:%lu, reserved_ret=%d\n", lba+i, reserved_ret);

			if (reserved_ret == 0) {
				found = 1;
				break;
			}
			else if (reserved_ret == 1)
			{
				void *data = bio_data(bio);
				w_ctx.lba = lba + i;
				w_ctx.flags = flags;
				pblk_ppa_set_empty(&w_ctx.ppa);

				if (r_ppa.c.is_cached == 0 || pblk_ppa_empty(r_ppa)) {
					BUG_ON(1);
				}
				pos = pblk_rb_wrap_pos(&pblk->rb_ctx[r_ppa.c.nrb].rwb, r_ppa.c.line);
				pblk_rb_write_entry_user_to_rrwb(&pblk->rb_ctx[r_ppa.c.nrb].rwb, data, w_ctx, pos);
				start_i = i + 1;
				bio_advance(bio, PBLK_EXPOSED_PAGE_SIZE);
			}

			if ((lba + i) % 4 == 3)
				break;
		}
		if (found == 0) {
			nr_entries_padding -= 4;
		}
	#ifdef CONFIG_PBLK_MULTIMAP
		spin_unlock(&pblk->rb_ctx[nrb].lock);
	#endif
		//printk("nrb:%d lba:%lu nr_padding(a):%u nr_entries:%u\n", nrb, lba, nr_entries_padding, nr_entries);
	}

retry:

	ret = pblk_rb_may_write_user(&pblk->rb_ctx[nrb].rwb, nrb, bio, nr_entries_padding, &bpos);
#endif

	switch (ret) {
	case NVM_IO_REQUEUE:
#ifndef CONFIG_PBLK_MULTIMAP
		if (user_rb_option == 1)
			preempt_enable();
#endif
		io_schedule();
		goto retry;
	case NVM_IO_ERR:
#ifndef CONFIG_PBLK_MULTIMAP
		if (user_rb_option == 1)
			preempt_enable();
#endif
		pblk_pipeline_stop(pblk);
		nr_entries_padding = 0;
		goto out;
	}

	//printk("pblk_write_to_cache_start: nrb=%d nr_entries:%u nr_padding:%u\n", nrb, nr_entries,
	//				nr_entries_padding);

	
	spin_lock(&pblk->rb_ctx[nrb].rwb.w_lock);
#ifndef CONFIG_PBLK_MULTIMAP
	pblk_rl_user_in(&pblk->rb_ctx[nrb].rb_rl, nr_entries);
#else
	pblk_rl_user_in(&pblk->rb_ctx[nrb].rb_rl, nr_entries_padding);
#endif
	spin_unlock(&pblk->rb_ctx[nrb].rwb.w_lock);


#ifndef CONFIG_PBLK_MULTIMAP
	if (user_rb_option == 1)
		preempt_enable();	// JJY: TODO: enable position
#endif
	
	if (start_i == nr_entries)
		goto out;

	if (unlikely(!bio_has_data(bio))) {
		#ifdef CONFIG_PBLK_MULTIMAP
//		printk("bio_has_data: nrb=%d\n", nrb);
		if (nr_entries_padding > 0)
			printk("bio_has_data: nrb=%d entries=%d\n" , nrb, nr_entries_padding);
		#endif
		goto out;
	}

#ifdef CONFIG_PBLK_MULTIMAP
	if (pblk->lstream[nrb].map_type == PAGE_MAP)
		spin_lock(&pblk->rb_ctx[nrb].lock);
#endif	
	w_ctx.flags = flags;
	pblk_ppa_set_empty(&w_ctx.ppa);
#ifdef CONFIG_PBLK_MULTIMAP
	pos_index = bpos;
#endif

	for (i = start_i; i < nr_entries; i++) {
		void *data = bio_data(bio);
#ifdef CONFIG_PBLK_MULTIMAP
//		printk("user write: %d trans_lock\n", i);
		reserved_ret = pblk_get_reserved_addr(pblk, lba + i, &r_ppa);
		if ((reserved_ret == 1) && (nrb != r_ppa.c.nrb))
		{
			printk("[cache] nrb:%d r_ppa.c.nrb:%d\n", nrb, r_ppa.c.nrb);
			reserved_ret = 0;
		}
		if ((i == 0) && (force_reserved == 1))
			reserved_ret = 0;

		if (reserved_ret == 0) {
			int j;
			sector_t start_lba = (lba + i) >> 2;
			start_lba = start_lba << 2;
			//printk("USER WRITE) rb_reserved_entry: lba:%lu pos_index:%d\n", lba + i, pos_index);
			nr_real_entries += 4;
			if (nr_entries_padding < nr_real_entries)
			{
				printk("nr_entrie_padding:%d nr_real_entries:%d\n", nr_entries_padding, nr_real_entries);
				BUG_ON(1);
			}
			spin_lock(&pblk->trans_lock);
			pblk->lstream[nrb].last_use_rrwb = start_lba;
			for (j = 0; j < 4; j++)
			{
				w_ctx.lba = start_lba + j;
				pos = pblk_rb_wrap_pos(&pblk->rb_ctx[nrb].rwb, pos_index + j);
				pblk_rb_reserved_entry(&pblk->rb_ctx[nrb].rwb, w_ctx, pos);
//				printk("pblk_write_to_cache(reserve):%lu nrb:%d, line:%u\n", start_lba+j, nrb, pos);
			}
			spin_unlock(&pblk->trans_lock);
			//printk("rb_reserved_entry trans_lock end\n");
			pos_index += 4;
			reserved_ret = pblk_get_reserved_addr(pblk, lba + i, &r_ppa);
		}

		if (reserved_ret == 1)
		{
			w_ctx.lba = lba + i;
			if (r_ppa.c.is_cached == 0 || pblk_ppa_empty(r_ppa)) {
				BUG_ON(1);
			}
			//printk("pblk_write_to_cache(use): %lu nrb:%d, line:%u\n", lba+i, r_ppa.c.nrb, r_ppa.c.line);
			pos = pblk_rb_wrap_pos(&pblk->rb_ctx[r_ppa.c.nrb].rwb, r_ppa.c.line);
			pblk_rb_write_entry_user_to_rrwb(&pblk->rb_ctx[r_ppa.c.nrb].rwb, data, w_ctx, pos);
		}
		else if (reserved_ret == 0) {
			BUG_ON(1);
		}
		else {
			w_ctx.lba = lba + i;
			pos = pblk_rb_wrap_pos(&pblk->rb_ctx[nrb].rwb, pos_index);
			pblk_rb_write_entry_user(&pblk->rb_ctx[nrb].rwb, data, w_ctx, pos);
			pos_index++;
			nr_real_entries += 1;
		}
#else
		w_ctx.lba = lba + i;

		pos = pblk_rb_wrap_pos(&pblk->rb_ctx[nrb].rwb, bpos + i);
		pblk_rb_write_entry_user(&pblk->rb_ctx[nrb].rwb, data, w_ctx, pos);
#endif

		bio_advance(bio, PBLK_EXPOSED_PAGE_SIZE);
	}

	if (nr_entries_padding > nr_real_entries)
	{
		printk("nrb:%d lba:%lu nr_entries:%u\n", nrb, lba, nr_entries);
		printk("cache_to_user(out): padding:%d real_entries:%d \n", nr_entries_padding, nr_real_entries);
		pblk_rl_out(&pblk->rb_ctx[nrb].rb_rl, nr_entries_padding - nr_real_entries, 0);
		nr_entries_padding = nr_real_entries;
//		BUG_ON(1);
	} else if (nr_entries_padding < nr_real_entries)
	{
		printk("cache_to_user(in): padding:%d real_entries:%d \n", nr_entries_padding, nr_real_entries);
		pblk_rl_user_in(&pblk->rb_ctx[nrb].rb_rl, nr_real_entries - nr_entries_padding);
		nr_entries_padding = nr_real_entries;
		BUG_ON(1);
	}
#ifdef CONFIG_PBLK_MULTIMAP
	if (pblk->lstream[nrb].map_type == PAGE_MAP) {
		spin_unlock(&pblk->rb_ctx[nrb].lock);
	}
#endif

#ifdef CONFIG_NVM_DEBUG
#ifdef CONFIG_PBLK_MULTIMAP
	atomic_long_add(nr_real_entries, &pblk->inflight_writes);
	atomic_long_add(nr_entries, &pblk->req_writes[nrb]);
	if (nrb > 0)
		atomic_long_add(nr_entries, &pblk->g_writes);
#endif
#endif


#ifdef CONFIG_PBLK_MULTIMAP
	pblk_rl_inserted(&pblk->rb_ctx[nrb].rb_rl, nr_entries_padding);
#else
	pblk_rl_inserted(&pblk->rb_ctx[nrb].rb_rl, nr_entries);
#endif
//	printk("JJY: cache cpu %d rb %d pos %d\n", smp_processor_id(), nrb, bpos);
out:
	//printk("[pblk_write_to_cache:end] nrb=%d\n", nrb);
#ifdef CONFIG_PBLK_MULTIMAP
	spin_lock(&pblk->rb_ctx[nrb].rwb.w_lock);
	if (nr_entries_padding > 0)
	{
		unsigned int nr_user = READ_ONCE(pblk->rb_ctx[nrb].rwb.nr_user);
		smp_store_release(&pblk->rb_ctx[nrb].rwb.nr_user, nr_user - nr_entries_padding);
	}
	spin_unlock(&pblk->rb_ctx[nrb].rwb.w_lock);
#endif

	pblk_write_should_kick(pblk, nrb);
	return ret;
}

int pblk_write_to_cache(struct pblk *pblk, struct bio *bio, unsigned long flags)
{
	sector_t lba = pblk_get_lba(bio);
	int total_entries = pblk_get_secs(bio);
	sector_t temp_lba = lba;
	int nr_entries = total_entries;
	int ret = NVM_IO_DONE;
	int unit = 4;

	if ((nr_entries <= 0)) {
//		printk("write_to_cache: case0 %lu %d\n", lba, nr_entries);
		ret = __pblk_write_to_cache(pblk, bio, flags, temp_lba, nr_entries);
		return ret;		
	}

	while (1)
	{
		if (nr_entries > unit) {
			int size = unit;
//			printk("write_to_cache: case1 %lu %lu %d %d %d\n", lba, temp_lba, loff, size, nr_entries);
			__pblk_write_to_cache(pblk, bio, flags, temp_lba, size);
			nr_entries = nr_entries - size;
			temp_lba = temp_lba + size;
		}
		else {
//			printk("write_to_cache: case2 %lu %lu %d %d\n", lba, temp_lba, loff, nr_entries);
			ret = __pblk_write_to_cache(pblk, bio, flags, temp_lba, nr_entries);
			break;
		}
	};

	return ret; 
}

/*
 * On GC the incoming lbas are not necessarily sequential. Also, some of the
 * lbas might not be valid entries, which are marked as empty by the GC thread
 */
int pblk_write_gc_to_cache(struct pblk *pblk, void *data, u64 *lba_list,
			   unsigned int nr_entries, unsigned int nr_rec_entries,
			   struct pblk_line *gc_line, unsigned long flags)
{
	struct pblk_w_ctx w_ctx;
	int i, valid_entries;
#ifndef CONFIG_PBLK_MULTIMAP
	unsigned int bpos, pos;
	unsigned int nrb;
	int gc_rb_option = pblk->gc_rb_option;
#else
	int gc_rb_option = 0;
	unsigned int bpos_sector;
	unsigned int pos;
	unsigned int nrb_sector = GC_NRB;
	unsigned int nr_sector = 0;
	int valid_sector = 0;
	char* copy_list;
	void *temp_data;
#endif
//	printk("JJY: pblk_write_gc_to_cache begin\n");
	/* Update the write buffer head (mem) with the entries that we can
	 * write. The write in itself cannot fail, so there is no need to
	 * rollback from here on.
	 */

#ifndef CONFIG_PBLK_MULTIMAP
retry:
	if (gc_rb_option == 1) {
		preempt_disable();
		nrb = smp_processor_id();
	}
	else if (gc_rb_option == 2) {
		nrb = pblk_rb_random(pblk->nr_rwb);
	}
	else if (gc_rb_option == 3) {
		nrb = pblk_rb_gc_remain_max(pblk, nr_entries);
	}
	else if (gc_rb_option == 4) {
		nrb = pblk->nr_rwb-1;
	}
error
#else
	if (pblk->lstream[gc_line->pstream].map_type == BLOCK_MAP) {
		printk("GC Target Change: %d->%d line %d\n", gc_line->pstream, 0, gc_line->id);
		BUG_ON(1);
	}

	copy_list = vzalloc(sizeof(char) * nr_entries);
    if (copy_list != NULL) {
		temp_data = data;
        for (i = 0, valid_entries = 0; i < nr_entries; i++) {
			int maptype;
            copy_list[i] = 0;
            if (lba_list[i] == ADDR_EMPTY) {
			    continue;
			}
			w_ctx.lba = lba_list[i];
			maptype = pblk_get_maptype(pblk, lba_list[i]);
			if (maptype == SECTOR_MAP) {	
				copy_list[i] = SECTOR_MAP;
				nr_sector++;
			}
			else {
				int ret_map = pblk_rb_gc_data_to_rrwb(pblk, temp_data, w_ctx, gc_line);
				if (ret_map == SECTOR_MAP) {
					copy_list[i] = SECTOR_MAP;
					nr_sector++;
				}
			}
			temp_data += PBLK_EXPOSED_PAGE_SIZE;
		}
	}
#endif

#ifndef CONFIG_PBLK_MULTIMAP
retry:
	if (!pblk_rb_may_write_gc(&pblk->rb_ctx[nrb].rwb, nrb, nr_rec_entries, &bpos)) {
		if (gc_rb_option == 1)
			preempt_enable();
		io_schedule();
		goto retry;
	}
	if (gc_rb_option == 1)
		preempt_enable();
#else
//	printk("gc cache\n");

retry_sector:
	if (!pblk_rb_may_write_gc(&pblk->rb_ctx[nrb_sector].rwb, nrb_sector, nr_sector, &bpos_sector)) {
		io_schedule();
		goto retry_sector;
	}
#endif

	w_ctx.flags = flags;
	pblk_ppa_set_empty(&w_ctx.ppa);

//	printk("JJY: pblk_write_gc_to_cache\n");
	for (i = 0; i < nr_entries; i++) {
		unsigned int nrb;
		if (lba_list[i] == ADDR_EMPTY) {
			continue;
		}
		if (copy_list[i] == 0) {
			data += PBLK_EXPOSED_PAGE_SIZE;
			continue;
		}
		
		spin_lock(&pblk->trans_lock);
		if (pblk->preinvalid_map[lba_list[i]] == 1) {
			atomic_long_inc(&pblk->preinvalid_gc[gc_line->pstream]);
			pblk->preinvalid_map[lba_list[i]] = 0;
		}
		spin_unlock(&pblk->trans_lock); 

		nrb = nrb_sector;
		pos = pblk_rb_wrap_pos(&pblk->rb_ctx[nrb].rwb, bpos_sector + valid_sector);
		valid_sector++;

		w_ctx.lba = lba_list[i];
		pblk_rb_write_entry_gc(&pblk->rb_ctx[nrb].rwb, data, w_ctx, gc_line, pos);

		data += PBLK_EXPOSED_PAGE_SIZE;
		valid_entries++;
	}

	if (copy_list != NULL)
		vfree(copy_list);

#ifndef CONFIG_PBLK_MULTIMAP
	WARN_ONCE(nr_rec_entries != valid_entries,
					"pblk: inconsistent GC write\n");
#else
	WARN_ONCE((nr_sector) != valid_entries,
					"pblk: inconsistent GC write\n");
#endif

#ifdef CONFIG_NVM_DEBUG
#ifdef CONFIG_PBLK_MULTIMAP
	atomic_long_add(valid_entries, &pblk->inflight_writes);
	atomic_long_add(valid_entries, &pblk->recov_gc_writes[gc_line->pstream]);
#else
	atomic_long_add(valid_entries, &pblk->inflight_writes);
	atomic_long_add(valid_entries, &pblk->recov_gc_writes);
#endif
#endif

#ifndef CONFIG_PBLK_MULTIMAP
	pblk_write_should_kick(pblk, nrb);
#else
	if (valid_sector > 0)
		pblk_write_should_kick(pblk, nrb_sector);
#endif
	
//	printk("gc end\n");

	return NVM_IO_OK;
}

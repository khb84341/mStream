/*
 * Copyright (C) 2016 CNEX Labs
 * Initial release: Javier Gonzalez <javier@cnexlabs.com>
 *
 * Based upon the circular ringbuffer.
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
 * pblk-rb.c - pblk's write buffer
 */

#include <linux/circ_buf.h>
#include <linux/random.h>

#include "pblk.h"

static DECLARE_RWSEM(pblk_rb_lock);

unsigned int pblk_rb_random(unsigned int nr_rwb)
{
        unsigned int nrb; 

        get_random_bytes(&nrb, sizeof(nrb));
        nrb %= nr_rwb;

        return nrb;
}

unsigned int pblk_rb_remain_max(struct pblk *pblk, int nr_entries, int nr_rwb)
{
 	unsigned int nrb, i;
	int remain_max, remain_cnt;
	struct pblk_rl *rl;
	struct pblk_rl_per_rb *rb_rl;

        nrb = 0;
	rl = &pblk->rl;	
	rb_rl = &pblk->rb_ctx[nrb].rb_rl;

        remain_max = rl->rb_user_max - atomic_read(&rb_rl->rb_user_cnt);

        for (i = nrb+1; i < nr_rwb; i++) {
		rb_rl = &pblk->rb_ctx[nrb].rb_rl;
                remain_cnt = rl->rb_user_max - atomic_read(&rb_rl->rb_user_cnt);
                if ( (remain_max >= nr_entries) && (remain_max < remain_cnt) ) {
                        nrb = i;
                        remain_max = remain_cnt;
                }
        }

        return nrb;
}

unsigned int pblk_rb_gc_remain_max(struct pblk *pblk, int nr_entries)
{
        unsigned int nrb, i;
        int remain_max, remain_cnt;
	struct pblk_rl *rl;
	struct pblk_rl_per_rb *rb_rl;

        nrb = 0;
	rl = &pblk->rl;	
	rb_rl = &pblk->rb_ctx[nrb].rb_rl;

        remain_max = rl->rb_gc_max - atomic_read(&rb_rl->rb_gc_cnt);

        for (i = nrb+1; i < pblk->nr_rwb; i++) {
		rb_rl = &pblk->rb_ctx[nrb].rb_rl;
                remain_cnt = rl->rb_gc_max - atomic_read(&rb_rl->rb_gc_cnt);
                if ( (remain_max >= nr_entries) && (remain_max < remain_cnt) ) {
                        nrb = i;
                        remain_max = remain_cnt;
                }
        }

	return nrb;
}

void pblk_rb_data_free(struct pblk_rb *rb)
{
	struct pblk_rb_pages *p, *t;

	down_write(&pblk_rb_lock);
	list_for_each_entry_safe(p, t, &rb->pages, list) {
		free_pages((unsigned long)page_address(p->pages), p->order);
		list_del(&p->list);
		kfree(p);
	}
	up_write(&pblk_rb_lock);
}

/*
 * Initialize ring buffer. The data and metadata buffers must be previously
 * allocated and their size must be a power of two
 * (Documentation/circular-buffers.txt)
 */
int pblk_rb_init(struct pblk_rb *rb, unsigned int nrb, struct pblk_rb_entry *rb_entry_base,
		 unsigned int power_size, unsigned int power_seg_sz)
{
	struct pblk *pblk = rb->pblk;
	unsigned int init_entry = 0;
	unsigned int alloc_order = power_size;
	unsigned int max_order = MAX_ORDER - 1;
	unsigned int order, iter;

	down_write(&pblk_rb_lock);
	rb->index = nrb;
	rb->entries = rb_entry_base;
	rb->seg_size = (1 << power_seg_sz);
	rb->nr_entries = (1 << power_size);
	rb->mem = rb->subm = rb->sync = rb->l2p_update = 0;
	rb->sync_point = EMPTY_ENTRY;
#ifdef CONFIG_PBLK_MULTIMAP
	rb->submit_point = 0;
	rb->nr_user = 0;
#endif

	spin_lock_init(&rb->w_lock);
	spin_lock_init(&rb->s_lock);

	INIT_LIST_HEAD(&rb->pages);

	if (alloc_order >= max_order) {
		order = max_order;
		iter = (1 << (alloc_order - max_order));
	} else {
		order = alloc_order;
		iter = 1;
	}

	do {
		struct pblk_rb_entry *entry;
		struct pblk_rb_pages *page_set;
		void *kaddr;
		unsigned long set_size;
		int i;

		page_set = kmalloc(sizeof(struct pblk_rb_pages), GFP_KERNEL);
		if (!page_set) {
			up_write(&pblk_rb_lock);
			return -ENOMEM;
		}

		page_set->order = order;
		page_set->pages = alloc_pages(GFP_KERNEL, order);
		if (!page_set->pages) {
			kfree(page_set);
			pblk_rb_data_free(rb);
			up_write(&pblk_rb_lock);
			return -ENOMEM;
		}
		kaddr = page_address(page_set->pages);

		entry = &rb->entries[init_entry];
		entry->data = kaddr;
		entry->cacheline = pblk_cacheline_to_addr(init_entry++, nrb);
		entry->w_ctx.flags = PBLK_WRITABLE_ENTRY;

		set_size = (1 << order);
		for (i = 1; i < set_size; i++) {
			entry = &rb->entries[init_entry];
			entry->cacheline = pblk_cacheline_to_addr(init_entry++, nrb);
			entry->data = kaddr + (i * rb->seg_size);
			entry->w_ctx.flags = PBLK_WRITABLE_ENTRY;
			bio_list_init(&entry->w_ctx.bios);
		}

		list_add_tail(&page_set->list, &rb->pages);
		iter--;
	} while (iter > 0);
	up_write(&pblk_rb_lock);

#ifdef CONFIG_NVM_DEBUG
	atomic_set(&rb->inflight_sync_point, 0);
#endif

	/*
	 * Initialize rate-limiter, which controls access to the write buffer
	 * but user and GC I/O
	 */
	pblk_rl_init(pblk, &pblk->rl, rb->nr_entries);

	return 0;
}

/*
 * pblk_rb_calculate_size -- calculate the size of the write buffer
 */
unsigned int pblk_rb_calculate_size(unsigned int nr_entries)
{
	/* Alloc a write buffer that can at least fit 128 entries */
	return (1 << max(get_count_order(nr_entries), 7));
}

void *pblk_rb_entries_ref(struct pblk_rb *rb)
{
	return rb->entries;
}

static void clean_wctx(struct pblk *pblk, struct pblk_rb *rb, unsigned int pos, struct pblk_w_ctx *w_ctx)
{
	int submit_flags;
	int count = 0;

try:
	submit_flags = READ_ONCE(w_ctx->submit_flags);

	if (!(submit_flags & PBLK_SUBMITTED_ENTRY)) {
		if (count == 20000) {
			int flags = READ_ONCE(w_ctx->flags);
			printk("clean_wctx: 0x%x 0x%x\n", submit_flags, flags);
			WARN_ONCE(1, "clean_wctx\n");
			count = 0;
		}
		else
			count ++;
		goto try;
	}

	/* Release flags on context. Protect from writes and reads */
	spin_lock(&pblk->trans_lock);
	smp_store_release(&w_ctx->flags, PBLK_WRITABLE_ENTRY);
	smp_store_release(&w_ctx->submit_flags, PBLK_WRITABLE_ENTRY);
	pblk_ppa_set_empty(&w_ctx->ppa);
	spin_unlock(&pblk->trans_lock);
	w_ctx->lba = ADDR_EMPTY;
}

#define pblk_rb_ring_count(head, tail, size) CIRC_CNT(head, tail, size)
#define pblk_rb_ring_space(rb, head, tail, size) \
					(CIRC_SPACE(head, tail, size))

/*
 * Buffer space is calculated with respect to the back pointer signaling
 * synchronized entries to the media.
 */
static unsigned int pblk_rb_space(struct pblk_rb *rb)
{
	unsigned int mem = READ_ONCE(rb->mem);
	unsigned int sync = READ_ONCE(rb->sync);

	return pblk_rb_ring_space(rb, mem, sync, rb->nr_entries);
}

/*
 * Buffer count is calculated with respect to the submission entry signaling the
 * entries that are available to send to the media
 */
unsigned int pblk_rb_read_count(struct pblk_rb *rb)
{
	unsigned int mem = READ_ONCE(rb->mem);
	unsigned int subm = READ_ONCE(rb->subm);

	return pblk_rb_ring_count(mem, subm, rb->nr_entries);
}

unsigned int pblk_rb_sync_count(struct pblk_rb *rb)
{
	unsigned int mem = READ_ONCE(rb->mem);
	unsigned int sync = READ_ONCE(rb->sync);

	return pblk_rb_ring_count(mem, sync, rb->nr_entries);
}

unsigned int pblk_rb_read_commit(struct pblk_rb *rb, unsigned int nr_entries)
{
	unsigned int subm;

	subm = READ_ONCE(rb->subm);
	/* Commit read means updating submission pointer */
	smp_store_release(&rb->subm,
				(subm + nr_entries) & (rb->nr_entries - 1));

	return subm;
}

static int __pblk_rb_update_l2p(struct pblk_rb *rb, unsigned int *l2p_upd,
				unsigned int to_update)
{
	struct pblk *pblk = rb->pblk;
	struct pblk_line *line;
	struct pblk_rb_entry *entry;
	struct pblk_w_ctx *w_ctx;
	unsigned int user_io = 0, gc_io = 0;
	unsigned int i;
	int flags;
	u8 nrb = rb->index;

	for (i = 0; i < to_update; i++) {
		entry = &rb->entries[*l2p_upd];
		w_ctx = &entry->w_ctx;

		flags = READ_ONCE(entry->w_ctx.flags);
		if (flags & PBLK_IOTYPE_USER)
			user_io++;
		else if (flags & PBLK_IOTYPE_GC)
			gc_io++;
		else
			WARN(1, "pblk: unknown IO type\n");

		pblk_update_map_dev(pblk, w_ctx->lba, w_ctx->ppa,
							entry->cacheline);

		line = &pblk->lines[pblk_tgt_ppa_to_line(w_ctx->ppa)];
		kref_put(&line->ref, pblk_line_put);
		clean_wctx(pblk, rb, *l2p_upd, w_ctx);
		*l2p_upd = (*l2p_upd + 1) & (rb->nr_entries - 1);
	}

	pblk_rl_out(&pblk->rb_ctx[nrb].rb_rl, user_io, gc_io);

	return 0;
}

/*
 * When we move the l2p_update pointer, we update the l2p table - lookups will
 * point to the physical address instead of to the cacheline in the write buffer
 * from this moment on.
 */
static int pblk_rb_update_l2p(struct pblk_rb *rb, unsigned int nr_entries,
			      unsigned int mem, unsigned int sync)
{
	unsigned int space, count;
	int ret = 0;

	lockdep_assert_held(&rb->w_lock);

	/* Update l2p only as buffer entries are being overwritten */
	space = pblk_rb_ring_space(rb, mem, rb->l2p_update, rb->nr_entries);
	// printk("JJY: rb->l2p_update: %d space: %d\n", rb->l2p_update, space);
	if (space > nr_entries)
		goto out;

	count = nr_entries - space;
	/* l2p_update used exclusively under rb->w_lock */
	ret = __pblk_rb_update_l2p(rb, &rb->l2p_update, count);
	// printk("JJY: rb->l2p_update: %d ret=%d\n", rb->l2p_update, ret);
		
out:
	return ret;
}

/*
 * Update the l2p entry for all sectors stored on the write buffer. This means
 * that all future lookups to the l2p table will point to a device address, not
 * to the cacheline in the write buffer.
 */
void pblk_rb_sync_l2p(struct pblk_rb *rb)
{
	unsigned int sync;
	unsigned int to_update;

	spin_lock(&rb->w_lock);

	/* Protect from reads and writes */
	sync = smp_load_acquire(&rb->sync);

	to_update = pblk_rb_ring_count(sync, rb->l2p_update, rb->nr_entries);
	__pblk_rb_update_l2p(rb, &rb->l2p_update, to_update);

	spin_unlock(&rb->w_lock);
}

/*
 * Write @nr_entries to ring buffer from @data buffer if there is enough space.
 * Typically, 4KB data chunks coming from a bio will be copied to the ring
 * buffer, thus the write will fail if not all incoming data can be copied.
 *
 */
static void __pblk_rb_write_entry(struct pblk_rb *rb, void *data,
				  struct pblk_w_ctx w_ctx,
				  struct pblk_rb_entry *entry)
{
	memcpy(entry->data, data, rb->seg_size);

	entry->w_ctx.lba = w_ctx.lba;
	entry->w_ctx.ppa = w_ctx.ppa;
}

void pblk_rb_write_entry_user(struct pblk_rb *rb, void *data,
			      struct pblk_w_ctx w_ctx, unsigned int ring_pos)
{
	struct pblk *pblk = rb->pblk;
	struct pblk_rb_entry *entry;
	int flags;

	entry = &rb->entries[ring_pos];
	flags = READ_ONCE(entry->w_ctx.flags);
#ifdef CONFIG_NVM_DEBUG
	/* Caller must guarantee that the entry is free */
	BUG_ON(!(flags & PBLK_WRITABLE_ENTRY));
#endif
	__pblk_rb_write_entry(rb, data, w_ctx, entry);

	pblk_update_map_cache(pblk, w_ctx.lba, entry->cacheline);
	flags = w_ctx.flags | PBLK_WRITTEN_DATA;

	/* Release flags on write context. Protect from writes */
	smp_store_release(&entry->w_ctx.flags, flags);
}

#ifdef CONFIG_PBLK_MULTIMAP
void pblk_rb_write_entry_user_to_rrwb(struct pblk_rb *rb, void *data,
			      struct pblk_w_ctx w_ctx, unsigned int ring_pos)
{
	struct pblk *pblk = rb->pblk;
	struct pblk_rb_entry *entry;
	int flags;
	struct ppa_addr l2p_ppa;

	entry = &rb->entries[ring_pos];
	flags = READ_ONCE(entry->w_ctx.flags);
#ifdef CONFIG_NVM_DEBUG
	/* Caller must guarantee that the entry is free */
	//printk("write_entry_user_to_rrwb: ring_pos:%u %x\n", ring_pos, flags); 
	if (!(flags & PBLK_WRITABLE_ENTRY)) {
		printk("ring_pos:%u %x\n", ring_pos, flags); 
//		spin_unlock(&pblk->trans_lock);
	}
	if (!(flags & PBLK_RESERVED_ENTRY)) {
		printk("no RESERVED_ENTRY ring_pos:%u %x\n", ring_pos, flags); 
	}
	BUG_ON(!(flags & PBLK_WRITABLE_ENTRY));
#endif
	memcpy(entry->data, data, rb->seg_size);
	if (entry->w_ctx.lba != w_ctx.lba) {
		printk("pblk_rb_write_entry_user1: entry_lba:%llu lba:%llu \n", entry->w_ctx.lba, w_ctx.lba);
//		spin_unlock(&pblk->trans_lock);
		BUG_ON(entry->w_ctx.lba != w_ctx.lba);
	}
	spin_lock(&pblk->trans_lock);
	l2p_ppa = entry->w_ctx.ppa;

	if (!pblk_addr_in_cache(l2p_ppa) && !pblk_ppa_empty(l2p_ppa)) {
		pblk_map_invalidate(pblk, l2p_ppa);
	}
	pblk_cache_map_set(pblk, w_ctx.lba, entry->cacheline);

	entry->w_ctx.lba = w_ctx.lba;
	entry->w_ctx.ppa = w_ctx.ppa;

	flags = w_ctx.flags | PBLK_WRITTEN_DATA;
	flags = flags & ~PBLK_RESERVED_ENTRY;

	smp_store_release(&entry->w_ctx.flags, flags);
	spin_unlock(&pblk->trans_lock);

}


void pblk_rb_write_read_data_to_rrwb(struct pblk_rb *rb, void *data,
			      struct pblk_w_ctx w_ctx, unsigned int ring_pos)
{
	struct pblk *pblk = rb->pblk;
	struct pblk_rb_entry *entry;
	int flags;

//	printk("write_read_data_to_rrwb trans_lock start\n");
	entry = &rb->entries[ring_pos];
	flags = READ_ONCE(entry->w_ctx.flags);
#ifdef CONFIG_NVM_DEBUG
	/* Caller must guarantee that the entry is free */
	// printk("write_entry_user_to_rrwb: ring_pos:%u %x\n", ring_pos, flags); 
//	BUG_ON(!(flags & PBLK_WRITABLE_ENTRY));
#endif

	if (flags & PBLK_WAITREAD_ENTRY) {
		spin_lock(&pblk->trans_lock);
		flags = READ_ONCE(entry->w_ctx.flags);
		if (flags & PBLK_WAITREAD_ENTRY) 
		{
			if (!pblk_addr_in_cache(entry->w_ctx.ppa) && !pblk_ppa_empty(entry->w_ctx.ppa))
				pblk_map_invalidate(pblk, entry->w_ctx.ppa);

			pblk_cache_map_set(pblk, w_ctx.lba, entry->cacheline);

			flags = flags | PBLK_WRITTEN_DATA;
			flags = flags & ~PBLK_WAITREAD_ENTRY;

			pblk_ppa_set_empty(&entry->w_ctx.ppa);
			smp_store_release(&entry->w_ctx.flags, flags);
			if (data != NULL)
				memcpy(entry->data, data, rb->seg_size);
			else 
				printk("warning: no data\n");

		}
		spin_unlock(&pblk->trans_lock);
	}

//	printk("write_read_data_to_rrwb trans_lock end\n");
}

void pblk_rb_reserved_entry(struct pblk_rb *rb,
			      struct pblk_w_ctx w_ctx, unsigned int ring_pos)
{
	struct pblk *pblk = rb->pblk;
	struct pblk_rb_entry *entry;
	int flags;
	struct ppa_addr r_addr;
	struct ppa_addr l2p_ppa;

	entry = &rb->entries[ring_pos];
	flags = READ_ONCE(entry->w_ctx.flags);
#ifdef CONFIG_NVM_DEBUG
	/* Caller must guarantee that the entry is free */
	if (!(flags & PBLK_WRITABLE_ENTRY)) {
		int submit_flags = READ_ONCE(entry->w_ctx.submit_flags);
		printk("reserved_entry: ring_pos:%u %x submit_flags:%x\n", ring_pos, flags, submit_flags); 
	}
	BUG_ON(!(flags & PBLK_WRITABLE_ENTRY));
#endif
	flags = PBLK_WRITABLE_ENTRY | w_ctx.flags;

	// printk("rb_reserved_entry:%lu\n", w_ctx.lba);
	l2p_ppa = pblk_trans_map_get(pblk, w_ctx.lba);
	
	entry->w_ctx.lba = w_ctx.lba;
	entry->w_ctx.ppa = l2p_ppa;

	if (pblk_addr_in_cache(l2p_ppa))
	{
		struct pblk_rb *old_rb = &pblk->rb_ctx[l2p_ppa.c.nrb].rwb;
		int old_pos = pblk_rb_wrap_pos(old_rb, l2p_ppa.c.line);
		struct pblk_rb_entry *old_entry = &old_rb->entries[old_pos];
		int old_flags = READ_ONCE(old_entry->w_ctx.flags);
	
		if (old_flags & PBLK_INVALIDATE_ENTRY);
		else if ((old_flags & PBLK_WRITTEN_DATA) || (old_flags & PBLK_SUBMITTED_ENTRY))
		{
			pblk_ppa_set_empty(&w_ctx.ppa);
			__pblk_rb_write_entry(rb, old_entry->data, w_ctx, entry);
			flags |= PBLK_WRITTEN_DATA;

			if (old_flags & PBLK_WRITTEN_DATA)	// WRITTEN or WRITTEN|RESERVED
			{
//				old_flags &= ~PBLK_RESERVED_ENTRY;
//				old_flags &= ~PBLK_WRITTEN_DATA;
//				old_flags |= PBLK_INVALIDATE_ENTRY;
//				smp_store_release(&old_entry->w_ctx.flags, old_flags);
			} 
			else if (old_flags & PBLK_SUBMITTED_ENTRY) {
//				old_flags |= PBLK_INVALIDATE_ENTRY;
//				smp_store_release(&old_entry->w_ctx.flags, old_flags);
			}
			else if ((old_flags & PBLK_RESERVED_ENTRY) && (old_flags & PBLK_SUBMITTED_ENTRY)) {
				printk("rb_resrved_entry: old_ring_pos:%u old_flags:0x%x ring_pos:%u\n", old_pos, old_flags, ring_pos);
				BUG_ON(1);
			}
		}	
		else if (old_flags & PBLK_RESERVED_ENTRY)
		{
			entry->w_ctx.ppa = old_entry->w_ctx.ppa;
			pblk_ppa_set_empty(&old_entry->w_ctx.ppa);
			old_flags &= ~PBLK_RESERVED_ENTRY;
			old_flags &= ~PBLK_WRITABLE_ENTRY;
			old_flags |= PBLK_INVALIDATE_ENTRY;
			old_flags |= PBLK_WRITTEN_DATA;
			smp_store_release(&old_entry->w_ctx.flags, old_flags);
		}
		else if (old_flags & PBLK_WAITREAD_ENTRY)
		{
			entry->w_ctx.ppa = old_entry->w_ctx.ppa;
			pblk_ppa_set_empty(&old_entry->w_ctx.ppa);
			old_flags &= ~PBLK_WAITREAD_ENTRY;
			old_flags |= PBLK_INVALIDATE_ENTRY;
			old_flags |= PBLK_WRITTEN_DATA;
			smp_store_release(&old_entry->w_ctx.flags, old_flags);
//			printk("wait read reserved: lba: %llu ring_pos:%u read padding\n", w_ctx.lba, ring_pos);
		}
		else {
			printk("rb_resrved_entry: old_ring_pos:%u old_flags:0x%x ring_pos:%u\n", old_pos, old_flags, ring_pos);
			BUG_ON(1);
		}
	}
	else {
		int line_id = pblk_tgt_ppa_to_line(l2p_ppa);
		pblk->lstream[rb->index].invalid_line = line_id;
	}

	if (pblk_addr_in_cache(entry->w_ctx.ppa))
	{			
		printk("cached:%d reserved:%d line:%u\n", entry->w_ctx.ppa.c.is_cached, 
							entry->w_ctx.ppa.c.is_reserved, (unsigned int)entry->w_ctx.ppa.c.line);
		BUG_ON(1);
	}
	
	r_addr = entry->cacheline;
	r_addr.c.is_reserved = 1;

	if ((w_ctx.lba % 4) != (ring_pos % 4)) {
		printk("reserved_align failed: lba:%llu ring_pos:%u\n", w_ctx.lba, ring_pos);
	}
	
	flags |= PBLK_RESERVED_ENTRY;
	pblk_cache_map_set(pblk, w_ctx.lba, r_addr);

	/* Release flags on write context. Protect from writes */
	smp_store_release(&entry->w_ctx.flags, flags);
}


int pblk_rb_gc_data_to_rrwb(struct pblk *pblk, 
				void *data, struct pblk_w_ctx w_ctx, struct pblk_line *gc_line)
{
	struct pblk_rb *rb; 
	struct pblk_rb_entry *entry;
	int flags;
	unsigned int pos; 
	struct ppa_addr l2p_ppa;
	int line_id;
	struct pblk_line *line; 

//	printk("rb_gc_data_to_rrwb trans_lock start\n");
	spin_lock(&pblk->trans_lock);

	l2p_ppa = pblk_trans_map_get(pblk, w_ctx.lba);

	if (pblk_ppa_empty(l2p_ppa)) {
		spin_unlock(&pblk->trans_lock);
//		printk("rb_gc_data_to_rrwb trans_lock1 end\n");
		atomic_long_inc(&pblk->nr_gcskip[gc_line->pstream]);
		return 0;
	}
	if (!pblk_addr_in_cache(l2p_ppa)) {
		spin_unlock(&pblk->trans_lock);
		//printk("rb_gc_data_to_rrwb trans_lock2 end\n");
		line_id = pblk_tgt_ppa_to_line(l2p_ppa);
		if (line_id != gc_line->id) {
			atomic_long_inc(&pblk->nr_gcskip[gc_line->pstream]);
			return 0;
		}
		return SECTOR_MAP;
	}
	
	rb = &pblk->rb_ctx[l2p_ppa.c.nrb].rwb;
	pos = pblk_rb_wrap_pos(rb, l2p_ppa.c.line);
	entry = &rb->entries[pos];

	flags = READ_ONCE(entry->w_ctx.flags);

	if ((flags & PBLK_WRITTEN_DATA));
	else if ((flags & PBLK_INVALIDATE_ENTRY));
	else if (flags & PBLK_RESERVED_ENTRY)
	{
		if (!pblk_addr_in_cache(entry->w_ctx.ppa) && !pblk_ppa_empty(entry->w_ctx.ppa)) {
			if ((pblk_tgt_ppa_to_line(entry->w_ctx.ppa) == gc_line->id)) {
				printk("gc reserved entry\n");
				pblk_map_invalidate(pblk, entry->w_ctx.ppa);
				memcpy(entry->data, data, rb->seg_size);
				pblk_ppa_set_empty(&entry->w_ctx.ppa);
				flags = flags | PBLK_WRITTEN_DATA;
				smp_store_release(&entry->w_ctx.flags, flags);
			}
		} 	
	}
	else if (flags & PBLK_WAITREAD_ENTRY)
	{
		if (!pblk_addr_in_cache(entry->w_ctx.ppa) && !pblk_ppa_empty(entry->w_ctx.ppa)) {
			if ((pblk_tgt_ppa_to_line(entry->w_ctx.ppa) == gc_line->id)) {
				printk("gc waitread entry\n");
				pblk_map_invalidate(pblk, entry->w_ctx.ppa);
				memcpy(entry->data, data, rb->seg_size);
				pblk_cache_map_set(pblk, w_ctx.lba, entry->cacheline);
				flags = flags | PBLK_WRITTEN_DATA;
				flags &= ~PBLK_WAITREAD_ENTRY;
				pblk_ppa_set_empty(&entry->w_ctx.ppa);
				smp_store_release(&entry->w_ctx.flags, flags);
			}
		}
	}
	else if (flags & PBLK_SUBMITTED_ENTRY)
	{
		spin_unlock(&pblk->trans_lock);
		// printk("rb_gc_data_to_rrwb3 trans_lock end\n");
		l2p_ppa = entry->w_ctx.ppa;
		if (pblk_ppa_empty(l2p_ppa)) {
			atomic_long_inc(&pblk->nr_gcskip[gc_line->pstream]);
			return 0;
		}
		line_id = pblk_tgt_ppa_to_line(l2p_ppa);
		line = &pblk->lines[line_id];
		if ((pblk_tgt_ppa_to_line(l2p_ppa) != gc_line->id)) {
			atomic_long_inc(&pblk->nr_gcskip[gc_line->pstream]);
			return 0;
		}
		return SECTOR_MAP;
	}
	// printk("rb_gc_data_to_rrwb4 trans_lock end\n");

	spin_unlock(&pblk->trans_lock);
	atomic_long_inc(&pblk->nr_gcskip[gc_line->pstream]);
	return 0; 
}
#endif

void pblk_rb_write_entry_gc(struct pblk_rb *rb, void *data,
			    struct pblk_w_ctx w_ctx, struct pblk_line *gc_line,
			    unsigned int ring_pos)
{
	struct pblk *pblk = rb->pblk;
	struct pblk_rb_entry *entry;
	int flags;
#ifdef CONFIG_PBLK_MULTIMAP
	struct ppa_addr rppa;
	pblk_ppa_set_empty(&rppa);
#endif 

	entry = &rb->entries[ring_pos];
	flags = READ_ONCE(entry->w_ctx.flags);
#ifdef CONFIG_NVM_DEBUG
	/* Caller must guarantee that the entry is free */
	if (!(flags & PBLK_WRITABLE_ENTRY)) {
		printk("ring_pos:%u %x\n", ring_pos, flags); 
		BUG_ON(!(flags & PBLK_WRITABLE_ENTRY));
	}
	BUG_ON(!(flags & PBLK_WRITABLE_ENTRY));
#endif

	__pblk_rb_write_entry(rb, data, w_ctx, entry);

	if (w_ctx.lba == ADDR_EMPTY)
		entry->w_ctx.lba = ADDR_EMPTY;	
	else if (!pblk_update_map_gc(pblk, w_ctx.lba, entry->cacheline, gc_line, &rppa))
		entry->w_ctx.lba = ADDR_EMPTY;

	flags = w_ctx.flags | PBLK_WRITTEN_DATA;

	/* Release flags on write context. Protect from writes */
	smp_store_release(&entry->w_ctx.flags, flags);
}

static int pblk_rb_sync_point_set(struct pblk_rb *rb, struct bio *bio,
				  unsigned int pos)
{
	struct pblk_rb_entry *entry;
	unsigned int subm, sync_point;
	int flags;

	subm = READ_ONCE(rb->subm);

#ifdef CONFIG_NVM_DEBUG
	atomic_inc(&rb->inflight_sync_point);
#endif

	if (pos == subm)
		return 0;

	sync_point = (pos == 0) ? (rb->nr_entries - 1) : (pos - 1);
	entry = &rb->entries[sync_point];

	flags = READ_ONCE(entry->w_ctx.flags);
	flags |= PBLK_FLUSH_ENTRY;

	/* Release flags on context. Protect from writes */
	smp_store_release(&entry->w_ctx.flags, flags);

	/* Protect syncs */
	smp_store_release(&rb->sync_point, sync_point);

	if (!bio)
		return 0;

	spin_lock_irq(&rb->s_lock);
	bio_list_add(&entry->w_ctx.bios, bio);
	spin_unlock_irq(&rb->s_lock);

	return 1;
}

static int __pblk_rb_may_write(struct pblk_rb *rb, unsigned int nr_entries,
			       unsigned int *pos)
{
	unsigned int mem;
	unsigned int sync;

	sync = READ_ONCE(rb->sync);
	mem = READ_ONCE(rb->mem);

	if (pblk_rb_ring_space(rb, mem, sync, rb->nr_entries) < nr_entries) {
//		printk("__pblk_rb_may_write1: space:%u needs:%u mem:%u sync:%u\n", (pblk_rb_ring_space(rb, mem, sync, rb->nr_entries)), nr_entries, mem, sync);
		return 0;
	}

//	printk("__pblk_rb_may_write2: space:%u needs:%u mem:%u sync:%u\n", (pblk_rb_ring_space(rb, mem, sync, rb->nr_entries)), nr_entries, mem, sync);

	if (pblk_rb_update_l2p(rb, nr_entries, mem, sync))
		return 0;

	*pos = mem;

	return 1;
}

static int pblk_rb_may_write(struct pblk_rb *rb, unsigned int nr_entries,
			     unsigned int *pos)
{
	if (!__pblk_rb_may_write(rb, nr_entries, pos))
		return 0;

	/* Protect from read count */
	smp_store_release(&rb->mem, (*pos + nr_entries) & (rb->nr_entries - 1));
	// printk("pblk_rb_may_write: pos:%u nr_entries:%u next_pos:%u\n", *pos, nr_entries, (*pos+nr_entries));

	return 1;
}

void pblk_rb_flush(struct pblk_rb *rb, unsigned int nrb)
{
	struct pblk *pblk = rb->pblk;
	unsigned int mem = READ_ONCE(rb->mem);

	if (pblk_rb_sync_point_set(rb, NULL, mem))
		return;

	pblk_write_should_kick(pblk, nrb);
}

static int pblk_rb_may_write_flush(struct pblk_rb *rb, unsigned int nr_entries,
				   unsigned int *pos, struct bio *bio,
				   int *io_ret)
{
	unsigned int mem;
#ifdef CONFIG_PBLK_MULTIMAP
	unsigned int nr_user;
#endif

	if (!__pblk_rb_may_write(rb, nr_entries, pos))
		return 0;

	mem = (*pos + nr_entries) & (rb->nr_entries - 1);
	*io_ret = NVM_IO_DONE;

#ifndef CONFIG_PBLK_MULTIMAP
	if (bio->bi_opf & REQ_PREFLUSH) {
		struct pblk *pblk = rb->pblk;

#ifdef CONFIG_NVM_DEBUG
		atomic_long_inc(&pblk->nr_flush);
#endif
		if (pblk_rb_sync_point_set(rb, bio, mem))
			*io_ret = NVM_IO_OK;
	}
#endif
	nr_user = READ_ONCE(rb->nr_user);
	smp_store_release(&rb->nr_user, nr_user + nr_entries);
//	printk("may_write nr_user=%u nr_entries_padding=%d\n", nr_user, nr_entries);
	/* Protect from read count */
	smp_store_release(&rb->mem, mem);
//	printk("*pblk_rb_may_write_flush: pos:%u nr_entries:%u next_pos:%u\n", *pos, nr_entries, mem);
	return 1;
}

/*
 * Atomically check that (i) there is space on the write buffer for the
 * incoming I/O, and (ii) the current I/O type has enough budget in the write
 * buffer (rate-limiter).
 */
int pblk_rb_may_write_user(struct pblk_rb *rb, unsigned int nrb, struct bio *bio,
			   unsigned int nr_entries, unsigned int *pos)
{
	struct pblk *pblk = rb->pblk;
	int io_ret;

	spin_lock(&rb->w_lock);
	io_ret = pblk_rl_user_may_insert(&pblk->rl, &pblk->rb_ctx[nrb].rb_rl, nr_entries);
	if (io_ret) {
		spin_unlock(&rb->w_lock);
		return io_ret;
	}

	if (!pblk_rb_may_write_flush(rb, nr_entries, pos, bio, &io_ret)) {
		spin_unlock(&rb->w_lock);
		return NVM_IO_REQUEUE;
	}
	
//	pblk_rl_user_in(&pblk->rb_ctx[nrb].rb_rl, nr_entries);
	spin_unlock(&rb->w_lock);

	return io_ret;
}

/*
 * Look at pblk_rb_may_write_user comment
 */
int pblk_rb_may_write_gc(struct pblk_rb *rb, unsigned int nrb, unsigned int nr_entries,
			 unsigned int *pos)
{
	struct pblk *pblk = rb->pblk;

	spin_lock(&rb->w_lock);
	if (!pblk_rl_gc_may_insert(&pblk->rl, &pblk->rb_ctx[nrb].rb_rl, nr_entries)) {
		spin_unlock(&rb->w_lock);
		return 0;
	}

	if (!pblk_rb_may_write(rb, nr_entries, pos)) {
		spin_unlock(&rb->w_lock);
		return 0;
	}

	pblk_rl_gc_in(&pblk->rb_ctx[nrb].rb_rl, nr_entries);
	spin_unlock(&rb->w_lock);

	return 1;
}

/*
 * The caller of this function must ensure that the backpointer will not
 * overwrite the entries passed on the list.
 */
unsigned int pblk_rb_read_to_bio_list(struct pblk_rb *rb, struct bio *bio,
				      struct list_head *list,
				      unsigned int max)
{
	struct pblk_rb_entry *entry, *tentry;
	struct page *page;
	unsigned int read = 0;
	int ret;

	list_for_each_entry_safe(entry, tentry, list, index) {
		if (read > max) {
			pr_err("pblk: too many entries on list\n");
			goto out;
		}

		page = virt_to_page(entry->data);
		if (!page) {
			pr_err("pblk: could not allocate write bio page\n");
			goto out;
		}

		ret = bio_add_page(bio, page, rb->seg_size, 0);
		if (ret != rb->seg_size) {
			pr_err("pblk: could not add page to write bio\n");
			goto out;
		}

		list_del(&entry->index);
		read++;
	}

out:
	return read;
}

/*
 * Read available entries on rb and add them to the given bio. To avoid a memory
 * copy, a page reference to the write buffer is used to be added to the bio.
 *
 * This function is used by the write thread to form the write bio that will
 * persist data on the write buffer to the media.
 */
unsigned int pblk_rb_read_to_bio(struct pblk_rb *rb, struct nvm_rq *rqd,
				 struct bio *bio, unsigned int pos,
				 unsigned int nr_entries, unsigned int count)
{
	struct pblk *pblk = rb->pblk;
	struct request_queue *q = pblk->dev->q;
	struct pblk_c_ctx *c_ctx = nvm_rq_to_pdu(rqd);
	struct pblk_rb_entry *entry;
	struct page *page;
	unsigned int pad = 0, to_read = nr_entries;
	unsigned int i;
	int flags;

	if (count < nr_entries) {
		pad = nr_entries - count;
		to_read = count;
	}

	c_ctx->sentry = pos;
	c_ctx->nr_valid = to_read;
	c_ctx->nr_padded = pad;

//	printk("pblk_rb_read_to_read: pos:%u to_read:%u nr_padded:%u\n", pos, to_read, pad);

	for (i = 0; i < to_read; i++) {
		int try_count = 0;
		int visit = 0;
		entry = &rb->entries[pos];

		/* A write has been allowed into the buffer, but data is still
		 * being copied to it. It is ok to busy wait.
		 */
try:
		flags = READ_ONCE(entry->w_ctx.flags);
		if (!(flags & PBLK_WRITTEN_DATA) && !(flags & PBLK_INVALIDATE_ENTRY)){
			if (flags & PBLK_RESERVED_ENTRY){
				BUG_ON(1);
			}
			if (try_count == 200000 && visit == 0)
			{
				visit = 1; 
				printk("read_to_bio flag:%x\n", flags);
//				WARN_ONCE(1, "read_to_bio\n");
				try_count = 0;
//				BUG_ON(1);
			}
			else
				try_count++;

			io_schedule();
			goto try;
		}
		flags &= ~PBLK_RESERVED_ENTRY;

#ifdef GC_TIME
		entry->w_ctx.write_time = ktime_get();
#endif
		page = virt_to_page(entry->data);
		if (!page) {
			pr_err("pblk: could not allocate write bio page\n");
			flags &= ~PBLK_WRITTEN_DATA;
			flags |= PBLK_SUBMITTED_ENTRY;
			/* Release flags on context. Protect from writes */
			smp_store_release(&entry->w_ctx.flags, flags);
			return NVM_IO_ERR;
		}

		if (bio_add_pc_page(q, bio, page, rb->seg_size, 0) !=
								rb->seg_size) {
			pr_err("pblk: could not add page to write bio\n");
			flags &= ~PBLK_WRITTEN_DATA;
			flags |= PBLK_SUBMITTED_ENTRY;
			/* Release flags on context. Protect from writes */
			smp_store_release(&entry->w_ctx.flags, flags);
//			printk("pblk_rb_read_to_bio2 trans_lock end\n");
			return NVM_IO_ERR;
		}

		if (flags & PBLK_FLUSH_ENTRY) {
			unsigned int sync_point;

			sync_point = READ_ONCE(rb->sync_point);
			if (sync_point == pos) {
				/* Protect syncs */
				smp_store_release(&rb->sync_point, EMPTY_ENTRY);
			}
			flags &= ~PBLK_FLUSH_ENTRY;
#ifdef CONFIG_NVM_DEBUG
			atomic_dec(&rb->inflight_sync_point);
#endif
		}
		flags &= ~PBLK_WRITTEN_DATA;
		flags |= PBLK_SUBMITTED_ENTRY;
		smp_store_release(&rb->submit_point, pos);

		/* Release flags on context. Protect from writes */
		smp_store_release(&entry->w_ctx.flags, flags);
		smp_store_release(&entry->w_ctx.submit_flags, PBLK_SUBMITTED_ENTRY);
		pos = (pos + 1) & (rb->nr_entries - 1);
	}

	if (pad) {
		if (pblk_bio_add_pages(pblk, bio, GFP_KERNEL, pad)) {
			pr_err("pblk: could not pad page in write bio\n");
			return NVM_IO_ERR;
		}
	}

#ifdef CONFIG_NVM_DEBUG
	atomic_long_add(pad, &(rb->pblk->padded_writes));
#endif

	return NVM_IO_OK;
}

/*
 * Copy to bio only if the lba matches the one on the given cache entry.
 * Otherwise, it means that the entry has been overwritten, and the bio should
 * be directed to disk.
 */
int pblk_rb_copy_to_bio(struct pblk_rb *rb, struct bio *bio, sector_t lba,
			struct ppa_addr ppa, int bio_iter, bool advanced_bio)
{
	struct pblk *pblk = rb->pblk;
	struct pblk_rb_entry *entry;
	struct pblk_w_ctx *w_ctx;
	struct ppa_addr l2p_ppa;
	u64 pos = pblk_addr_to_cacheline(ppa);
	void *data;
	int flags;
	int ret = 1;


#ifdef CONFIG_NVM_DEBUG
	/* Caller must ensure that the access will not cause an overflow */
	BUG_ON(pos >= rb->nr_entries);
#endif
	entry = &rb->entries[pos];
	w_ctx = &entry->w_ctx;
	flags = READ_ONCE(w_ctx->flags);

	spin_lock(&rb->w_lock);
	spin_lock(&pblk->trans_lock);
//	printk("pblk_rb_copy_to_bio: lba:%lu\n", lba);
	l2p_ppa = pblk_trans_map_get(pblk, lba);
//	printk("pblk_rb_copy_to_bio: trans_lock end\n");
	spin_unlock(&pblk->trans_lock);

	/* Check if the entry has been overwritten or is scheduled to be */
	if (!pblk_ppa_comp(l2p_ppa, ppa) || w_ctx->lba != lba ||
						flags & PBLK_WRITABLE_ENTRY) {
		ret = 0;
		goto out;
	}

	/* Only advance the bio if it hasn't been advanced already. If advanced,
	 * this bio is at least a partial bio (i.e., it has partially been
	 * filled with data from the cache). If part of the data resides on the
	 * media, we will read later on
	 */
	if (unlikely(!advanced_bio))
		bio_advance(bio, bio_iter * PBLK_EXPOSED_PAGE_SIZE);

	data = bio_data(bio);
	memcpy(data, entry->data, rb->seg_size);

out:
	spin_unlock(&rb->w_lock);
	return ret;
}

struct pblk_w_ctx *pblk_rb_w_ctx(struct pblk_rb *rb, unsigned int pos)
{
	unsigned int entry = pos & (rb->nr_entries - 1);

	return &rb->entries[entry].w_ctx;
}

unsigned int pblk_rb_sync_init(struct pblk_rb *rb, unsigned long *flags)
	__acquires(&rb->s_lock)
{
	if (flags)
		spin_lock_irqsave(&rb->s_lock, *flags);
	else
		spin_lock_irq(&rb->s_lock);

	return rb->sync;
}

void pblk_rb_sync_end(struct pblk_rb *rb, unsigned long *flags)
	__releases(&rb->s_lock)
{
	lockdep_assert_held(&rb->s_lock);

	if (flags)
		spin_unlock_irqrestore(&rb->s_lock, *flags);
	else
		spin_unlock_irq(&rb->s_lock);
}

unsigned int pblk_rb_sync_advance(struct pblk_rb *rb, unsigned int nr_entries)
{
	unsigned int sync;
	unsigned int i;

	lockdep_assert_held(&rb->s_lock);

	sync = READ_ONCE(rb->sync);

	for (i = 0; i < nr_entries; i++)
		sync = (sync + 1) & (rb->nr_entries - 1);

	/* Protect from counts */
	smp_store_release(&rb->sync, sync);

	return sync;
}

unsigned int pblk_rb_sync_point_count(struct pblk_rb *rb)
{
	unsigned int subm, sync_point;
	unsigned int count;

	/* Protect syncs */
	sync_point = smp_load_acquire(&rb->sync_point);
	if (sync_point == EMPTY_ENTRY)
		return 0;

	subm = READ_ONCE(rb->subm);

	/* The sync point itself counts as a sector to sync */
	count = pblk_rb_ring_count(sync_point, subm, rb->nr_entries) + 1;

	return count;
}

/*
 * Scan from the current position of the sync pointer to find the entry that
 * corresponds to the given ppa. This is necessary since write requests can be
 * completed out of order. The assumption is that the ppa is close to the sync
 * pointer thus the search will not take long.
 *
 * The caller of this function must guarantee that the sync pointer will no
 * reach the entry while it is using the metadata associated with it. With this
 * assumption in mind, there is no need to take the sync lock.
 */
struct pblk_rb_entry *pblk_rb_sync_scan_entry(struct pblk_rb *rb,
					      struct ppa_addr *ppa)
{
	unsigned int sync, subm, count;
	unsigned int i;

	sync = READ_ONCE(rb->sync);
	subm = READ_ONCE(rb->subm);
	count = pblk_rb_ring_count(subm, sync, rb->nr_entries);

	for (i = 0; i < count; i++)
		sync = (sync + 1) & (rb->nr_entries - 1);

	return NULL;
}

int pblk_rb_tear_down_check(struct pblk_rb *rb)
{
	struct pblk_rb_entry *entry;
	int i;
	int ret = 0;

	spin_lock(&rb->w_lock);
	spin_lock_irq(&rb->s_lock);

	if ((rb->mem == rb->subm) && (rb->subm == rb->sync) &&
				(rb->sync == rb->l2p_update) &&
				(rb->sync_point == EMPTY_ENTRY)) {
		goto out;
	}

	if (!rb->entries) {
		ret = 1;
		goto out;
	}

	for (i = 0; i < rb->nr_entries; i++) {
		entry = &rb->entries[i];

		if (!entry->data) {
			ret = 1;
			goto out;
		}
	}

out:
	spin_unlock_irq(&rb->s_lock);
	spin_unlock(&rb->w_lock);

	return ret;
}

unsigned int pblk_rb_wrap_pos(struct pblk_rb *rb, unsigned int pos)
{
	return (pos & (rb->nr_entries - 1));
}

int pblk_rb_pos_oob(struct pblk_rb *rb, u64 pos)
{
	return (pos >= rb->nr_entries);
}

ssize_t pblk_rb_sysfs(struct pblk_rb *rb, char *buf)
{
	struct pblk *pblk = rb->pblk;
	struct pblk_c_ctx *c;
	ssize_t offset;
	int queued_entries = 0;
	u8 index = rb->index;

	spin_lock_irq(&rb->s_lock);
	list_for_each_entry(c, &pblk->rb_ctx[index].compl_list, list)
	queued_entries++;
	spin_unlock_irq(&rb->s_lock);

	if (rb->sync_point != EMPTY_ENTRY)
		offset = scnprintf(buf, PAGE_SIZE,
			"%u\t%u\t%u\t%u\t%u\t%u\t%u - %u/%u/%u - %d\n",
			rb->nr_entries,
			rb->mem,
			rb->subm,
			rb->sync,
			rb->l2p_update,
#ifdef CONFIG_NVM_DEBUG
			atomic_read(&rb->inflight_sync_point),
#else
			0,
#endif
			rb->sync_point,
			pblk_rb_read_count(rb),
			pblk_rb_space(rb),
			pblk_rb_sync_point_count(rb),
			queued_entries);
	else
		offset = scnprintf(buf, PAGE_SIZE,
			"%u\t%u\t%u\t%u\t%u\t%u\tNULL - %u/%u/%u - %d\n",
			rb->nr_entries,
			rb->mem,
			rb->subm,
			rb->sync,
			rb->l2p_update,
#ifdef CONFIG_NVM_DEBUG
			atomic_read(&rb->inflight_sync_point),
#else
			0,
#endif
			pblk_rb_read_count(rb),
			pblk_rb_space(rb),
			pblk_rb_sync_point_count(rb),
			queued_entries);

	return offset;
}

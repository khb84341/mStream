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
 * Implementation of a physical block-device target for Open-channel SSDs.
 *
 * pblk-sysfs.c - pblk's sysfs
 *
 */

#include "pblk.h"

static ssize_t pblk_sysfs_luns_show(struct pblk *pblk, char *page)
{
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;
	struct pblk_lun *rlun;
	ssize_t sz = 0;
	int i;

	for (i = 0; i < geo->nr_luns; i++) {
		int active = 1;

		rlun = &pblk->luns[i];
		if (!down_trylock(&rlun->wr_sem)) {
			active = 0;
			up(&rlun->wr_sem);
		}
		sz += snprintf(page + sz, PAGE_SIZE - sz,
				"pblk: pos:%d, ch:%d, lun:%d - %d\n",
					i,
					rlun->bppa.g.ch,
					rlun->bppa.g.lun,
					active);
	}

	return sz;
}

// JJY: TODO: rl
static ssize_t pblk_sysfs_rate_limiter(struct pblk *pblk, char *page)
{
	int free_blocks, total_blocks;
	int rb_user_max, rb_user_cnt;
	int rb_gc_max, rb_gc_cnt, rb_budget, rb_state;

	free_blocks = atomic_read(&pblk->rl.free_blocks);
	rb_user_max = pblk->rl.rb_user_max;
	rb_user_cnt = atomic_read(&pblk->rb_ctx[0].rb_rl.rb_user_cnt);
	rb_gc_max = pblk->rl.rb_gc_max;
	rb_gc_cnt = atomic_read(&pblk->rb_ctx[0].rb_rl.rb_gc_cnt);
	rb_budget = pblk->rl.rb_budget;
	rb_state = pblk->rl.rb_state;

	total_blocks = pblk->rl.total_blocks;

	return snprintf(page, PAGE_SIZE,
		"u:%u/%u,gc:%u/%u(%u/%u)(stop:<%u,full:>%u,free:%d/%d)-%d\n",
				rb_user_cnt,
				rb_user_max,
				rb_gc_cnt,
				rb_gc_max,
				rb_state,
				rb_budget,
				pblk->rl.low,
				pblk->rl.high,
				free_blocks,
				total_blocks,
				READ_ONCE(pblk->rb_ctx[0].rb_rl.rb_user_active));
}

static ssize_t pblk_sysfs_gc_state_show(struct pblk *pblk, char *page)
{
	int gc_enabled, gc_active;

	pblk_gc_sysfs_state_show(pblk, &gc_enabled, &gc_active);
	return snprintf(page, PAGE_SIZE, "gc_enabled=%d, gc_active=%d\n",
					gc_enabled, gc_active);
}

static ssize_t pblk_sysfs_stats(struct pblk *pblk, char *page)
{
	ssize_t sz;

	sz = snprintf(page, PAGE_SIZE,
			"read_failed=%lu, read_high_ecc=%lu, read_empty=%lu, read_failed_gc=%lu, write_failed=%lu, erase_failed=%lu\n",
			atomic_long_read(&pblk->read_failed),
			atomic_long_read(&pblk->read_high_ecc),
			atomic_long_read(&pblk->read_empty),
			atomic_long_read(&pblk->read_failed_gc),
			atomic_long_read(&pblk->write_failed),
			atomic_long_read(&pblk->erase_failed));

	return sz;
}

// JJY: TODO: rb
static ssize_t pblk_sysfs_write_buffer(struct pblk *pblk, char *page)
{
	return pblk_rb_sysfs(&pblk->rb_ctx[0].rwb, page);
}

static ssize_t pblk_sysfs_ppaf(struct pblk *pblk, char *page)
{
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;
	ssize_t sz = 0;

	sz = snprintf(page, PAGE_SIZE - sz,
		"g:(b:%d)blk:%d/%d,pg:%d/%d,lun:%d/%d,ch:%d/%d,pl:%d/%d,sec:%d/%d\n",
		pblk->ppaf_bitsize,
		pblk->ppaf.blk_offset, geo->ppaf.blk_len,
		pblk->ppaf.pg_offset, geo->ppaf.pg_len,
		pblk->ppaf.lun_offset, geo->ppaf.lun_len,
		pblk->ppaf.ch_offset, geo->ppaf.ch_len,
		pblk->ppaf.pln_offset, geo->ppaf.pln_len,
		pblk->ppaf.sec_offset, geo->ppaf.sect_len);

	sz += snprintf(page + sz, PAGE_SIZE - sz,
		"d:blk:%d/%d,pg:%d/%d,lun:%d/%d,ch:%d/%d,pl:%d/%d,sec:%d/%d\n",
		geo->ppaf.blk_offset, geo->ppaf.blk_len,
		geo->ppaf.pg_offset, geo->ppaf.pg_len,
		geo->ppaf.lun_offset, geo->ppaf.lun_len,
		geo->ppaf.ch_offset, geo->ppaf.ch_len,
		geo->ppaf.pln_offset, geo->ppaf.pln_len,
		geo->ppaf.sect_offset, geo->ppaf.sect_len);

	return sz;
}

static ssize_t pblk_sysfs_lines(struct pblk *pblk, char *page)
{
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;
	struct pblk_line_meta *lm = &pblk->lm;
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	struct pblk_line *line;
	ssize_t sz = 0;
	int nr_free_lines;
	int cur_data, cur_log;
	int free_line_cnt = 0, closed_line_cnt = 0, emeta_line_cnt = 0;
	int d_line_cnt = 0, l_line_cnt = 0;
	int gc_full = 0, gc_high = 0, gc_mid = 0, gc_low = 0, gc_empty = 0;
	int bad = 0, cor = 0;
	int msecs = 0, cur_sec = 0, vsc = 0, sec_in_line = 0;
	int map_weight = 0, meta_weight = 0;

	spin_lock(&l_mg->free_lock);
	cur_data = (l_mg->data_line[0]) ? l_mg->data_line[0]->id : -1;
	cur_log = (l_mg->log_line) ? l_mg->log_line->id : -1;
	nr_free_lines = l_mg->nr_free_lines;

	list_for_each_entry(line, &l_mg->free_list, list)
		free_line_cnt++;
	spin_unlock(&l_mg->free_lock);

	spin_lock(&l_mg->close_lock[0]);
	list_for_each_entry(line, &l_mg->emeta_list[0], list)
		emeta_line_cnt++;
	spin_unlock(&l_mg->close_lock[0]);

	spin_lock(&l_mg->gc_lock);
	list_for_each_entry(line, &l_mg->gc_full_list, list) {
		if (line->type == PBLK_LINETYPE_DATA)
			d_line_cnt++;
		else if (line->type == PBLK_LINETYPE_LOG)
			l_line_cnt++;
		closed_line_cnt++;
		gc_full++;
	}

	list_for_each_entry(line, &l_mg->gc_high_list, list) {
		if (line->type == PBLK_LINETYPE_DATA)
			d_line_cnt++;
		else if (line->type == PBLK_LINETYPE_LOG)
			l_line_cnt++;
		closed_line_cnt++;
		gc_high++;
	}

	list_for_each_entry(line, &l_mg->gc_mid_list, list) {
		if (line->type == PBLK_LINETYPE_DATA)
			d_line_cnt++;
		else if (line->type == PBLK_LINETYPE_LOG)
			l_line_cnt++;
		closed_line_cnt++;
		gc_mid++;
	}

	list_for_each_entry(line, &l_mg->gc_low_list, list) {
		if (line->type == PBLK_LINETYPE_DATA)
			d_line_cnt++;
		else if (line->type == PBLK_LINETYPE_LOG)
			l_line_cnt++;
		closed_line_cnt++;
		gc_low++;
	}

	list_for_each_entry(line, &l_mg->gc_empty_list, list) {
		if (line->type == PBLK_LINETYPE_DATA)
			d_line_cnt++;
		else if (line->type == PBLK_LINETYPE_LOG)
			l_line_cnt++;
		closed_line_cnt++;
		gc_empty++;
	}

	list_for_each_entry(line, &l_mg->bad_list, list)
		bad++;
	list_for_each_entry(line, &l_mg->corrupt_list, list)
		cor++;
	spin_unlock(&l_mg->gc_lock);

	spin_lock(&l_mg->free_lock);
	if (l_mg->data_line[0]) {
		cur_sec = l_mg->data_line[0]->cur_sec;
		msecs = l_mg->data_line[0]->left_msecs;
		vsc = le32_to_cpu(*l_mg->data_line[0]->vsc);
		sec_in_line = l_mg->data_line[0]->sec_in_line;
		meta_weight = bitmap_weight(&l_mg->meta_bitmap,
							PBLK_DATA_LINES);
		map_weight = bitmap_weight(l_mg->data_line[0]->map_bitmap,
							lm->sec_per_line);
	}
	spin_unlock(&l_mg->free_lock);

	if (nr_free_lines != free_line_cnt)
		pr_err("pblk: corrupted free line list:%d/%d\n",
						nr_free_lines, free_line_cnt);

	sz = snprintf(page, PAGE_SIZE - sz,
		"line: nluns:%d, nblks:%d, nsecs:%d\n",
		geo->nr_luns, lm->blk_per_line, lm->sec_per_line);

	sz += snprintf(page + sz, PAGE_SIZE - sz,
		"lines:d:%d,l:%d-f:%d,m:%d/%d,c:%d,b:%d,co:%d(d:%d,l:%d)t:%d\n",
					cur_data, cur_log,
					nr_free_lines,
					emeta_line_cnt, meta_weight,
					closed_line_cnt,
					bad, cor,
					d_line_cnt, l_line_cnt,
					l_mg->nr_lines);

	sz += snprintf(page + sz, PAGE_SIZE - sz,
		"GC: full:%d, high:%d, mid:%d, low:%d, empty:%d, queue:%d\n",
			gc_full, gc_high, gc_mid, gc_low, gc_empty,
			atomic_read(&pblk->gc.inflight_gc));

	sz += snprintf(page + sz, PAGE_SIZE - sz,
		"data (%d) cur:%d, left:%d, vsc:%d, s:%d, map:%d/%d (%d)\n",
			cur_data, cur_sec, msecs, vsc, sec_in_line,
			map_weight, lm->sec_per_line,
			atomic_read(&pblk->inflight_io));

	return sz;
}

static ssize_t pblk_sysfs_lines_info(struct pblk *pblk, char *page)
{
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;
	struct pblk_line_meta *lm = &pblk->lm;
	ssize_t sz = 0;

	sz = snprintf(page, PAGE_SIZE - sz,
				"smeta - len:%d, secs:%d\n",
					lm->smeta_len, lm->smeta_sec);
	sz += snprintf(page + sz, PAGE_SIZE - sz,
				"emeta - len:%d, sec:%d, bb_start:%d\n",
					lm->emeta_len[0], lm->emeta_sec[0],
					lm->emeta_bb);
	sz += snprintf(page + sz, PAGE_SIZE - sz,
				"bitmap lengths: sec:%d, blk:%d, lun:%d\n",
					lm->sec_bitmap_len,
					lm->blk_bitmap_len,
					lm->lun_bitmap_len);
	sz += snprintf(page + sz, PAGE_SIZE - sz,
				"blk_line:%d, sec_line:%d, sec_blk:%d\n",
					lm->blk_per_line,
					lm->sec_per_line,
					geo->sec_per_blk);

	return sz;
}

static ssize_t pblk_sysfs_get_sec_per_write(struct pblk *pblk, char *page)
{
	return snprintf(page, PAGE_SIZE, "%d\n", pblk->sec_per_write);
}

#ifdef CONFIG_NVM_DEBUG
static ssize_t pblk_sysfs_stats_debug(struct pblk *pblk, char *page)
{
#ifdef CONFIG_PBLK_MULTIMAP
#define NR_STREAM	14
	ssize_t sz = 0;
	int i;
	int stream_count[NR_STREAM];
	int map_count[4]; 
	int log_count = 0;
	struct pba_addr *map;
	struct pblk_lstream *lstream;
	int nr_seg = (pblk->rl.nr_secs >> pblk->ppaf.blk_offset) + 1;
	long gc_write = 0;

	for (i = 0; i < NR_STREAM; i++) {
		stream_count[i] = 0;
	}
	for (i = 0; i < 4; i++) {
		map_count[i] = 0;
	}

	//spin_lock(&pblk->trans_lock);
	map = (struct pba_addr *)pblk->trans_map;
	for (i = 0; i < nr_seg; i++)
	{
		int map_type = map[i].m.map;
		int stream_num = map[i].m.lstream;
		if (map_type == NONE_MAP)
			stream_num = NR_STREAM - 1;
		map_count[map_type]++;
		stream_count[stream_num]++;
	}
	for (i = 0; i < pblk->nr_lstream; i++)
	{
		lstream = &pblk->lstream[i];
		log_count += lstream->map_size;
	}
	//spin_unlock(&pblk->trans_lock);


	sz += snprintf(page + sz, PAGE_SIZE - sz,
				"STREAM\t");
	for (i = 0; i < NR_STREAM; i++)
	{
		sz += snprintf(page + sz, PAGE_SIZE - sz,
				"%d\t", stream_count[i]);
	}

	sz += snprintf(page + sz, PAGE_SIZE - sz,
				"\nMAP\t");
	for (i = 0; i < 4; i++)
	{
		sz += snprintf(page + sz, PAGE_SIZE - sz,
					"%d\t", map_count[i]);
	}
	sz += snprintf(page + sz, PAGE_SIZE - sz,
					"\nLOG\t%d\n", log_count);

	sz += snprintf(page + sz, PAGE_SIZE - sz,
					"\n[WAF] req sub gc erase gcskip preinvalid\n");

	for (i = 0; i < pblk->nr_rwb; i++)
	{
		sz += snprintf(page + sz, PAGE_SIZE - sz,
				"%d\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\n", i,
			atomic_long_read(&pblk->req_writes[i]),
			atomic_long_read(&pblk->sub_writes[i]),
			atomic_long_read(&pblk->recov_gc_writes[i]),
			atomic_long_read(&pblk->nr_erase[i]),
			atomic_long_read(&pblk->nr_gcskip[i]),
			atomic_long_read(&pblk->preinvalid_gc[i])
			);
		gc_write += atomic_long_read(&pblk->recov_gc_writes[i]);
	}

	sz += snprintf(page + sz, PAGE_SIZE - sz,
				"\n\n\n\n\n[MAP] define BMT SMT PMT plog blog\n");

	sz += snprintf(page + sz, PAGE_SIZE - sz,
		"%lu\t%lu\t%lu\t%lu\t%lu\t%lu\n",
		atomic_long_read(&pblk->nr_definemap), 
		atomic_long_read(&pblk->nr_blockmap), 
		atomic_long_read(&pblk->nr_sectormap), 
		atomic_long_read(&pblk->nr_pagemap), 
		atomic_long_read(&pblk->nr_pagelog),
		atomic_long_read(&pblk->nr_blocklog));

	sz += snprintf(page + sz, PAGE_SIZE - sz,
				"\n[MAP] mapwrite mapread\n");

	sz += snprintf(page + sz, PAGE_SIZE - sz,
		"%lu\t%lu\n",
		atomic_long_read(&pblk->nr_mapwrite), 
		atomic_long_read(&pblk->nr_mapread));

	sz += snprintf(page + sz, PAGE_SIZE - sz,
				"\n[ETC] if_writes if_reads nrflush paddedwrite paddedwb sync_write recov_write recov_gc_reads cache_reads sync_reads\n");

	sz += snprintf(page + sz, PAGE_SIZE - sz,
		"\n%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%llu\n",
			atomic_long_read(&pblk->inflight_writes),
			atomic_long_read(&pblk->inflight_reads),
			atomic_long_read(&pblk->nr_flush),
			atomic_long_read(&pblk->padded_writes),
			atomic_long_read(&pblk->padded_wb),
			atomic_long_read(&pblk->sync_writes),
			gc_write,
			atomic_long_read(&pblk->recov_gc_reads),
			atomic_long_read(&pblk->cache_reads),
			atomic_long_read(&pblk->sync_reads),
			pblk->gc_time
		);

	return sz;

#else
	return snprintf(page, PAGE_SIZE,
		"%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\n",
			atomic_long_read(&pblk->inflight_writes),
			atomic_long_read(&pblk->inflight_reads),
			atomic_long_read(&pblk->req_writes),
			atomic_long_read(&pblk->nr_flush),
			atomic_long_read(&pblk->padded_writes),
			atomic_long_read(&pblk->padded_wb),
			atomic_long_read(&pblk->sub_writes),
			atomic_long_read(&pblk->sync_writes),
			atomic_long_read(&pblk->recov_writes),
			atomic_long_read(&pblk->recov_gc_writes),
			atomic_long_read(&pblk->recov_gc_reads),
			atomic_long_read(&pblk->cache_reads),
			atomic_long_read(&pblk->sync_reads));
#endif
}
#endif

static ssize_t pblk_sysfs_gc_force(struct pblk *pblk, const char *page,
				   size_t len)
{
	size_t c_len;
	int force;

	c_len = strcspn(page, "\n");
	if (c_len >= len)
		return -EINVAL;

	if (kstrtouint(page, 0, &force))
		return -EINVAL;

	pblk_gc_sysfs_force(pblk, force);

	return len;
}

static ssize_t pblk_sysfs_set_sec_per_write(struct pblk *pblk,
					     const char *page, size_t len)
{
	size_t c_len;
	int sec_per_write;

	c_len = strcspn(page, "\n");
	if (c_len >= len)
		return -EINVAL;

	if (kstrtouint(page, 0, &sec_per_write))
		return -EINVAL;

	if (sec_per_write < pblk->min_write_pgs
				|| sec_per_write > pblk->max_write_pgs
				|| sec_per_write % pblk->min_write_pgs != 0)
		return -EINVAL;

	pblk_set_sec_per_write(pblk, sec_per_write);

	return len;
}

static struct attribute sys_write_luns = {
	.name = "write_luns",
	.mode = 0444,
};

static struct attribute sys_rate_limiter_attr = {
	.name = "rate_limiter",
	.mode = 0444,
};

static struct attribute sys_gc_state = {
	.name = "gc_state",
	.mode = 0444,
};

static struct attribute sys_errors_attr = {
	.name = "errors",
	.mode = 0444,
};

static struct attribute sys_rb_attr = {
	.name = "write_buffer",
	.mode = 0444,
};

static struct attribute sys_stats_ppaf_attr = {
	.name = "ppa_format",
	.mode = 0444,
};

static struct attribute sys_lines_attr = {
	.name = "lines",
	.mode = 0444,
};

static struct attribute sys_lines_info_attr = {
	.name = "lines_info",
	.mode = 0444,
};

static struct attribute sys_gc_force = {
	.name = "gc_force",
	.mode = 0200,
};

static struct attribute sys_max_sec_per_write = {
	.name = "max_sec_per_write",
	.mode = 0644,
};

#ifdef CONFIG_NVM_DEBUG
static struct attribute sys_stats_debug_attr = {
	.name = "stats",
	.mode = 0444,
};
#endif

static struct attribute *pblk_attrs[] = {
	&sys_write_luns,
	&sys_rate_limiter_attr,
	&sys_errors_attr,
	&sys_gc_state,
	&sys_gc_force,
	&sys_max_sec_per_write,
	&sys_rb_attr,
	&sys_stats_ppaf_attr,
	&sys_lines_attr,
	&sys_lines_info_attr,
#ifdef CONFIG_NVM_DEBUG
	&sys_stats_debug_attr,
#endif
	NULL,
};

static ssize_t pblk_sysfs_show(struct kobject *kobj, struct attribute *attr,
			       char *buf)
{
	struct pblk *pblk = container_of(kobj, struct pblk, kobj);

	if (strcmp(attr->name, "rate_limiter") == 0)
		return pblk_sysfs_rate_limiter(pblk, buf);
	else if (strcmp(attr->name, "write_luns") == 0)
		return pblk_sysfs_luns_show(pblk, buf);
	else if (strcmp(attr->name, "gc_state") == 0)
		return pblk_sysfs_gc_state_show(pblk, buf);
	else if (strcmp(attr->name, "errors") == 0)
		return pblk_sysfs_stats(pblk, buf);
	else if (strcmp(attr->name, "write_buffer") == 0)
		return pblk_sysfs_write_buffer(pblk, buf);
	else if (strcmp(attr->name, "ppa_format") == 0)
		return pblk_sysfs_ppaf(pblk, buf);
	else if (strcmp(attr->name, "lines") == 0)
		return pblk_sysfs_lines(pblk, buf);
	else if (strcmp(attr->name, "lines_info") == 0)
		return pblk_sysfs_lines_info(pblk, buf);
	else if (strcmp(attr->name, "max_sec_per_write") == 0)
		return pblk_sysfs_get_sec_per_write(pblk, buf);
#ifdef CONFIG_NVM_DEBUG
	else if (strcmp(attr->name, "stats") == 0)
		return pblk_sysfs_stats_debug(pblk, buf);
#endif
	return 0;
}

static ssize_t pblk_sysfs_store(struct kobject *kobj, struct attribute *attr,
				const char *buf, size_t len)
{
	struct pblk *pblk = container_of(kobj, struct pblk, kobj);

	if (strcmp(attr->name, "gc_force") == 0)
		return pblk_sysfs_gc_force(pblk, buf, len);
	else if (strcmp(attr->name, "max_sec_per_write") == 0)
		return pblk_sysfs_set_sec_per_write(pblk, buf, len);

	return 0;
}

static const struct sysfs_ops pblk_sysfs_ops = {
	.show = pblk_sysfs_show,
	.store = pblk_sysfs_store,
};

static struct kobj_type pblk_ktype = {
	.sysfs_ops	= &pblk_sysfs_ops,
	.default_attrs	= pblk_attrs,
};

int pblk_sysfs_init(struct gendisk *tdisk)
{
	struct pblk *pblk = tdisk->private_data;
	struct device *parent_dev = disk_to_dev(pblk->disk);
	int ret;

	ret = kobject_init_and_add(&pblk->kobj, &pblk_ktype,
					kobject_get(&parent_dev->kobj),
					"%s", "pblk");
	if (ret) {
		pr_err("pblk: could not register %s/pblk\n",
						tdisk->disk_name);
		return ret;
	}

	kobject_uevent(&pblk->kobj, KOBJ_ADD);
	return 0;
}

void pblk_sysfs_exit(struct gendisk *tdisk)
{
	struct pblk *pblk = tdisk->private_data;

	kobject_uevent(&pblk->kobj, KOBJ_REMOVE);
	kobject_del(&pblk->kobj);
	kobject_put(&pblk->kobj);
}

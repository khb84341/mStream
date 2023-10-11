#include "pblk.h"

#ifdef CONFIG_PBLK_DFTL
static struct map_entry* pblk_search_cmt(struct pblk *pblk, int lseg)
{
    struct rb_node *node = pblk->cmt_mgt.root.rb_node;
    struct map_entry *me;

    while (node) {
        me = rb_entry(node, struct map_entry, rb_node);

        if (lseg < me->lseg)
            node = node->rb_left;
        else if (lseg > me->lseg)
            node = node->rb_right;
        else {
            return me;
        }
    }
    return NULL;
}

static int pblk_insert_cmt(struct pblk *pblk, int lseg, int map_size, int nr_log, int dirty) {
    struct rb_root *root = &pblk->cmt_mgt.root;
    struct rb_node **p = &(root->rb_node);
    struct rb_node *parent = NULL;
    struct map_entry *me;

    while (*p) {
        parent = *p;
        me = rb_entry(parent, struct map_entry, rb_node);

        if (lseg < me->lseg)
            p = &(*p)->rb_left;
        else if (lseg > me->lseg)
            p = &(*p)->rb_right;
        else
			return 0;
    }

    me = mempool_alloc(pblk->dftl_pool, GFP_ATOMIC);
    me->lseg = lseg;
    me->dirty = dirty;
    me->nr_log = nr_log;
    me->map_size = map_size;
    INIT_LIST_HEAD(&me->list);

    rb_link_node(&me->rb_node, parent, p);
    rb_insert_color(&me->rb_node, root);

    list_add_tail(&me->list, &pblk->cmt_mgt.map_list);
	atomic_long_inc(&pblk->nr_mapread);

	pblk->cmt_mgt.alloc_res += (map_size + nr_log);

//    me = pblk_search_cmt(pblk, lseg);
//    if (me == NULL)
  //      BUG_ON(1);

    return 0;
}

int pblk_delete_cmt(struct pblk *pblk, int lseg)
{
    struct map_entry *me;
    struct rb_root *root = &pblk->cmt_mgt.root;

    me = pblk_search_cmt(pblk, lseg);
    if (me != NULL) {
        list_del_init(&me->list);
        rb_erase(&me->rb_node, root);

        pblk->cmt_mgt.alloc_res = pblk->cmt_mgt.alloc_res - me->map_size - me->nr_log;
        mempool_free(me, pblk->dftl_pool);
    }
    return 0;
}

static int pblk_evict_cmt(struct pblk *pblk, int need_res)
{
    int free_res = pblk->cmt_mgt.max_res - pblk->cmt_mgt.alloc_res;
    struct rb_root *root = &pblk->cmt_mgt.root;
    unsigned int get_res = 0;

//    printk("pblk_evict_cmt need_res:%d free:%d\n", need_res, free_res);
    while (need_res > free_res)
    {
    	struct map_entry *me;

        if (list_empty(&pblk->cmt_mgt.map_list))
            BUG_ON(1);
        me = list_first_entry(&pblk->cmt_mgt.map_list, struct map_entry, list);
        list_del_init(&me->list);
        free_res = free_res + me->map_size + me->nr_log;
        get_res = get_res + (me->map_size + me->nr_log);

        if (me->dirty) {
            atomic_long_inc(&pblk->nr_mapwrite);
            // TODO: SEARCH PAGE_MAP and dirty to clean
        }
        rb_erase(&me->rb_node, root);
        mempool_free(me, pblk->dftl_pool);
    }
	pblk->cmt_mgt.alloc_res = pblk->cmt_mgt.alloc_res - get_res;
//    printk("pblk_evict_cmt alloc_res:%u get_res:%u end\n", pblk->cmt_mgt.alloc_res, get_res);

    return 0;
}

int pblk_load_cmt(struct pblk *pblk, int lseg, int dirty)
{
	struct map_entry *me = NULL;
	int type = PAGE_MAP;
	struct pba_addr *bmap = (struct pba_addr *)pblk->trans_map;
	struct pba_addr pba = bmap[lseg];
	int lstream = (u64)pba.m.lstream;
 	int map_size = pblk->lstream[lstream].map_size;
	type = pblk->lstream[lstream].map_type;

	if (type == BLOCK_MAP) {
		me = &pblk->cmt_mgt.global_map;
		if (bmap[lseg].m.is_cached == 0) {
			return 0;
		}
	} 
	me = pblk_search_cmt(pblk, lseg);

	if (me != NULL) {
		if (dirty)
			me->dirty = 1;
		list_move_tail(&me->list, &pblk->cmt_mgt.map_list);
	}
    else {
		int log_size = 0;
        int map_size = 4096;
		int free_res;

        if (type == PAGE_MAP) {
            struct pba_addr *pmap = (struct pba_addr*) pba.pointer;
            int pmap_entry_num = (1 << pblk->ppaf.blk_offset) >> pblk->ppaf.pln_offset;
            int i;
            for (i = 0; i < pmap_entry_num; i++) {
                if (pmap[i].m.is_cached == 1)
                    log_size += pblk->lstream[lstream].log_size;
            }
        }
        else if (pba.m.map == BLOCK_MAP) {
            map_size = pblk->lstream[lstream].log_size;
        }

        free_res = pblk->cmt_mgt.max_res - pblk->cmt_mgt.alloc_res;

        if (free_res < map_size + log_size)
            pblk_evict_cmt(pblk, map_size + log_size);
        pblk_insert_cmt(pblk, lseg, map_size, log_size, dirty);
    }

	return 0;
}

int pblk_alloc_log(struct pblk *pblk,
								int lseg, int type, unsigned int log_size)
{
	unsigned int free_res;
	struct map_entry *me = NULL;

//	printk("pblk_alloc_log: %d T:%d log:%u\n",
//					lseg, type, log_size);

	if (type == BLOCK_MAP) {
		me = &pblk->cmt_mgt.global_map;
	}
	else if (type == PAGE_MAP) {
		me = pblk_search_cmt(pblk, lseg);
		if (me == NULL) {
			pblk_load_cmt(pblk, lseg, true);
			me = pblk_search_cmt(pblk, lseg);
			if (me == NULL)
				BUG_ON(1);
		}
	}
	else {
		printk("pblk_alloc_log: %d %d %u\n", lseg, type, log_size);
		BUG_ON(1);
	}

	free_res = pblk->cmt_mgt.max_res - pblk->cmt_mgt.alloc_res;
	if (free_res < log_size)
		pblk_evict_cmt(pblk, log_size);

	if (me == NULL) {
		BUG_ON(1);
	}

	if (type == BLOCK_MAP) {
		me = pblk_search_cmt(pblk, lseg);
		if (me == NULL) {
			pblk_insert_cmt(pblk, lseg, 0, log_size, true);
		}
	} else {
		me->nr_log += log_size;
		pblk->cmt_mgt.alloc_res += log_size;
		me->dirty = 1;
	}

	return 0;
}

int pblk_free_log(struct pblk *pblk, int lseg, 
								int type, unsigned int log_size)
{
	struct map_entry *me = NULL;

	// printk("pblk_free_log: %d T:%d log:%u\n",
//					lseg, type, log_size);

	if (type == BLOCK_MAP) {
		me = &pblk->cmt_mgt.global_map;
		pblk_delete_cmt(pblk, lseg);
		return 0;
	}
	else if (type == PAGE_MAP) {
		me = pblk_search_cmt(pblk, lseg);
		if (me == NULL) {
			pblk_load_cmt(pblk, lseg, true);
			me = pblk_search_cmt(pblk, lseg);
			if (me == NULL)
				BUG_ON(1);
		}
		me->nr_log -= log_size;
		pblk->cmt_mgt.alloc_res -= log_size;
	}
	else {
		printk("pblk_alloc_log: %d %d %u\n", lseg, type, log_size);
		BUG_ON(1);
	}

	// printk("pblk_free_log free_log alloc_res:%u log_size:%u\n", pblk->cmt_mgt.alloc_res, log_size);
	return 0;
}

int pblk_change_map(struct pblk *pblk, int lseg)
{
	pblk_delete_cmt(pblk, lseg);
	pblk_load_cmt(pblk, lseg, true); 
	return 0;
}

#endif

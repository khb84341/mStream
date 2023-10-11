#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/f2fs_fs.h>

#include "f2fs.h"
#include "node.h"
#include "segment.h"

#ifdef F2FS_FGROUP
static int cmp_name(char *name1, char *name2, int size)
{
	int i = 0;
	while (name1[i] || name2[i]) 
	{
		if (name1[i] != name2[i])
			return name1[i] - name2[i];
		i++;
		if (i == 50)
			break;
	}
	return 0;
}

struct fgroup_history *lookup_fgroup(struct f2fs_sb_info *sbi, char *keyword, int vtype) 
{
	struct fgroup_history *fe;
	int i; 
	
	for (i = 0; i < sbi->history_count; i++) {
		fe = &sbi->fgroup_history[i];

		if (cmp_name(fe->name, keyword, 50) == 0) {
			if (fe->vtype == vtype) {
				return fe;
			}
		}
	}

	return NULL;	
}

static int insert_fgroup(struct f2fs_sb_info *sbi, struct drb_entry *re) 
{
	struct fgroup_history *fe;
	int vtype = re->ino & (31);
    unsigned long long cur_seq = user_data_blocks(sbi);
	unsigned long long lifetime = cur_seq - re->create_time;

	fe = lookup_fgroup(sbi, re->name, vtype);
	if (fe == NULL) {
		if (sbi->history_count >= MAX_HISTORY) {
			printk("HISTORY FULL\n");
			return 0;
		}
		fe = &sbi->fgroup_history[sbi->history_count];
		sbi->history_count++;
		memset(fe->name, 0, 51);
		snprintf(fe->name, 50, "%s", re->name);
		fe->vtype = vtype;
	}

	re->count += 1;
	re->latest_count += 1;
	re->latest_lifetime += (lifetime);
	if ((re->latest_count >= 1) && (re->latest_count >= re->valid)) {
		unsigned long long lifetime_value = re->latest_lifetime / re->latest_count;
		if (re->count == re->latest_count)
			re->ema = lifetime_value;
		else
			re->ema = re->ema * (EMA_W_DIV - EMA_W_NUM) / EMA_W_DIV + lifetime_value * EMA_W_NUM / EMA_W_DIV;
		re->latest_count = 0;
		re->latest_lifetime = 0;
	}

	fe->count = re->count;
	fe->ema = re->ema;
	fe->latest_count = re->latest_count; 
	fe->latest_lifetime = re->latest_lifetime;
	return 0;
}

struct drb_entry *lookup_dir_rb_tree(struct rb_root *root, 
				unsigned int ino)
{
	struct rb_node *node = root->rb_node;
	struct drb_entry *re;

	while (node) {
		re = rb_entry(node, struct drb_entry, rb_node);
		
		if (ino < re->ino)
			node = node->rb_left;
		else if (ino > re->ino)
			node = node->rb_right;
		else
			return re;
	}

	return NULL;
}

struct drb_entry* insert_dir_rb_tree(struct f2fs_sb_info *sbi, struct rb_root *root, unsigned int ino, char *name)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct drb_entry *re;
	unsigned long long cur_seq = user_data_blocks(sbi);
	struct fgroup_history *fe;
	int vtype = 0;

	while (*p) {
		parent = *p;
		re = rb_entry(parent, struct drb_entry, rb_node);
	
		if (ino < re->ino)
			p = &(*p)->rb_left;
		else if (ino > re->ino)
			p = &(*p)->rb_right;
		else {
			BUG_ON(1);
		}
	}

	re = vmalloc(sizeof(struct drb_entry));
	re->ino = ino;
	re->pstream = CLUSTER_NUM - 1;
#ifdef LIFETIME_AVERAGE
	re->lifetime_hot = 0;
	re->lifetime_load = 0;
	re->count_hot = 0;
	re->count_load = 0;
#else
	re->count = 0;
	re->ema = 0;
	re->latest_count = 0;
	re->latest_lifetime = 0;
#endif
	re->valid = 0;
	re->create_time = cur_seq;
	memset(re->name, 0, 50);
	snprintf(re->name, 50, "%s", name);

	vtype = ino & 31;
	fe = lookup_fgroup(sbi, name, vtype);
	if (fe != NULL) {
		re->count = fe->count;
		re->ema = fe->ema;
		re->latest_count = fe->latest_count;
		re->latest_lifetime = fe->latest_lifetime;

		if (re->count > 0) {
			unsigned long long lifetime_value = 0;
			if (re->latest_count > 0) {
				lifetime_value = re->latest_lifetime / re->latest_count;
				if (re->latest_count < re->count)	
					lifetime_value = lifetime_value * EMA_W_NUM / EMA_W_DIV + re->ema * (EMA_W_DIV - EMA_W_NUM) / EMA_W_DIV;
			}
			else 
				lifetime_value = re->ema;

			re->pstream = f2fs_lifetime_to_pstream(sbi, lifetime_value);
		}
	}
	//printk("insert tree ino:%u\n", ino);

	rb_link_node(&re->rb_node, parent, p);
	rb_insert_color(&re->rb_node, root);

	return re;
}

int delete_dir_rb_tree(struct f2fs_sb_info *sbi, unsigned int ino)
{
	int vtype = 0;
	struct rb_root *root = &sbi->drb_tree;
	for (vtype = 0; vtype <= FGROUP_ETC; vtype++) {
		unsigned int key = (ino << 5) + vtype;
		struct drb_entry *re = lookup_dir_rb_tree(root, key);
		if (re != NULL) {
			insert_fgroup(sbi, re);
/*
			unsigned long long lifetime_value = re->ema;
			if (re->count == 0) {
			    unsigned long long cur_seq = user_data_blocks(sbi);
				lifetime_value = cur_seq - re->create_time;
			} else if (re->latest_count > 0) {
				if (re->count == re->latest_count)
					lifetime_value = re->latest_lifetime / re->latest_count;
				else
					lifetime_value = lifetime_value *(EMA_W_DIV - EMA_W_NUM) / EMA_W_DIV + re->latest_lifetime / re->latest_count * EMA_W_NUM / EMA_W_DIV;
			}


		    if (sbi->vstream_keyword[vtype].count == 0) 
				sbi->vstream_keyword[vtype].ema = lifetime_value;
	        else {
				sbi->vstream_keyword[vtype].ema = sbi->vstream_keyword[vtype].ema *(EMA_W_DIV - EMA_W_NUM) / EMA_W_DIV + lifetime_value * EMA_W_NUM / EMA_W_DIV;
			}
		    sbi->vstream_keyword[vtype].count += 1;
*/
			rb_erase(&re->rb_node, root);
			vfree(re);
		}
	}
	return 0;
}

int lookup_pstream_dir_rb_tree(struct f2fs_sb_info *sbi, unsigned int ino)
{
	struct drb_entry* entry;
//	unsigned long long cur_seq = user_data_blocks(sbi);
//	unsigned long long dir_lifetime;
//	unsigned long long lifetime;
//	unsigned int count;
	
	entry = lookup_dir_rb_tree(&sbi->drb_tree, ino);
	if (entry == NULL)
		return -1;
	
//	dir_lifetime = cur_seq - entry->create_time;
//	lifetime = entry->lifetime_hot + entry->lifetime_load;
//	count = entry->count_load + entry->count_hot;

//	if (dir_lifetime < (CLUSTER_T * 4))
//	if (dir_lifetime < (128 * 512 * 8 * 4))
//		return -1;
/*
	if (entry->pstream == NR_CLUSTER - 1) {
		if (count > 0) {
			lifetime = lifetime / count;
			return f2fs_lifetime_to_pstream(sbi, lifetime);
		}
		else if (dir_lifetime < (CLUSTER_T * 4))
			return 0;
//		return f2fs_lifetime_to_pstream(sbi, dir_lifetime);
	}
*/
	return entry->pstream;
}

int update_pstream_dir_rb_tree(struct f2fs_sb_info *sbi, unsigned int ino, unsigned int pstream)
{
	struct drb_entry* entry;

	entry = lookup_dir_rb_tree(&sbi->drb_tree, ino);

	if (entry == NULL) {
	//	entry = insert_dir_rb_tree(sbi, &sbi->drb_tree, ino);
		return 0;
	}
	entry->pstream = pstream;

	return 0;
}

int update_dir_rb_tree(struct f2fs_sb_info *sbi, unsigned int dir_ino,  unsigned long long lifetime, unsigned int count, char* name)
{
	struct drb_entry* entry;
#ifdef LIFETIME_AVERAGE
	unsigned long long lifetime_seg; 
#endif
	entry = lookup_dir_rb_tree(&sbi->drb_tree, dir_ino);
	
	if (entry == NULL)
		entry = insert_dir_rb_tree(sbi, &sbi->drb_tree, dir_ino, name);

#ifdef LIFETIME_AVERAGE
	lifetime_seg = lifetime / 512;
	if (lifetime_seg >= 8)  {
		entry->count_load += count;
		entry->lifetime_load += lifetime;
	} else {
		entry->count_hot += count;
		entry->lifetime_hot += lifetime;
	}
#else
	entry->count += count;
	entry->latest_count += count;
	entry->latest_lifetime += (lifetime * count);
	if ((entry->latest_count >= 1) && (entry->latest_count > entry->valid)) {
		unsigned long long lifetime_value;
		lifetime_value = entry->latest_lifetime / entry->latest_count;
		if (entry->count == entry->latest_count)
			entry->ema = lifetime_value;
		else
			entry->ema = entry->ema * (EMA_W_DIV - EMA_W_NUM) / EMA_W_DIV + lifetime_value * EMA_W_NUM / EMA_W_DIV;
		entry->latest_count = 0;
		entry->latest_lifetime = 0;
	}
#endif
	return 0;
}

int update_valid_rb_tree(struct f2fs_sb_info *sbi, unsigned int dir_ino, int valid, char* name)
{
	struct drb_entry* entry;
	if (dir_ino == 0)
		return 0;

	entry = lookup_dir_rb_tree(&sbi->drb_tree, dir_ino);
	if (entry == NULL) {
		entry = insert_dir_rb_tree(sbi,&sbi->drb_tree, dir_ino, name);
	}

	if (valid < 0) {
		unsigned int valid_plus = 0 - valid;
		if (entry->valid < valid_plus) {
//			printk("valid=0 dir_ino:%u\n", dir_ino);
			entry->valid = 0;
		}
		else
			entry->valid += valid;
	}
	else 
		entry->valid += valid;

	if (entry->valid > entry->max_valid)
		entry->max_valid = entry->valid;

	return 0;
}

#endif

#include "f2fs.h"

#ifdef F2FS_FGROUP
static struct file_entry *lookup_file_entry(struct rb_root *root, 
				unsigned int ino)
{
	struct rb_node *node = root->rb_node;
	struct file_entry *re;

	while (node) {
		re = rb_entry(node, struct file_entry, rb_node);
		
		if (ino < re->ino)
			node = node->rb_left;
		else if (ino > re->ino)
			node = node->rb_right;
		else
			return re;
	}
	return NULL;
}

static struct file_entry* insert_file_entry(struct f2fs_sb_info *sbi, struct rb_root *root, int ino)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct file_entry *re;
	int i;

	while (*p) {
		parent = *p;
		re = rb_entry(parent, struct file_entry, rb_node);
	
		if (ino < re->ino)
			p = &(*p)->rb_left;
		else if (ino > re->ino)
			p = &(*p)->rb_right;
		else {
			BUG_ON(1);
		}
	}

	re = vmalloc(sizeof(struct file_entry));
	re->ino = ino;
	re->last_0_update = 0;
	re->last_offset = 0;
	re->lifetime = 0;
	re->filetype = 0;
	re->valid = 0;
	re->cold = 1;

	re->sample_avglifetime = 0;
	re->sample_rate = 0;
	for (i = 0; i < MAX_SAMPLE; i++) {
		re->sample_lifetime[i] = 0;
		re->sample_invalid[i] = 0;
		re->sample_lastupdate[i] = 0;
		re->sample_index[i] = -1;
	}
	re->max_range = 0;
	re->last_kmeans = sbi->kmeans_count;

	rb_link_node(&re->rb_node, parent, p);
	rb_insert_color(&re->rb_node, root);

	return re;
}

static int update_file_type(struct f2fs_sb_info *sbi, unsigned int ino, int filetype)
{
	struct file_entry *re;
	struct rb_root *root = &sbi->lifetime_tree;

	spin_lock(&sbi->ltree_lock);
	re = lookup_file_entry(root, ino);
	if (re != NULL) {
		re->filetype = filetype;
	}
	spin_unlock(&sbi->ltree_lock);

	return 0;
}

static int get_file_type(struct f2fs_sb_info *sbi, unsigned int ino)
{
	struct file_entry *re;
	struct rb_root *root = &sbi->lifetime_tree;

	int type = 0;
	spin_lock(&sbi->ltree_lock);
	re = lookup_file_entry(root, ino);
	if (re != NULL) {
		type = re->filetype;
	}
	spin_unlock(&sbi->ltree_lock);

	return type;
}

int delete_file_entry(struct f2fs_sb_info *sbi, unsigned int ino)
{
	struct file_entry *re;
	struct rb_root *root = &sbi->lifetime_tree;

	spin_lock(&sbi->ltree_lock);
	re = lookup_file_entry(root, ino);
	if (re != NULL) {
		rb_erase(&re->rb_node, root);
		vfree(re);
	}
	spin_unlock(&sbi->ltree_lock);

	return 0;
}




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

static struct dentry* get_dentry(struct inode *inode) {
    struct dentry *dentry_iter, *de = NULL;

    spin_lock(&inode->i_lock);

    hlist_for_each_entry(dentry_iter, &inode->i_dentry, d_u.d_alias) {
        de = dentry_iter;
        break;
    }
    spin_unlock(&inode->i_lock);

	return de;
}

#define MAX_PARENT_DENTRY   20
static struct dentry* get_parent_dentry(struct dentry *de, char *keyword)
{
    int count;
    if (de == NULL)
        return NULL;

    for (count = 1; count < MAX_PARENT_DENTRY; count++)
    {
        struct dentry *p_de = de->d_parent;
        if (p_de) {
            if (p_de->d_name.name == NULL)
                return 0;
            if ((strstr(p_de->d_name.name, keyword) != NULL) &&
                            (p_de->d_name.name[0] == keyword[0]) &&
                    (strlen(p_de->d_name.name) < strlen(keyword) + 3)) {
				return de;
            }
            if (!strcmp("/", de->d_name.name))
                break;
        } else {
            break;
        }
        de = p_de;
    }
	return NULL;
}

static unsigned int get_parent_inode(struct inode *inode, char *keyword, int depth)
{
    int count;
    struct dentry *dentry_iter, *de = NULL;
    struct inode *i_list[MAX_PARENT_DENTRY];
    struct dentry *de_list[MAX_PARENT_DENTRY];
	struct f2fs_inode_info *ei = F2FS_I(inode);

    spin_lock(&inode->i_lock);

    hlist_for_each_entry(dentry_iter, &inode->i_dentry, d_u.d_alias) {
        de = dentry_iter;
        break;
    }
    spin_unlock(&inode->i_lock);

    i_list[0] = inode;
    de_list[0] = de;

    if (de == NULL)
        return 0;

    for (count = 1; count < MAX_PARENT_DENTRY; count++)
    {
        struct dentry *p_de = de->d_parent;
        if (p_de) {
            de_list[count] = p_de;
            if (p_de->d_name.name == NULL)
                return 0;
            i_list[count] = p_de->d_inode;
            if ((strstr(p_de->d_name.name, keyword) != NULL) &&
                            (p_de->d_name.name[0] == keyword[0]) &&
                    (strlen(p_de->d_name.name) < strlen(keyword) + 3)) {
                if (count > depth) {
                    if (depth == 1)
						snprintf(ei->i_keyword, 50, "KEY-%d:%s/%s", ei->i_filetype, p_de->d_parent->d_name.name, de_list[count-depth]->d_name.name);
					if (depth == 2)
                        snprintf(ei->i_keyword, 50, "KEY-%d:%s/%s", ei->i_filetype, de_list[count-1]->d_name.name, de_list[count-depth]->d_name.name);
					if (depth == 3)
                        snprintf(ei->i_keyword, 50, "KEY-%d:%s/%s", ei->i_filetype, de_list[count-1]->d_name.name, de_list[count-depth]->d_name.name);
                    return i_list[count-depth]->i_ino;
                } else if (count > depth - 1) {
                    if (depth == 1)
                        snprintf(ei->i_keyword, 50, "KEY-%d/%s/%s", ei->i_filetype, p_de->d_parent->d_name.name, de_list[count-depth+1]->d_name.name);
                    if (depth == 2)
                        snprintf(ei->i_keyword, 50, "KEY-%d/%s/%s", ei->i_filetype, de_list[count-1]->d_name.name, de_list[count-depth+1]->d_name.name);
                    if (depth == 3) {
                        snprintf(ei->i_keyword, 50, "KEY-%d/%s/%s", ei->i_filetype, de_list[count-1]->d_name.name, de_list[count-depth+1]->d_name.name);
					}
                    return i_list[count-depth+1]->i_ino;
                }
                printk("except case: K[%s] C(%d) %s\n", keyword, count, de_list[count]->d_name.name);
                break;
            }
            if (!strcmp("/", de->d_name.name))
                break;
        } else {
            break;
        }
        de = p_de;
    }
    return 0;
}

static int get_full_path_dentry(struct dentry *dentry, char *full_name, int name_size)
{
    int count, i;
#define MAX_PARENT_DENTRY   20
    struct dentry *de_list[MAX_PARENT_DENTRY];
    struct dentry *de = dentry;

    de_list[0] = de;

    memset(full_name, 0, name_size);

    for (count = 1; count < MAX_PARENT_DENTRY; count++)
    {
        struct dentry *p_de = de->d_parent;
        if (p_de) {
            de_list[count] = p_de;
            if (p_de->d_name.name == NULL)
                return -1;
            if (!strcmp("/", de->d_name.name)) {
                break;
            }
        } else {
            break;
        }
        de = p_de;
    }
    sprintf(full_name, "/");
    for (i = count-2; i>=0; i--) {
        strncat(full_name, de_list[i]->d_name.name, name_size);
        if (i != 0)
            strncat(full_name, "/", name_size);
    }
    return 0;
}

int check_hot_stream(unsigned long long fgroup)
{
	int vtype = fgroup & (31);
	
	switch (vtype)
	{
		case FGROUP_EXT_JOURNAL:
			return 1;
			break; 
		default:
			break;
	}

	return 0; 
}

unsigned long long int get_fgroup(struct f2fs_sb_info *sbi, struct inode *inode, struct dentry *dentry)
{
	char name[500]; 
	int ret = 0;
	unsigned long long fgroup = 0;
	int file_type = -1;
	long long app_type = -1;
	struct f2fs_inode_info *ei = F2FS_I(inode);

	if (dentry == NULL)
		ret = get_full_path(inode, name, 500);
	else
		ret = get_full_path_dentry(dentry, name, 500);
	
	if (ret < 0)
		return FGROUP_ETC; 

#ifdef F2FS_TRACE_ENABLE
	snprintf(ei->i_name, 200, "%s", name);
#endif

    if ((strstr(name, "data/media/0/") != NULL) && (strstr(name, "/Android/") == NULL)) {
        if (strstr(name, "/0/DCIM/") != NULL) {
            file_type = FGROUP_DCIM;
            app_type = 0;
        }
        else if (strstr(name, "/0/movie/") != NULL) {
            file_type = FGROUP_MOVIE;
            app_type = 0;
        }
        else if (strstr(name, "/0/music") != NULL) {
            file_type = FGROUP_MUSIC;
            app_type = 0;
        }
    }
    else if (strstr(name, "data/local") != NULL) {
        file_type = FGROUP_LOCAL;
        app_type = 0;
    }
    else if (strstr(name, "data/system/") != NULL) {
        file_type = FGROUP_SYSTEM;
        app_type = 0;
    }
    else if (strstr(name, "/data/app/") != NULL)
        file_type = FGROUP_EXEC;
    else if (strstr(name, "lib-main") != NULL)
        file_type = FGROUP_LIBMAIN;
#if (defined MSTREAM_EXT || defined MSTREAM_DBDIR)
    else if (strstr(name, "-journal") != NULL) {
        file_type = FGROUP_EXT_JOURNAL;
	}
#else
    else if (strstr(name, "-journal") != NULL) {
		struct dentry *temp_de = get_dentry(inode);
        file_type = FGROUP_EXT_JOURNAL;
        app_type = inode->i_ino;
		if (temp_de == NULL) 
			snprintf(ei->i_keyword, 50, "KEY-%d:%s", file_type, name);
		else
			snprintf(ei->i_keyword, 50, "KEY-%d:%s", file_type, temp_de->d_name.name);
	}
#endif
//    else if (strstr(name, ".png") != NULL)
//        file_type = FGROUP_EXT_PNG;
    else if (strstr(name, ".bak") != NULL)
        file_type = FGROUP_EXT_BAK;
#if (defined MSTREAM_EXT || defined MSTREAM_DBDIR)
    else if (strstr(name, "-wal") != NULL)
		file_type = FGROUP_EXT_WAL;
#else
    else if (strstr(name, "-wal") != NULL) {
		struct dentry *temp_de = get_dentry(inode);
        struct dentry *parent_de = get_parent_dentry(temp_de, "data");
        file_type = FGROUP_EXT_WAL;
        app_type = inode->i_ino;
		if (temp_de == NULL || parent_de == NULL)
			snprintf(ei->i_keyword, 50, "KEY-%d:%s", file_type, name);
		else
			snprintf(ei->i_keyword, 50, "KEY-%d:%s/%s", file_type, parent_de->d_name.name, temp_de->d_name.name);

	}
#endif
    else if (strstr(name, "db-") != NULL)
        file_type = FGROUP_EXT_DBETC;
    else if (strstr(name, "dbtmp") != NULL)
        file_type = FGROUP_EXT_DBETC;
    else if (strstr(name, "-shm") != NULL)
        file_type = FGROUP_EXT_DBETC;
    else if (strstr(name, "/app_webview/") != NULL)
        file_type = FGROUP_APPWEBVIEW;
    else if (strstr(name, "/code_cache/") != NULL)
        file_type = FGROUP_APPOTHERS;
    else if (strstr(name, "/shared_prefs/") != NULL)
        file_type = FGROUP_APPOTHERS;
#if (defined MSTREAM_EXT || defined MSTREAM_DBDIR)
    else if (strstr(name, "/databases/") != NULL)
        file_type = FGROUP_DATABASES;
    else if (strstr(name, ".db/") != NULL)
        file_type = FGROUP_DATABASES;
#else
    else if (strstr(name, "/databases/") != NULL) {
		struct dentry *temp_de = get_dentry(inode);
        struct dentry *parent_de = get_parent_dentry(temp_de, "data");
        file_type = FGROUP_DATABASES;
        app_type = inode->i_ino;
		if (temp_de == NULL || parent_de == NULL)
			snprintf(ei->i_keyword, 50, "KEY-%d:%s", file_type, name);
		else
			snprintf(ei->i_keyword, 50, "KEY-%d:%s/%s", file_type, parent_de->d_name.name, temp_de->d_name.name);
	}
    else if (strstr(name, ".db") != NULL) {
		struct dentry *temp_de = get_dentry(inode);
        struct dentry *parent_de = get_parent_dentry(temp_de, "data");
        file_type = FGROUP_DATABASES;
        app_type = inode->i_ino;
		if (temp_de == NULL || parent_de == NULL)
			snprintf(ei->i_keyword, 50, "KEY-%d:%s", file_type, name);
		else
			snprintf(ei->i_keyword, 50, "KEY-%d:%s/%s", file_type, parent_de->d_name.name, temp_de->d_name.name);
	}
#endif

#if (defined MSTREAM_EXT || defined MSTREAM_APP)
    else if (strstr(name, "/cache/") != NULL) 
		file_type = FGROUP_CACHE;
    else if (strstr(name, "/files/") != NULL)
		file_type = FGROUP_CACHE;
//    else if (strstr(name, ".tmp") != NULL)
//        file_type = FGROUP_EXT_TMP;
    else if (strstr(name, "data/data/") != NULL)
        file_type = FGROUP_CACHE;
#endif
    else if (strstr(name, "/cache/") != NULL) {
        int ret;
        if (strstr(name, "index") != NULL)
            file_type = FGROUP_CACHE_INDEX;
        else if (strstr(name, "journal") != NULL)
            file_type = FGROUP_CACHE_INDEX;
        else
            file_type = FGROUP_CACHE;
		ei->i_filetype = file_type;
        ret = get_parent_inode(inode, "cache", 1);
        if (ret != 0) {
            app_type = ret;
        }
    }
    else if (strstr(name, "/files/") != NULL) {
        int ret;
        if (strstr(name, "index") != NULL)
            file_type = FGROUP_CACHE_INDEX;
        else if (strstr(name, "journal") != NULL)
            file_type = FGROUP_CACHE_INDEX;
        else
            file_type = FGROUP_FILES;
		ei->i_filetype = file_type;
        ret = get_parent_inode(inode, "files", 1);
        if (ret != 0) {
            app_type = ret;
        }
    }
//    else if (strstr(name, ".tmp") != NULL)
//        file_type = FGROUP_EXT_TMP;
    else if (strstr(name, "data/data/") != NULL) {
        int ret;
        if (strstr(name, "index") != NULL)
            file_type = FGROUP_CACHE_INDEX;
        else if (strstr(name, "journal") != NULL)
            file_type = FGROUP_CACHE_INDEX;
        else
            file_type = FGROUP_APPSPECIAL;
		ei->i_filetype = file_type;
        ret = get_parent_inode(inode, "data", 3);
        if (ret != 0) {
            app_type = ret;
        } else {
	        ret = get_parent_inode(inode, "data", 2);
	        if (ret != 0) {
    	        app_type = ret;
        	}
		}
    }

    if (file_type == -1) {
        file_type = FGROUP_ETC;
    }

	ei->i_filetype = file_type;
#ifndef MSTREAM_EXT
    if (app_type == -1) {
        if (file_type == FGROUP_EXEC)
            app_type = get_parent_inode(inode, "app", 1);
        else
            app_type = get_parent_inode(inode, "data", 1);
    }
#endif
    if (app_type == 0) {
		snprintf(ei->i_keyword, 50, "KEY-%d:%s", file_type, "NoApp");
    }

    fgroup = (((unsigned long long)app_type) << 5) + file_type;
//	printk("fgroup: %llu app_type: %lld file_type:%d\n", fgroup, app_type, file_type);
		
	return fgroup;
}

///////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////

int f2fs_lifetime_to_cluster(struct f2fs_sb_info *sbi, unsigned long long lifetime)
{
	//unsigned long long min = 4294967295;
	int i;
	int cluster = NR_CLUSTER - 2;

	if (sbi->kmeans_max[0] == 0)
		return 0;	

	for (i = 0; i < NR_CLUSTER - 1; i++) {
		if (lifetime < sbi->kmeans_max[i] * 512) {
			cluster = i;
			break;
		}
	}

	return cluster;
}

static struct fgroup_history *lookup_fgroup_history(struct f2fs_sb_info *sbi, char *keyword, int vtype) 
{
	struct fgroup_history *fe;
	int i; 
	
	for (i = 0; i < sbi->history_count; i++) {
		fe = &sbi->fgroup_history[i];

		if (cmp_name(fe->keyword, keyword, 50) == 0) {
			if (fe->vtype == vtype) {
				return fe;
			}
		}
	}
	return NULL;	
}

unsigned long long update_new_ema(struct f2fs_sb_info *sbi, struct fgroup_entry *re)
{
	unsigned long long lifetime_value;
	int trace = 0;
	unsigned long long prev_ema; 
	int vtype = re->fgroup & (31);
	unsigned long long num_active, num_invalid, life_invalid;


//	if (strstr(re->keyword, "KEY-15:data/com.airbnb.android")) {
//		trace = 1;
//	}

#if (defined MSTREAM_EXT || defined MSTREAM_DBDIR)
#else
/*
	if (vtype == FGROUP_EXT_SPECIAL_JOURNAL) {
		re->cluster = 0;
		printk("[JOURNAL]\t%u\t%llu\t%llu\t%llu\t%llu\t%d\t%s\n", re->cluster, 0, re->cold, re->valid, re->count, vtype, re->keyword);
		return 0;
	}
*/
#endif

	if (vtype == FGROUP_LIBMAIN || vtype == FGROUP_EXEC 
			|| vtype == FGROUP_DCIM || vtype == FGROUP_MOVIE ||
			vtype == FGROUP_MUSIC)
	{
		unsigned long long fgroup_age = user_data_blocks(sbi) - re->create_time;
		re->cluster = NR_CLUSTER - 1;
		printk("[COLDRATE]\t%u\t%llu\t%llu\t%d\t%llu\t%d\t%s\n", re->cluster, fgroup_age/512, re->cold, re->valid, re->count, vtype, re->keyword);
		return 0;
	}

	if (re->update_ema == 0) {
		unsigned long long fgroup_age = user_data_blocks(sbi) - re->create_time;
		if (fgroup_age < PROFILE_T) {
#ifndef CLUSTER_TIME
//			printk("[PROFILE]\t%u\t%llu\t%llu\t%d\t%llu\t%d\t%s\n", re->cluster, fgroup_age/512, re->cold, re->valid, re->count, vtype, re->keyword);
#endif
			return 0;
		}
		if (re->cold > (re->cold + re->count) * COLD_RATE / 100) {
			int new_cluster = f2fs_lifetime_to_cluster(sbi, fgroup_age);
			if (new_cluster > re->cluster)
				re->cluster = new_cluster;
#ifndef CLUSTER_TIME
			printk("[COLDRATE]\t%u\t%llu\t%llu\t%d\t%llu\t%d\t%s\n", re->cluster, fgroup_age/512, re->cold, re->valid, re->count, vtype, re->keyword);
#endif
			return 0;
		} else if (re->latest_count == 0) {
			int new_cluster = f2fs_lifetime_to_cluster(sbi, fgroup_age);	
			if (new_cluster > re->cluster)
				re->cluster = new_cluster;
#ifndef CLUSTER_TIME
			printk("[COLDRATE]\t%u\t%llu\t%llu\t%d\t%llu\t%d\t%s\n", re->cluster, fgroup_age/512, re->cold, re->valid, re->count, vtype, re->keyword);
#endif
			return 0;
		}
/*
		if ((re->latest_count < 4096)){
			if (re->valid > re->latest_count)
				return 0;
			else if (re->latest_count == 0)
				return 0;
		}
*/

		num_active = re->valid;
		num_invalid = re->latest_count;
		life_invalid = re->latest_lifetime;

		lifetime_value = re->latest_lifetime / re->latest_count;

		re->update_ema = 1;
		re->ema = lifetime_value;
		if (trace == 1) {
			printk("lv: %llu %llu %llu\n", re->latest_lifetime/512, re->latest_count, lifetime_value/512);
			printk("EMA(first): %llu\n", lifetime_value/512);
		}
		re->latest_count = 0;
		re->latest_lifetime = 0;
		printk("[EXPECTLIFE:L0]\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%s\n"
	, num_active, num_invalid, 0, life_invalid, lifetime_value, re->ema, re->keyword);
		return re->ema;
	}
	else if ((re->latest_count + re->valid > 0)) {
		num_active = re->valid;
		num_invalid = re->latest_count;
		life_invalid = re->latest_lifetime;

		lifetime_value = num_active * re->ema + re->latest_lifetime;
		lifetime_value = lifetime_value / (num_active + num_invalid);
		if (trace == 1) {
			printk("lv: I/ %llu %llu /A %llu %llu\n", re->latest_lifetime/512, num_invalid, num_active, lifetime_value/512);
		}
		re->latest_count = 0;
		re->latest_lifetime = 0;
	} else {
		return re->ema;
	}

	if (strstr(re->keyword, "KEY-15")) {
		trace_printk("[KMEAN-lifetime]\t%llu\t%s\n", lifetime_value/512, re->keyword);
	}
	prev_ema = re->ema;
	re->ema = re->ema * (EMA_W_DIV - EMA_W_NUM) / EMA_W_DIV + lifetime_value * EMA_W_NUM / EMA_W_DIV;

	printk("[EXPECTLIFE]\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%s\n"
	, num_active, num_invalid, prev_ema, life_invalid, lifetime_value, re->ema, re->keyword);
	return re->ema;
}

static int insert_fgroup_history(struct f2fs_sb_info *sbi, struct fgroup_entry *re) 
{
	struct fgroup_history *fe;
	int vtype = re->fgroup & (31);
	fe = lookup_fgroup_history(sbi, re->keyword, vtype);
	if (fe == NULL) {
		if (sbi->history_count >= MAX_HISTORY) {
			printk("HISTORY FULL\n");
			return 0;
		}
		fe = &sbi->fgroup_history[sbi->history_count];
		sbi->history_count++;
		memset(fe->keyword, 0, 50);
		snprintf(fe->keyword, 50, "%s", re->keyword);
		fe->vtype = vtype;
	}

	if (re->count == 0) {
	    unsigned long long cur_seq = user_data_blocks(sbi);
		unsigned long long lifetime = cur_seq - re->create_time;
		re->count += 1;
		re->latest_count += 1;
		re->latest_lifetime += (lifetime);
	}
	fe->count = re->count;
	fe->ema = re->ema;
	fe->update_ema = re->update_ema;
	fe->latest_count = re->latest_count; 
	fe->latest_lifetime = re->latest_lifetime;
	return 0;
}

struct fgroup_entry *lookup_fgroup_entry(struct rb_root *root, 
				unsigned long long fgroup)
{
	struct rb_node *node = root->rb_node;
	struct fgroup_entry *re;

	while (node) {
		re = rb_entry(node, struct fgroup_entry, rb_node);
		
		if (fgroup < re->fgroup)
			node = node->rb_left;
		else if (fgroup > re->fgroup)
			node = node->rb_right;
		else
			return re;
	}
	return NULL;
}

static struct fgroup_entry* insert_fgroup_entry(struct f2fs_sb_info *sbi, struct rb_root *root, unsigned long long fgroup, char* name)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct fgroup_entry *re;
	unsigned long long cur_seq = user_data_blocks(sbi);
	struct fgroup_history *fe;
	int vtype = 0;
	int found = 0;

	vtype = fgroup & 31;
	if (vtype == FGROUP_EXT_JOURNAL) {
		fe = lookup_fgroup_history(sbi, name, vtype);
		if (fe != NULL) {
			re = lookup_fgroup_entry(root, FGROUP_EXT_SPECIAL_JOURNAL);
			if (re == NULL) 
				re = insert_fgroup_entry(sbi, root, FGROUP_EXT_SPECIAL_JOURNAL, "KEY-7:JOURNAL");
			return re;
		}
	}

	while (*p) {
		parent = *p;
		re = rb_entry(parent, struct fgroup_entry, rb_node);
	
		if (fgroup < re->fgroup)
			p = &(*p)->rb_left;
		else if (fgroup > re->fgroup)
			p = &(*p)->rb_right;
		else {
			BUG_ON(1);
		}
	}

	re = vmalloc(sizeof(struct fgroup_entry));
	re->fgroup = fgroup;
	re->cluster = NR_CLUSTER - 1;
	re->count = 0;
	re->ema = 0;
	re->latest_count = 0;
	re->latest_lifetime = 0;
	re->create_time = cur_seq;
	re->valid = 0;
	re->cold = 0;
	re->update_ema = 0;
	memset(re->keyword, 0, 50);
	snprintf(re->keyword, 50, "%s", name);

	if (vtype == FGROUP_LIBMAIN || vtype == FGROUP_EXEC 
			|| vtype == FGROUP_DCIM || vtype == FGROUP_MOVIE ||
			vtype == FGROUP_MUSIC)
		found = 0;
	else {
		fe = lookup_fgroup_history(sbi, name, vtype);
		if (fe != NULL) 
			found = 1;
	}

	if (found == 1) {
		re->count = fe->count;
		re->ema = fe->ema;
		re->latest_count = fe->latest_count;
		re->latest_lifetime = fe->latest_lifetime;
		re->update_ema = fe->update_ema;

		if (re->count > 0) {
			unsigned long long lifetime_value = 0;
			if (re->update_ema == 1) {
				lifetime_value = re->ema;
				re->cluster = f2fs_lifetime_to_cluster(sbi, lifetime_value);
			}
			else if (re->latest_count > 0) {
				lifetime_value = re->latest_lifetime / re->latest_count;
				re->cluster = f2fs_lifetime_to_cluster(sbi, lifetime_value);
			}
			else
				re->cluster = NR_CLUSTER - 2;
		}
	} else {
		switch (vtype)
		{
			case FGROUP_CACHE:
			case FGROUP_FILES:
			case FGROUP_APPSPECIAL:
				re->cluster = 1;
				break;
			case FGROUP_EXEC:
			case FGROUP_LIBMAIN:
			case FGROUP_DCIM:
			case FGROUP_MOVIE:
			case FGROUP_MUSIC:
				re->cluster = NR_CLUSTER - 1;
				break;
			case FGROUP_EXT_JOURNAL:
			case FGROUP_EXT_SPECIAL_JOURNAL:
			case FGROUP_EXT_BAK:
			case FGROUP_EXT_TMP:
			case FGROUP_EXT_WAL:
			case FGROUP_EXT_DBETC:
			case FGROUP_APPWEBVIEW:
			case FGROUP_APPOTHERS:
			case FGROUP_CACHE_INDEX:
			case FGROUP_DATABASES:
			case FGROUP_LOCAL:
			case FGROUP_SYSTEM:
				re->cluster = 0;
				break;
			default:
				re->cluster = 1;
				break;
		}
	}
	//printk("insert tree ino:%u\n", ino);
	rb_link_node(&re->rb_node, parent, p);
	rb_insert_color(&re->rb_node, root);

//	printk("insert fgroup: %llu %d\n", fgroup, re->cluster);

	return re;
}

static int update_fgroup_entry_valid(struct f2fs_sb_info *sbi, unsigned long long fgroup, int valid, char* name, int cold)
{
	struct fgroup_entry* entry;
	entry = lookup_fgroup_entry(&sbi->fgroup_tree, fgroup);
	
	if (entry == NULL)
		entry = insert_fgroup_entry(sbi, &sbi->fgroup_tree, fgroup, name);

	if (valid < 0) {
		int valid_size = 0 - valid;
		if (valid_size > entry->valid) {
			entry->valid = 0;
			return 0;
		}
	}

	if (cold == 1) 
		entry->cold += valid;

	entry->valid += valid;

	return 0;
}

static int clear_fgroup_cold(struct f2fs_sb_info *sbi, unsigned long long fgroup, int cold)
{
	struct fgroup_entry* entry;
	entry = lookup_fgroup_entry(&sbi->fgroup_tree, fgroup);
	
	if (entry == NULL)
		return 0;

	if (entry->cold < cold) {
		entry->cold = 0;
		return 0;
	}

	entry->cold -= cold;

	return 0;
}

static int convert_fgroup_to_special_journal(struct f2fs_sb_info *sbi, struct inode *inode)
{
	struct fgroup_entry *fe, *jentry;
	struct f2fs_inode_info *ei = F2FS_I(inode);
	struct file_entry *re;
	struct rb_root *root = &sbi->fgroup_tree;
	fe = lookup_fgroup_entry(&sbi->fgroup_tree, ei->i_fgroup);

	if (fe == NULL)
		return 0;

	spin_lock(&sbi->ltree_lock);
	re = lookup_file_entry(&sbi->lifetime_tree, inode->i_ino);
	if (re != NULL) {
		re->filetype = FGROUP_EXT_SPECIAL_JOURNAL; 
	}
	ei->i_filetype = FGROUP_EXT_SPECIAL_JOURNAL; 
	ei->i_fgroup = FGROUP_EXT_SPECIAL_JOURNAL;
	spin_unlock(&sbi->ltree_lock);

	jentry = lookup_fgroup_entry(&sbi->fgroup_tree, FGROUP_EXT_SPECIAL_JOURNAL);
	if (jentry == NULL)
		jentry = insert_fgroup_entry(sbi, &sbi->fgroup_tree, FGROUP_EXT_SPECIAL_JOURNAL, "KEY-7:JOURNAL");
	
	jentry->cold += fe->cold;
	jentry->count += fe->count;
	jentry->latest_lifetime += fe->latest_lifetime;
	jentry->latest_count += fe->latest_count;
	jentry->valid += fe->valid;

	rb_erase(&fe->rb_node, root);
	vfree(fe);

	return 0;
}

static int update_fgroup_entry(struct f2fs_sb_info *sbi, unsigned long long fgroup,  unsigned long long lifetime, unsigned int count, char* name)
{
	struct fgroup_entry* entry;
	entry = lookup_fgroup_entry(&sbi->fgroup_tree, fgroup);
	
	if (entry == NULL)
		entry = insert_fgroup_entry(sbi, &sbi->fgroup_tree, fgroup, name);

	entry->count += count;
	entry->latest_count += count;
	entry->latest_lifetime += (lifetime * count);
	return 0;
}

int delete_fgroup_entry(struct f2fs_sb_info *sbi, unsigned int ino)
{
	int vtype = 0;
	struct rb_root *root = &sbi->fgroup_tree;
	spin_lock(&sbi->ftree_lock);

	for (vtype = 0; vtype <= FGROUP_ETC; vtype++) {
		unsigned long long key = (((unsigned long long)ino) << 5) + vtype;
		struct fgroup_entry *re = lookup_fgroup_entry(root, key);
		if (re != NULL) {
			insert_fgroup_history(sbi, re);
			rb_erase(&re->rb_node, root);
			vfree(re);
		}
	}
	spin_unlock(&sbi->ftree_lock);
	return 0;
}

static int get_cluster(struct f2fs_sb_info *sbi, unsigned long long fgroup)
{
	struct fgroup_entry* entry;

#if 0
	if (check_hot_stream(fgroup) > 0) 
		return 0;
#endif 

	entry = lookup_fgroup_entry(&sbi->fgroup_tree, fgroup);
	if (entry == NULL)
		return -1;

//	printk("get_cluster: %llu %d\n", fgroup, entry->cluster); 
	
	return entry->cluster;
}

int update_cluster(struct f2fs_sb_info *sbi, unsigned long long fgroup, int cluster)
{
	struct fgroup_entry* entry;

	entry = lookup_fgroup_entry(&sbi->fgroup_tree, fgroup);
	if (entry == NULL) {
		return 0;
	}
	entry->cluster = cluster;
//	printk("update_cluster: %llu %d\n ", fgroup, cluster);

	return 0;
}

int get_pstream(struct f2fs_sb_info *sbi, unsigned long long fgroup)
{
	int pstream;
	pstream = get_cluster(sbi, fgroup);
	if (pstream == -1) {
		pstream = NR_CLUSTER - 1;
	}
	pstream = pstream + CLUSTER_START_TYPE;
	if (pstream >= CLUSTER_START_TYPE + NR_CLUSTER) {
		printk("pstream:%d\n", pstream);
	}

	if (pstream == NR_CLUSTER - 1)
		return CURSEG_COLD_DATA;

	return pstream;
}

/////////////////////////////////////////
////////////////////////////////////////

///////////////////////////////////////////

#if 1
#define RESET_RATE	(2)
static int reset_sample_lifetime(struct f2fs_sb_info *sbi, struct file_entry *re)
{
	unsigned long long cur_seq = user_data_blocks(sbi);
	int i;
	int delete_idx = re->last_kmeans % (RESET_RATE);

	if (re->max_range < MAX_SAMPLE)
		return 0;

	for (i = 0; i < MAX_SAMPLE; i++) {
		int sample_index = re->sample_index[i];
		if (i != delete_idx)
			continue; 
		delete_idx = delete_idx + RESET_RATE; 
		if (sample_index < 0)
			continue;
		if (re->sample_lifetime[i] > 0) {
			re->sample_lastupdate[i] = 0;
			re->sample_lifetime[i] = 0;
			re->sample_invalid[i] = 0;
			re->sample_index[i] = -1;
		}
	}

	return 0;
}

static unsigned long update_sample_lifetime(struct f2fs_sb_info *sbi, struct inode* inode, struct file_entry *re, int index, int unlink)
{
	unsigned long long cur_seq = user_data_blocks(sbi);
	int i;
	unsigned long lifetime = 0;
	int sampling = 0;
	struct f2fs_inode_info *ei = F2FS_I(inode);
	int found = 0;

	if (sbi->kmeans_count > re->last_kmeans) {
		reset_sample_lifetime(sbi, re);
		re->last_kmeans = sbi->kmeans_count;
	}

	re->sample_rate += 1;

	if (re->max_range < index) {
		re->max_range = index;
//		if (re->max_range > MAX_SAMPLE)
//			printk("max_range: %s %d\n", ei->i_name, re->max_range);
	}

	// find sample 
	for (i = 0; i < MAX_SAMPLE; i++) {
		int sample_index = re->sample_index[i];
		if (sample_index == index) {
//			printk("[F]index:%d/%d %llu %s\n", index, i, cur_seq, ei->i_name);
			if (unlink == -1) {
				re->sample_lastupdate[i] = cur_seq;
				re->sample_lifetime[i] = 0;
				re->sample_invalid[i] = 0;
				return 0; 
			}
			else if (re->sample_lastupdate[i] > 0) {
				if (cur_seq == re->sample_lastupdate[i]) {
				//	printk("cur_seq=sample_lastupdate: %s %llu\n", ei->i_name, cur_seq);
				}
				lifetime = (cur_seq - re->sample_lastupdate[i]);
				re->sample_lifetime[i] += (cur_seq - re->sample_lastupdate[i]);
				re->sample_invalid[i] += 1;
//				if (strstr(ei->i_keyword, "icing-indexapi") != NULL)
//					printk("[L]index:%d/%d %llu %s\n", index, i, lifetime, ei->i_name);
			}

			if (unlink == 1) {
//				if (strstr(ei->i_keyword, "icing-indexapi") != NULL)
//					printk("[D]index:%d/%d %s\n", index, i, ei->i_name);
				re->sample_lastupdate[i] = 0;
				re->sample_lifetime[i] = 0;
				re->sample_invalid[i] = 0;
				re->sample_index[i] = -1;
			} else {
				re->sample_lastupdate[i] = cur_seq;
			}

			if (lifetime == 0)
				return 0;

			found = 1;
			break;
		}
	}

	if (found == 1) {
		unsigned long lifetime_sum = 0;
		int lifetime_count = 0;
		for (i = 0; i < MAX_SAMPLE; i++) {
			unsigned long sample_lifetime = re->sample_lifetime[i];
			if (re->sample_index[i] >= 0 && re->sample_invalid[i] > 0) {
				lifetime_sum += (sample_lifetime / re->sample_invalid[i]);
				lifetime_count += 1;
			}
		}
		if (lifetime_count > 0)
			re->sample_avglifetime = lifetime_sum / lifetime_count;
		else
			re->sample_avglifetime = 0;
#ifdef F2FS_TRACE_ENABLE
		trace_printk("[DB-lifetime]\t%lld\t%u\t%u\t%u\t%llu\t%s\t%s\n",
				index, cur_seq - lifetime, 2, 0, cur_seq, ei->i_keyword, ei->i_name);
#endif

		return lifetime;
	}

	// new_sample
	if (unlink != 1) {
		if (re->max_range < MAX_SAMPLE) {
			sampling = 1;
		} 
		else if (re->sample_rate >= SAMPLE_RATE) {	
			sampling = 1;
		}
	}

	if (sampling == 1) {
		for (i = 0; i < MAX_SAMPLE; i++) {
			int sample_index = re->sample_index[i];
			if (sample_index == -1) {
				re->sample_lastupdate[i] = cur_seq;
				re->sample_lifetime[i] = 0;
				re->sample_invalid[i] = 0;
				re->sample_index[i] = index;
//				if (strstr(ei->i_keyword, "icing-indexapi") != NULL)
//					printk("[I]:%d/%d %s\n", index, i, ei->i_name);
				break;
			}
		}
		re->sample_rate = 0;
	} else {
//		if ((strstr(ei->i_keyword, "icing-indexapi") != NULL) && (unlink != 1))
//			printk("no sampling:%d %s\n", index, ei->i_name);
	}

	if (unlink == -1)
		return 0;

//	if ((strstr(ei->i_keyword, "icing-indexapi") != NULL))
//		printk("[Avg]:%d %s\n", index, ei->i_name);
#ifdef F2FS_TRACE_ENABLE
	trace_printk("[DB-lifetime]\t%lld\t%u\t%u\t%u\t%llu\t%s\t%s\n",
			index, 0, 3, 0, re->sample_avglifetime, ei->i_keyword, ei->i_name);
#endif

	return re->sample_avglifetime;
}
#endif

///////////////////////////////////////////
int update_file_lifetime(struct f2fs_sb_info *sbi, struct inode *inode, loff_t range_start, long page_written, int unlink)
{
	struct file_entry *re;
	unsigned long long cur_seq = user_data_blocks(sbi);
	struct f2fs_inode_info *ei = F2FS_I(inode);
	unsigned long long lifetime = 0;

	if (page_written == 0)
		return 0;

	spin_lock(&sbi->ltree_lock);
	re = lookup_file_entry(&sbi->lifetime_tree, inode->i_ino); 
	if (re == NULL) {
		if (unlink == 1) {
			spin_unlock(&sbi->ltree_lock);
			return 0; 
		}
		re = insert_file_entry(sbi, &sbi->lifetime_tree, inode->i_ino);
	}

#if 1 
	if (range_start == 0) {
		if (unlink == -1) 
			re->lifetime = 0;
		else if (re->last_0_update > 0)
			re->lifetime = cur_seq - re->last_0_update;
		else if (unlink == 1) {
			re->lifetime = 0;
			re->last_0_update = 0;
			spin_unlock(&sbi->ltree_lock);
			return 0;
		}
		re->last_0_update = cur_seq; 
	}
#else
	if (range_start <= re->last_offset) {
		if (re->last_0_update > 0)
			re->lifetime = cur_seq - re->last_0_update;
		re->last_0_update = cur_seq;
	}
	re->last_offset = range_start;
#endif
	if ((ei->i_filetype == FGROUP_DATABASES) && (range_start > 0)) {
		lifetime = update_sample_lifetime(sbi, inode, re, range_start, unlink);
	} else 
		lifetime = re->lifetime;

	spin_unlock(&sbi->ltree_lock);

	if (lifetime > 0) {
		if (ei->i_filetype == FGROUP_INIT) {
			ei->i_fgroup = get_fgroup(sbi, inode, NULL); 
		} else if (strstr(ei->i_keyword, "KEY-INIT") != NULL) {
			ei->i_fgroup = get_fgroup(sbi, inode, NULL); 
		}
		spin_lock(&sbi->ftree_lock);
		update_fgroup_entry(sbi, ei->i_fgroup, lifetime, page_written, ei->i_keyword);
		if (re->cold == 1) {
			clear_fgroup_cold(sbi, ei->i_fgroup, re->valid);
			re->cold = 0;
		}
		if (ei->i_filetype == FGROUP_EXT_JOURNAL) {
			if (unlink == 1 && range_start == 0) {
				convert_fgroup_to_special_journal(sbi, inode);
			}
		}
		spin_unlock(&sbi->ftree_lock);
		if (ei->i_filetype == FGROUP_DATABASES && range_start == 0) {
			int i = 0;
			for (i = 0; i < page_written; i++) {	 
#ifdef F2FS_TRACE_ENABLE
				trace_printk("[DB-lifetime]\t%lld\t%u\t%u\t%u\t%llu\t%s\t%s\n",
					range_start+i, cur_seq - lifetime, 1, 0, cur_seq, ei->i_keyword, ei->i_name);
#endif
			}
		}
	}
	return 0;
}

#ifndef EXT4_MSTREAM
int update_file_valid(struct f2fs_sb_info *sbi, struct inode *inode, int valid)
{
	struct file_entry *re;
	int update_valid = valid;
	struct f2fs_inode_info *ei = F2FS_I(inode);

	spin_lock(&sbi->ltree_lock);
	re = lookup_file_entry(&sbi->lifetime_tree, inode->i_ino); 
	if (re == NULL) {
		if (valid < 0) {
			spin_unlock(&sbi->ltree_lock);
			return 0;
		}
		re = insert_file_entry(sbi, &sbi->lifetime_tree, inode->i_ino);
	}

	if (valid == 0) {
		update_valid = 0 - re->valid;
	}
	else if (valid < 0) {
		int valid_size = 0 - valid;
		if (valid_size > re->valid) {
			update_valid = 0 - re->valid;
		}
	}

	re->valid += update_valid; 
	spin_unlock(&sbi->ltree_lock);

	if (ei->i_filetype == FGROUP_INIT) {
		ei->i_fgroup = get_fgroup(sbi, inode, NULL); 
	} else if (strstr(ei->i_keyword, "KEY-INIT") != NULL) {
		ei->i_fgroup = get_fgroup(sbi, inode, NULL); 
	}
	spin_lock(&sbi->ftree_lock);
	update_fgroup_entry_valid(sbi, ei->i_fgroup, update_valid, ei->i_keyword, re->cold);
	spin_unlock(&sbi->ftree_lock);

	return 0;
}
#else
int update_file_valid(struct ext4_sb_info *sbi, struct inode *inode)
{
    struct file_entry *re;
    unsigned int size = (inode->i_size + 4095) / 4096;
    int update_valid, valid;
    struct ext4_inode_info *ei = EXT4_I(inode);

    spin_lock(&sbi->ltree_lock);
    re = lookup_file_entry(&sbi->lifetime_tree, inode->i_ino);
    if (re == NULL) {
        re = insert_file_entry(sbi, &sbi->lifetime_tree, inode->i_ino);
    }

    if (size == re->valid) {
        spin_unlock(&sbi->ltree_lock);
        return 0;
    }

    valid = size - re->valid;
    update_valid = valid;

    if (valid == 0) {
        update_valid = 0 - re->valid;
    }
    else if (valid < 0) {
        int valid_size = 0 - valid;
        if (valid_size > re->valid) {
            update_valid = 0 - re->valid;
        }
    }
    re->valid += update_valid;
    spin_unlock(&sbi->ltree_lock);

    if (ei->i_filetype == FGROUP_INIT) {
        ei->i_fgroup = get_fgroup(sbi, inode, NULL);
    } else if (strstr(ei->i_keyword, "KEY-INIT") != NULL) {
        ei->i_fgroup = get_fgroup(sbi, inode, NULL);
    }
    spin_lock(&sbi->ftree_lock);
    update_fgroup_entry_valid(sbi, ei->i_fgroup, update_valid, ei->i_keyword, re->cold);
    spin_unlock(&sbi->ftree_lock);

    return 0;
}
#endif
#endif


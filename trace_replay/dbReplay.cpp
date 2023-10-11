#include "dbReplay.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <ftw.h>
#include <search.h>
#include <iostream>
#include <map>
#include <string>
#include <list>
#include <vector>
#include <fstream>


#define MAX_SIZE 40960
#define MAX_BLOCK 10000
#define MAX_FILE 10

struct stats_info
{
    string path;
	DBTYPE type;

    int tr_count;
    int tr_after_size;

    int access_first[MAX_SIZE];
    int access_cnt[MAX_SIZE];

	int access_avg_day[MAX_FILE];
	int sum_access_avg;

    long long int cur_file_size;
    int max_file_size;

    int install_file_size;
};

struct request
{
    int start;
    int size;
};

struct dirty_info
{
    string path;
    int req_type;
    long long int off;
    long long int size;
    long long int file_size;
};

map<string, struct dirty_info*> dirty_DB;
map<string, struct stats_info*> update_DB;
map<string, struct stats_info*> rename_DB;

map<string, double> file_DB;
double cur_time = 0.0;
int g_cur_seq = 1;

#define SPRINTF_LOADING_PATH(BUF, PATH, NAME, NUM) \
            memset(BUF, 0, PATH_MAX); \
            sprintf(BUF, "%s/TRACE_%s%d_loading_ALL.input", PATH, NAME, NUM);
#define SPRINTF_UPDATE_PATH(BUF, PATH, NAME) \
            memset(BUF, 0, PATH_MAX); \
            sprintf(BUF, "%s/TRACE_%s_update_ALL.input", PATH, NAME);
#define SPRINTF_INSTALL_PATH(BUF, PATH, NAME) \
            memset(BUF, 0, PATH_MAX); \
            sprintf(BUF, "%s/TRACE_%s_install_ALL.input", PATH, NAME);

#define FI_UNLINK   0
#define FI_RENAME   1
#define FI_TRUNCATE 2

#define REQ_APPEND  0
#define REQ_UPDATE  1


struct db_config
{
    string app_path;
    string app_ps;
    string app_name;
};

struct db_config dbconfig;

static double trace_analysis(char *input_name);
static int do_update_stats_DB_for_file(struct dirty_info* dirty);
static int update_stats_DB_for_delete(string path, int delete_type);
static int update_request(string path, long long int offset, long long int write_size, long long int file_size, int type);
static int update_stats_DB_for_bg(void);
static int update_stats_DB_for_file(string path, int fsync);
static int update_stats_DB_for_tr(string path, int before_size, int after_size, string input_name);
static int update_db_stat(string path, int start_index, int size, string type, int filesize);

void analysis_database(vector<struct DBInfo*> &DBvec, string app_path, string app_name, string app_ps, int total_loading_file)
{
    int i;
    map<string, struct stats_info*>::iterator it;
    char open_file_name[PATH_MAX + 1];

	dbconfig.app_path = app_path;
	dbconfig.app_name = app_name;
	dbconfig.app_ps = app_ps;

    SPRINTF_INSTALL_PATH(open_file_name, app_path.c_str(), app_name.c_str());
    trace_analysis(open_file_name);

    for (it = update_DB.begin(); it != update_DB.end(); it++)
    {
		struct stats_info *info = it->second;
		info->install_file_size = info->max_file_size;
		info->type = DBTYPE_INSERT;
	}

    for (i = 1; i <= total_loading_file; i++)
    {
        SPRINTF_LOADING_PATH(open_file_name, app_path.c_str(), app_name.c_str(), i);
        trace_analysis(open_file_name);
    }

    for (it = update_DB.begin(); it != update_DB.end();)
    {
        struct stats_info *info = it->second;
		struct DBInfo *result = new struct DBInfo;
		info->install_file_size = (info->install_file_size + 4095) / 4096;
		info->max_file_size = (info->max_file_size + 4095) / 4096;

		if (info->max_file_size <= info->install_file_size) {
			info->type = DBTYPE_FIXED;
		} 
		if (info->path.find("-journal") != std::string::npos) {
			info->type = DBTYPE_JOURNAL;
		}
		else if (info->path.find("-wal") != std::string::npos) {
			info->type = DBTYPE_JOURNAL;
		}
		else if (info->path.find("-shm") != std::string::npos) {
			info->type = DBTYPE_JOURNAL;
		}

		result->path = info->path;
		result->type = info->type;
		result->meta_offset = info->install_file_size;
		result->limit_size = info->max_file_size * 10;

		if (info->type == DBTYPE_INSERT) {
			int valid_block = 0;
			int cold_block = 0;
			int sum_access = 0;
			for (i = info->install_file_size; i < info->max_file_size; i++) {
				if (info->access_cnt[i] == 0) 
					cold_block++;
				else {
					valid_block++;
					sum_access += info->access_cnt[i];
				}
			}
			result->cold_brate = (cold_block * 100) / (cold_block + valid_block);
			if (valid_block > 0) {
				int avg_access = (sum_access / valid_block);
				int hot_count = 0, hot_wsum = 0, warm_count = 0, warm_wsum = 0;
				for (i = info->install_file_size; i < info->max_file_size; i++) {
					if (info->access_cnt[i] == 0) {}
					else if (avg_access < info->access_cnt[i]) {
						hot_count++;
						hot_wsum += info->access_cnt[i];			
					}
					else {
						warm_count++;
						warm_wsum += info->access_cnt[i];
					}
				}
				result->hot_brate = (hot_count * 100) / (valid_block + cold_block);
				result->hot_wrate = (hot_wsum * 100) / (hot_wsum + warm_wsum);	
			} else {
				result->hot_brate = 0;
				result->hot_wrate = 0;
			}
		} else {
			result->cold_brate = 0;
			result->hot_brate = 0;
			result->hot_wrate = 0;
		}
		DBvec.push_back(result);

        delete info;
        update_DB.erase(it++);
    }
}

static double trace_analysis(char *input_name)
{
    double last_time;
    char line[2048];
    FILE *input_fp;
    bool first_trace = true;
    string str_line;
    double flush_time = 0;

    input_fp = fopen(input_name, "r");
    if (input_fp == NULL)
        return -1;
    memset(line, 0, 2048);
    cur_time = 0;

    while (fgets(line, 2048, input_fp) != NULL)
    {
        char *ptr;
        char *ptr_2;
        char type[10];
        char tmp[PATH_MAX + 1];
        char path[PATH_MAX + 1];
        str_line = string(line);

        memset(path, 0, PATH_MAX);
        ptr = strtok(line, "\t");
        if (ptr == NULL)
            continue;
        last_time = strtod(ptr, &ptr_2);
        if (last_time > (3 * 60 * 60))
            cur_time = cur_time + 1;
        else
            cur_time = last_time;

        if (first_trace) {
            first_trace = false;
        }

        if (flush_time < cur_time) {
            update_stats_DB_for_bg();
            flush_time = cur_time + 5;
        }

        ptr = strtok(NULL, "\t");
        if (ptr == NULL)
            continue;
        strncpy(type, ptr, strlen(ptr));
        type[strlen(ptr)] = 0x00;

        ptr = strtok(NULL, "\t");
        if (ptr == NULL)
            continue;
        strncpy(tmp, ptr, strlen(ptr));
        tmp[strlen(ptr)] = 0x00;
        ptr_2 = tmp;

        sprintf(path, "%s", tmp);
        if (string(path).find("/com.") == 0) {
            sprintf(path, "data/data%s", tmp);
        }
        else
            sprintf(path, "data%s", tmp);

        if (string(path).find(dbconfig.app_ps) == std::string::npos) {
            continue;
        }
        if (string(path).find("/databases/") == std::string::npos) {
            continue;
        }

        if (strncmp(type, "[CR]", 4) == 0) {
            update_stats_DB_for_delete(string(path), FI_RENAME);
        }
        else if (strncmp(type, "[UN]", 4) == 0) {
            map<string, double>::iterator it;
            update_stats_DB_for_delete(string(path), FI_UNLINK);
        }
        else if (strncmp(type, "[FS]", 4) == 0)
        {
            int sync_option;
            ptr = strtok(NULL, "\t");
            if (ptr == NULL)
                continue;
            sync_option = atoi(ptr);
            update_stats_DB_for_file(string(path), 1);
        }
        else if (strncmp(type, "[WO]", 4) == 0)
        {
            long long int write_off;
            long long int write_size;
            long long int file_size;

            ptr = strtok(NULL, "\t");
            if (ptr == NULL)
                continue;
            write_off = atoll(ptr);
            ptr = strtok(NULL, "\t");
            if (ptr == NULL)
                continue;
            write_size = atoll(ptr);
            ptr = strtok(NULL, "\t");
            if (ptr == NULL)
                continue;
            file_size = atoll(ptr);

            update_request(string(path), write_off, write_size, file_size, REQ_UPDATE);
        }
        else if (strncmp(type, "[WA]", 4) == 0)
        {
            long long int write_off;
            long long int write_size;
            long long int file_size;

            ptr = strtok(NULL, "\t");
            if (ptr == NULL)
                continue;
            write_off = atoll(ptr);
            ptr = strtok(NULL, "\t");
            if (ptr == NULL)
                continue;
            write_size = atoll(ptr);
            ptr = strtok(NULL, "\t");
            if (ptr == NULL)
                continue;
            file_size = atoll(ptr);

            update_request(string(path), write_off, write_size, file_size, REQ_APPEND);
        }
        else if (strncmp(type, "[RD]", 4) == 0)
        {
            map<string, double>::iterator it;
            vector<string> path_vec;
            string dir_path = string(path) + string("/");
            it = file_DB.find(string(path));
            if (it != file_DB.end()) {
                path_vec.push_back(it->first);
            }

            for(it = file_DB.begin(); it != file_DB.end(); ++it)
            {
                if (it->first.find(dir_path) == std::string::npos)
                    continue;
                path_vec.push_back(it->first);
            }
            vector<string>::iterator it_vec;
            for(it_vec = path_vec.begin(); it_vec != path_vec.end(); ++it_vec)
            {
                string target_file = (*it_vec);
                it = file_DB.find(string(target_file));
                if (it != file_DB.end()) {
                    update_stats_DB_for_delete(string(path), FI_UNLINK);
                    file_DB.erase(it);
                }
            }
        }
        else if (strncmp(type, "[TR]", 4) == 0)
        {
            long long int after_size;
            long long int before_size;
            map<string, double>::iterator it;

            ptr = strtok(NULL, "\t");
            if (ptr == NULL)
                continue;
            after_size = atoll(ptr);
            ptr = strtok(NULL, "\t");
            if (ptr == NULL)
                continue;
            before_size = atoll(ptr);

            update_stats_DB_for_tr(string(path), before_size, after_size, string(input_name));
        }
        else
            continue;
    }
out:
    fclose(input_fp);
    return last_time;
}

static int update_request(string path, long long int offset, long long int write_size, long long int file_size, int type)
{
    long long int write_end = offset + write_size - 1;
    int found = 0;
    int i = 0;
    struct dirty_info *info;

    // TODO: SEARCH dirty DB
    map<string, struct dirty_info*>::iterator map_it;
    map_it = dirty_DB.find(path);

    if (map_it == dirty_DB.end())
    {
        info = new dirty_info;
        // insert new dirty DB
        info->path = path;
        info->off = offset;
        info->size = write_size;
        if (info->off + info->size > file_size)
            info->file_size = info->off + info->size;
        else
            info->file_size = file_size;
        info->req_type = type;
        dirty_DB.insert(pair<string, struct dirty_info*>(path, info));
    }
    else {
        info = map_it->second;
        if (type != info->req_type) {
            do_update_stats_DB_for_file(info);
            info->req_type = type;
            info->off = offset;
            info->size = write_size;
        }
        else if ((info->off + info->size) == offset)
            info->size = info->size + write_size;
        else {
            do_update_stats_DB_for_file(info);
            info->off = offset;
            info->size = write_size;
        }
        if (info->off + info->size > file_size)
            info->file_size = info->off + info->size;
        else
            info->file_size = file_size;

        if (file_size > info->file_size)
            info->file_size = file_size;
    }
    return 0;
}

static int init_info(struct stats_info *info, string path)
{
    int i;
    info->path = path;
    info->cur_file_size = 0;

    for (int i = 0; i < MAX_SIZE; i++) {
        info->access_first[i] = 0;
        info->access_cnt[i] = 0; 
    }

	info->tr_count = 0;
	info->tr_after_size = MAX_SIZE * 4096;
    info->max_file_size = 0;
    info->type = DBTYPE_INSERT;

    return 0;
}

static int update_stats_DB_for_delete(string path, int delete_type)
{
    struct stats_info *info;
    map<string, struct stats_info*>::iterator map_it;
    update_stats_DB_for_file(path, 0);
    int delete_size;

    map_it = update_DB.find(path);
    if (map_it == update_DB.end())
    {
        info = new stats_info;
        init_info(info, path);

        update_DB.insert(pair<string, struct stats_info*>(path, info));
    }
    else {
        info = map_it->second;
    }
    delete_size = (info->cur_file_size + 4095) / 4096;

    update_db_stat(path, 0, delete_size, "[UN]", info->cur_file_size);
    info->cur_file_size = 0;

    return 0;
}

static int do_update_stats_DB_for_file(struct dirty_info* dirty)
{
    int i = 0;
    struct stats_info *info;
    map<string, struct stats_info*>::iterator map_it;
    map_it = update_DB.find(dirty->path);
    long long int off_end = dirty->off + dirty->size - 1;
    long long int start_block = dirty->off / 4096;
    long long int end_block = off_end / 4096;
    long long int update_size_block = (end_block - start_block + 1);

    if (map_it == update_DB.end())
    {
        info = new stats_info;
        init_info(info, dirty->path);

        update_DB.insert(pair<string, struct stats_info*>(dirty->path, info));
    }
    else {
        info = map_it->second;
    }

    g_cur_seq += update_size_block;

	update_db_stat(dirty->path, start_block, update_size_block, "[W]", info->cur_file_size);

    info->cur_file_size = dirty->file_size;

    if (info->cur_file_size > info->max_file_size) {
        info->max_file_size = info->cur_file_size;
    }
    if (off_end > info->max_file_size) {
        info->max_file_size = info->cur_file_size;
    }

    return 0;
}


static int update_stats_DB_for_file(string path, int fsync)
{
    map<string, struct dirty_info*>::iterator it = dirty_DB.find(path);
    if (it == dirty_DB.end())
        return 0;
    struct dirty_info *info = it->second;
    do_update_stats_DB_for_file(info);
    delete info;
    dirty_DB.erase(it);
    return 0;
}

static int update_stats_DB_for_tr(string path, int before_size, int after_size, string input_name)
{
    struct stats_info *info;
    map<string, struct stats_info*>::iterator map_it;
    update_stats_DB_for_file(path, 0);
    int delete_size;
    int before_index, after_index;

    map_it = update_DB.find(path);
    if (map_it == update_DB.end())
    {
        info = new stats_info;
        init_info(info, path);

        update_DB.insert(pair<string, struct stats_info*>(path, info));
        return 0;
    }
    else {
        info = map_it->second;
    }

    if (before_size > info->cur_file_size)
        before_size = info->cur_file_size;

    if (after_size >= info->cur_file_size) {
        return 0;
    }

    before_index = (info->cur_file_size + 4095) / 4096;
    after_index = (after_size + 4095) / 4096;
    delete_size = before_index - after_index;

    if (info->tr_after_size == after_index) {
        info->tr_count++;
        if (info->tr_count == 5) {
            info->type = DBTYPE_DELETE;
		}
    }
    else if (info->tr_after_size > after_index) {
		info->tr_after_size = after_index;
        info->tr_count = 0;
    }

	update_db_stat(info->path, after_index, delete_size, "[TR]", info->cur_file_size);
    info->cur_file_size = after_size;

    if (info->cur_file_size > info->max_file_size)
        info->max_file_size = info->cur_file_size;

    return 0;
}

static int update_stats_DB_for_bg(void)
{
    map<string, struct dirty_info*>::iterator it;

    for (it = dirty_DB.begin(); it != dirty_DB.end();)
    {
        struct dirty_info *info = it->second;
        do_update_stats_DB_for_file(info);
        delete info;
        dirty_DB.erase(it++);
    }
    return 0;
}

static int reset_file_DB(void)
{
    map<string, double>::iterator it;

    for (it = file_DB.begin(); it != file_DB.end();)
    {
        file_DB.erase(it++);
    }
    return 0;
}

static int update_db_stat(string path, int start_index, int size, string type, int filesize)
{
    map<string, struct stats_info*>::iterator info_it;
    struct stats_info *info;
    int i;
    int invalidate = 1;

    if (path.find(dbconfig.app_ps) == std::string::npos) {
        return 0;
    }

    if (path.find("/databases/") == std::string::npos) {
        return 0;
    }

    if (type.find("W") != std::string::npos)
        invalidate = 0;

    info_it = update_DB.find(path);
    info = info_it->second;

    if (start_index + size > MAX_SIZE) {
        cout << path <<  " DB index/size:" << start_index << "/" << size << endl;
    }

    for (i = start_index; i < start_index + size; i++) {
        if (invalidate == 0) {
	        if (info->access_first[i] > 0) {
				info->access_cnt[i] += 1;
	        }
            info->access_first[i] = 1;
		}
        else
            info->access_first[i] = 0;
    }
    return 0;
}

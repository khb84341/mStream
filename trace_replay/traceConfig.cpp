#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <iostream>
#include <list>
#include <string>
#include <limits>
#include <cfloat>
#include <dirent.h>
#include "traceReplay.h"
#include "cJSON.h"

static int trace_replay(char *config_name, int day);
static int parse_multimedia(cJSON *multi_obj, struct Config *config);
static int parse_basic_app(cJSON *basic_obj, struct Config *config);
static int parse_normal_app(cJSON *normal_obj, struct Config *config);
static int parse_apps(cJSON *apps_obj, struct App *apps, 
			double default_update, double default_loading, double default_bg);
static int parse_ps_name(cJSON *ps_name_obj, struct Config *config);
static int parse_fulldisk(cJSON *ps_full_obj, struct Config *config);
static double parse_time(string line);


int parse_config(char *config_name, struct Config *config)
{
	FILE *config_fp;
	long len = 0;
	char *config_data;
	int ret = 0;
	cJSON *root_obj;
	cJSON *mount_dir_obj;
	cJSON *init_filemap_obj;
	cJSON *backup_path_obj;
	cJSON *multimedia_obj;
	cJSON *basic_app_obj;
	cJSON *normal_app_obj;
	cJSON *ps_name_obj;
	cJSON *ps_full_obj;

	config_fp = fopen(config_name, "r");
	if (config_fp == NULL) {
		printf("error: cannot read %s\n", config_name);
		ret = -1;
		return -1;
	}
	
	fseek(config_fp, 0, SEEK_END);
	len = ftell(config_fp);
	fseek(config_fp, 0, SEEK_SET);

	config_data = (char *)malloc(len);
	if (config_data == NULL) {
		printf("error: failed malloc\n");
		fclose(config_fp);
		return -1;
	}
	fread(config_data, sizeof(char), len, config_fp);

	root_obj = cJSON_Parse(config_data);
	if (root_obj == NULL) {
		printf("error: %s\n", cJSON_GetErrorPtr());
		ret = -1;
		goto out;
	}

	// MOUNT_DIR
	memset(config->mount_dir, 0, PATH_MAX);
	mount_dir_obj = cJSON_GetObjectItem(root_obj, "MOUNT_DIR");
	if (mount_dir_obj == NULL) {
		sprintf(config->mount_dir, "data");
	} else {
		sprintf(config->mount_dir, "%s", mount_dir_obj->valuestring);
	}

	// INIT_FILEMAP
	memset(config->INIT_FILEMAP, 0, PATH_MAX);
	init_filemap_obj = cJSON_GetObjectItem(root_obj, "INIT_FILEMAP");
	if (init_filemap_obj == NULL) {
		sprintf(config->INIT_FILEMAP, "NULL");
	} else {
		sprintf(config->INIT_FILEMAP, "%s", init_filemap_obj->valuestring);
	}

	// BACKUP_PATH
	memset(config->backup_path, 0, PATH_MAX);
	backup_path_obj = cJSON_GetObjectItem(root_obj, "BACKUP_PATH");
	if (backup_path_obj == NULL) {
		sprintf(config->backup_path, "trace-bak");
	} else {
		sprintf(config->backup_path, "%s", backup_path_obj->valuestring);
	}

	// MULTIMEDIA
	multimedia_obj = cJSON_GetObjectItem(root_obj, "MULTIMEDIA");
	if (multimedia_obj == NULL) {
		printf("error: Parse JSON file, No MULTIMEDIA\n");
		ret = -1;
		goto out;
	} else {
		ret = parse_multimedia(multimedia_obj, config);
		if (ret == -1)
			goto out;
	}

	basic_app_obj = cJSON_GetObjectItem(root_obj, "BASIC_APP");
	if (basic_app_obj == NULL) {
		printf("error: Parse JSON file, No BASIC_APP\n");
		ret = -1;
		goto out;
	} else {
		ret = parse_basic_app(basic_app_obj, config);
		if (ret == -1)
			goto out;
	}

	normal_app_obj = cJSON_GetObjectItem(root_obj, "NORMAL_APP");
	if (normal_app_obj == NULL) {
		printf("error: Parse JSON file, No NORMAL_APP\n");
		ret = -1;
		goto out;
	} else {
		ret = parse_normal_app(normal_app_obj, config);
		if (ret == -1)
			goto out;
	}

	ps_name_obj = cJSON_GetObjectItem(root_obj, "PROCESS");
	if (ps_name_obj == NULL) {
		printf("warning: Failed Parse JSON file, No PROCESS\n");
		ret = 0;
		goto out;
	} else {
		ret = parse_ps_name(ps_name_obj, config);
		if (ret == -1)
			goto out;
	}

	ps_full_obj = cJSON_GetObjectItem(root_obj, "FULLDISK");
	if (ps_full_obj == NULL) {
		printf("error: Failed Parse JSON file, No FULLDISK\n");
		ret = -1;
		goto out;
	} else {
		ret = parse_fulldisk(ps_full_obj, config);
		if (ret == -1)
			goto out;
	}

out:
	fclose(config_fp);
	free(config_data);
	return ret;
}

static int parse_multimedia(cJSON *multi_obj, struct Config *config)
{
	cJSON *camera_obj;
	cJSON *others_obj;
	cJSON *camera_path_obj;
	cJSON *camera_multipath_obj;
	cJSON *camera_takecount_obj;
	cJSON *camera_deletecount_obj;
	cJSON *camera_initcount_obj;
	cJSON *camera_sizeunit_obj;
	cJSON *camera_minsize_obj;
	cJSON *camera_maxsize_obj;
	unsigned long unit;

	camera_obj = cJSON_GetObjectItem(multi_obj, "CAMERA");
	if (camera_obj == NULL) {
		printf("error: No exist CAMERA in MULTIMEDIA");
		return -1;
	}

	config->multi.mul_camera.name = string("camera");
	camera_multipath_obj = cJSON_GetObjectItem(camera_obj, "MULTIMEDIA_PATH");
	if (camera_multipath_obj == NULL) {
		printf("error: No exist MULTIMEDIA_PATH in CAMERA");
		return -1;
	}
	config->multi.mul_camera.multimedia_path = string(camera_multipath_obj->valuestring);

	camera_takecount_obj = cJSON_GetObjectItem(camera_obj, "TAKE_COUNT");
	if (camera_takecount_obj == NULL) {
		printf("error: No exist TAKE_COUNT in CAMERA");
		return -1;
	}
	config->multi.mul_camera.take_count = camera_takecount_obj->valueint;

	camera_deletecount_obj = cJSON_GetObjectItem(camera_obj, "DELETE_COUNT");
	if (camera_deletecount_obj == NULL) {
		printf("error: No exist DELETE_COUNT in CAMERA");
		return -1;
	}
	config->multi.mul_camera.delete_count = camera_deletecount_obj->valueint;

	camera_initcount_obj = cJSON_GetObjectItem(camera_obj, "INIT_COUNT");
	if (camera_deletecount_obj == NULL) {
		printf("error: No exist INIT_COUNT in CAMERA");
		return -1;
	}
	config->multi.mul_camera.init_count = camera_initcount_obj->valueint;

	camera_sizeunit_obj = cJSON_GetObjectItem(camera_obj, "SIZE_UNIT(B)");
	if (camera_sizeunit_obj != NULL) {
		unit = camera_sizeunit_obj->valueint; 
	} else
		unit = 4096;

	camera_minsize_obj = cJSON_GetObjectItem(camera_obj, "MIN_SIZE");
	if (camera_minsize_obj != NULL) {
		config->multi.mul_camera.min_size = camera_minsize_obj->valueint;
		config->multi.mul_camera.min_size *= unit;
	}
	else
		config->multi.mul_camera.min_size = unit;

	camera_maxsize_obj = cJSON_GetObjectItem(camera_obj, "MAX_SIZE");
	if (camera_maxsize_obj != NULL) {
		config->multi.mul_camera.max_size = camera_maxsize_obj->valueint;
		config->multi.mul_camera.max_size *= unit;
	}
	else
		config->multi.mul_camera.max_size = unit;

	others_obj = cJSON_GetObjectItem(multi_obj, "OTHERS");
	if (others_obj != NULL)
	{
		int array_size = cJSON_GetArraySize(others_obj);
		struct MulConf other;
		for (int i = 0; i < array_size; i++)
		{
			cJSON* other_obj = cJSON_GetArrayItem(others_obj, i);
			cJSON* other_name_obj = cJSON_GetObjectItem(other_obj, "NAME");
			if (other_name_obj == NULL) {
				printf("error: No exist NAME in OTHERS");
				continue;
			}
			other.name = string(other_name_obj->valuestring);
			
			cJSON* other_multipath_obj = cJSON_GetObjectItem(other_obj, "MULTIMEDIA_PATH");
			if (other_multipath_obj == NULL) {
				printf("error: No exist MULTIMEDIA_PATH in %s\n", other.name.c_str());
				continue;
			}
			other.multimedia_path = string(other_multipath_obj->valuestring);

			cJSON *other_takecount_obj = cJSON_GetObjectItem(other_obj, "TAKE_COUNT");
			if (camera_takecount_obj == NULL) {
				printf("error: No exist TAKE_COUNT in %s\n", other.name.c_str());
				continue;
			}
			other.take_count = other_takecount_obj->valueint;

			cJSON* other_deletecount_obj = cJSON_GetObjectItem(other_obj, "DELETE_COUNT");
			if (other_deletecount_obj == NULL) {
				printf("error: No exist DELETE_COUNT in %s\n", other.name.c_str());
				continue;
			}
			other.delete_count = other_deletecount_obj->valueint;

			cJSON* other_initcount_obj = cJSON_GetObjectItem(other_obj, "INIT_COUNT");
			if (other_initcount_obj == NULL) {
				printf("error: No exist INIT_COUNT in %s\n", other.name.c_str());
				continue;
			}
			other.init_count = other_initcount_obj->valueint;

			cJSON* other_sizeunit_obj = cJSON_GetObjectItem(other_obj, "SIZE_UNIT(B)");
			if (other_sizeunit_obj != NULL) {
				unit = other_sizeunit_obj->valueint; 
			} else
				unit = 4096;

			cJSON* other_minsize_obj = cJSON_GetObjectItem(other_obj, "MIN_SIZE");
			if (other_minsize_obj != NULL) {
				other.min_size = other_minsize_obj->valueint;
				other.min_size *= unit;
			}
			else
				other.min_size = unit;

			cJSON* other_maxsize_obj = cJSON_GetObjectItem(other_obj, "MAX_SIZE");
			if (other_maxsize_obj != NULL) {
				other.max_size = other_maxsize_obj->valueint;
				other.max_size *= unit;
			}
			else
				other.max_size = unit;
			config->multi.mul_others.push_back(other);
		}
	}
	return 0;
}

static int parse_basic_app(cJSON *basic_obj, struct Config *config)
{
	cJSON *default_update_obj;
	cJSON *default_loading_obj;
	cJSON *default_bg_obj;
	cJSON *apps_obj;
	int app_count;

	default_update_obj = cJSON_GetObjectItem(basic_obj, "DEFAULT_UPDATE_CYCLE");
	if (default_update_obj == NULL) {
		printf("error: No exist DEFAULT_UPDATE_CYCLE in BASIC_APP");
		return -1;
	}
	config->basic_app.default_update_cycle = default_update_obj->valuedouble;

	default_loading_obj = cJSON_GetObjectItem(basic_obj, "DEFAULT_LOADING_CYCLE");
	if (default_loading_obj == NULL) {
		printf("error: No exist DEFAULT_LOADING_CYCLE in BASIC_APP");
		return -1;
	}
	config->basic_app.default_loading_cycle = default_loading_obj->valuedouble;

	default_bg_obj = cJSON_GetObjectItem(basic_obj, "DEFAULT_BG_CYCLE");
	if (default_bg_obj == NULL) {
		config->basic_app.default_bg_cycle = 0;
	}
	config->basic_app.default_bg_cycle = default_bg_obj->valuedouble;

	apps_obj = cJSON_GetObjectItem(basic_obj, "APPS");
	if (apps_obj == NULL) {
		printf("error: No exist APPS in BASIC_APP");
		return -1;
	}
	app_count = cJSON_GetArraySize(apps_obj);
	config->basic_app.app_count = app_count;
	if (app_count > 0) {
		config->basic_app.apps = new struct App[app_count];
		parse_apps(apps_obj, config->basic_app.apps, config->basic_app.default_update_cycle, config->basic_app.default_loading_cycle, config->basic_app.default_bg_cycle);
	}
	
	return 0;
}

static int parse_normal_app(cJSON *normal_obj, struct Config *config)
{
	cJSON *default_update_obj;
	cJSON *default_loading_obj;
	cJSON *default_bg_obj;
	cJSON *install_obj;
	cJSON *uninstall_obj;
	cJSON *apps_obj;
	int app_count;

	default_update_obj = cJSON_GetObjectItem(normal_obj, "DEFAULT_UPDATE_CYCLE");
	if (default_update_obj == NULL) {
		printf("error: No exist DEFAULT_UPDATE_CYCLE in NORMAL_APP");
		return -1;
	}
	config->normal_app.default_update_cycle = default_update_obj->valuedouble;

	default_loading_obj = cJSON_GetObjectItem(normal_obj, "DEFAULT_LOADING_CYCLE");
	if (default_loading_obj == NULL) {
		printf("error: No exist DEFAULT_LOADING_CYCLE in NORMAL_APP");
		return -1;
	}
	config->normal_app.default_loading_cycle = default_loading_obj->valuedouble;

	default_bg_obj = cJSON_GetObjectItem(normal_obj, "DEFAULT_BG_CYCLE");
	if (default_bg_obj == NULL) {
		config->normal_app.default_bg_cycle = 0;
	}
	config->normal_app.default_bg_cycle = default_bg_obj->valuedouble;

	install_obj = cJSON_GetObjectItem(normal_obj, "APP_INSTALL_CYCLE");
	if (install_obj == NULL) {
		printf("error: No exist APP_INSTALL_CYCLE in NORMAL_APP");
		return -1;
	}
	config->normal_app.install_cycle = install_obj->valuedouble;

	uninstall_obj = cJSON_GetObjectItem(normal_obj, "APP_UNINSTALL_CYCLE");
	if (uninstall_obj == NULL) {
		printf("error: No exist APP_UNINSTALL_CYCLE in NORMAL_APP");
		return -1;
	}
	config->normal_app.uninstall_cycle = uninstall_obj->valuedouble;

	apps_obj = cJSON_GetObjectItem(normal_obj, "APPS");
	if (apps_obj == NULL) {
		printf("error: No exist APPS in BASIC_APP");
		return -1;
	}
	app_count = cJSON_GetArraySize(apps_obj);
	config->normal_app.app_count = app_count;
	if (app_count > 0) {
		config->normal_app.apps = new struct App[app_count];
		parse_apps(apps_obj, config->normal_app.apps, config->normal_app.default_update_cycle, config->normal_app.default_loading_cycle, config->normal_app.default_bg_cycle);
	}

	return 0;
}

static int parse_apps(cJSON *apps_obj, struct App *apps, 
			double default_update, double default_loading, double default_bg)
{
	int array_size = cJSON_GetArraySize(apps_obj);
	int i;
	if (array_size == 0) {
		return 0;
	}
	
	for (i = 0; i < array_size; i++)
	{
		cJSON* app_obj = cJSON_GetArrayItem(apps_obj, i);
		cJSON* app_name_obj = cJSON_GetObjectItem(app_obj, "NAME");
		cJSON* app_path_obj = cJSON_GetObjectItem(app_obj, "PATH");
		cJSON* app_update_obj = cJSON_GetObjectItem(app_obj, "UPDATE_CYCLE");
		cJSON* app_loading_obj = cJSON_GetObjectItem(app_obj, "LOADING_CYCLE");
		cJSON* app_bg_obj = cJSON_GetObjectItem(app_obj, "BG_CYCLE");
		cJSON* app_psname_obj = cJSON_GetObjectItem(app_obj, "PS_NAME");
		cJSON* app_loading_file_obj = cJSON_GetObjectItem(app_obj, "LOADING_FILE");
		cJSON* app_cache_obj = cJSON_GetObjectItem(app_obj, "CACHE");

		if (app_name_obj == NULL || app_path_obj == NULL || app_psname_obj == NULL) 
			continue;
		memset(apps[i].name, 0, MAX_NAME);
		sprintf(apps[i].name, "%s", app_name_obj->valuestring);
		memset(apps[i].path, 0, PATH_MAX + 1);
		sprintf(apps[i].path, "%s", app_path_obj->valuestring);
		if (app_update_obj == NULL)
			apps[i].update_cycle = default_update;
		else
			apps[i].update_cycle = app_update_obj->valuedouble;

		if (app_loading_obj == NULL)
			apps[i].loading_cycle = default_loading;
		else
			apps[i].loading_cycle = app_loading_obj->valuedouble;

		if (app_bg_obj == NULL)
			apps[i].bg_cycle = default_bg;
		else
			apps[i].bg_cycle = app_bg_obj->valuedouble;

		if (app_loading_file_obj == NULL)
			apps[i].loading_file = 0;
		else
			apps[i].loading_file = app_loading_file_obj->valueint;

		memset(apps[i].ps_name, 0, MAX_NAME);
		sprintf(apps[i].ps_name, "%s", app_psname_obj->valuestring);

		if (app_cache_obj != NULL)
		{
			int cache_num = cJSON_GetArraySize(app_cache_obj);
			int j;
			for (j = 0; j < cache_num; j++)
			{
				struct CacheInfo cacheInfo;
				cJSON* cache_info_obj = cJSON_GetArrayItem(app_cache_obj, j);
				cJSON* cache_path_obj = cJSON_GetObjectItem(cache_info_obj, "PATH");
				cJSON* cache_maxcache_obj = cJSON_GetObjectItem(cache_info_obj, "MAX_CACHE");
				cJSON* cache_evictratio_obj = cJSON_GetObjectItem(cache_info_obj, "EVICT_RATIO");
				cJSON* cache_unique_obj = cJSON_GetObjectItem(cache_info_obj, "UNIQUE");
				cJSON* cache_onetimes_obj = cJSON_GetObjectItem(cache_info_obj, "ONETIMES");
				cJSON* cache_zipfslope_obj = cJSON_GetObjectItem(cache_info_obj, "ZIPF_S");
				cJSON* cache_maxref_obj = cJSON_GetObjectItem(cache_info_obj, "MAX_REF");

				if ((cache_path_obj == NULL) || (cache_maxcache_obj == NULL) || (cache_evictratio_obj == NULL) 
					|| (cache_unique_obj == NULL) || (cache_onetimes_obj == NULL) || (cache_maxref_obj == NULL))
				{
					cout << "Error: check cache: " << apps[i].path << " " << apps[i].name << endl;
					continue;
				}

				cacheInfo.path = cache_path_obj->valuestring;
				cacheInfo.max_cache_size = cache_maxcache_obj->valuedouble * 1024 / 4;
				cacheInfo.evict_ratio = cache_evictratio_obj->valuedouble;
				cacheInfo.unique_ratio = cache_unique_obj->valuedouble;
				cacheInfo.onetimes_ratio = cache_onetimes_obj->valuedouble;
				cacheInfo.zipf_slope = cache_zipfslope_obj->valuedouble;
				cacheInfo.max_ref = cache_maxref_obj->valuedouble;
				apps[i].cache_info.push_back(cacheInfo);

				//cout << cacheInfo.path << " ";
				//cout << cacheInfo.max_cache_size << " ";
				//cout << cacheInfo.evict_ratio << " ";
				//cout << cacheInfo.unique_ratio << " ";
				//cout << cacheInfo.onetimes_ratio << " ";
				//cout << cacheInfo.zipf_slope << " ";
				//cout << cacheInfo.max_ref << " ";
				//cout << endl;
			}
		}
	}

	return 0;
}

static int parse_ps_name(cJSON *ps_name_obj, struct Config *config)
{
	cJSON *install_obj;
	cJSON *update_obj;
	cJSON *loading_obj;
	cJSON *uninstall_obj;

	install_obj = cJSON_GetObjectItem(ps_name_obj, "INSTALL");
	if (install_obj != NULL) {
		int array_size = cJSON_GetArraySize(install_obj);
		for (int i = 0; i < array_size; i++)
		{
			cJSON* app_obj = cJSON_GetArrayItem(install_obj, i);
			string ps_name(app_obj->valuestring);
			config->ps_name.ps_install.push_back(ps_name);
		}
		config->ps_name.ps_install.push_back(string(""));
	}

	update_obj = cJSON_GetObjectItem(ps_name_obj, "UPDATE");
	if (update_obj != NULL) {
		int array_size = cJSON_GetArraySize(update_obj);
		for (int i = 0; i < array_size; i++)
		{
			cJSON* app_obj = cJSON_GetArrayItem(update_obj, i);
			string ps_name(app_obj->valuestring);
			config->ps_name.ps_update.push_back(ps_name);
		}
		config->ps_name.ps_update.push_back(string(""));
	}

	loading_obj = cJSON_GetObjectItem(ps_name_obj, "LOADING");
	if (loading_obj != NULL) {
		int array_size = cJSON_GetArraySize(loading_obj);
		for (int i = 0; i < array_size; i++)
		{
			cJSON* app_obj = cJSON_GetArrayItem(loading_obj, i);
			string ps_name(app_obj->valuestring);
			config->ps_name.ps_loading.push_back(ps_name);
		}
		config->ps_name.ps_loading.push_back(string(""));
	}

	uninstall_obj = cJSON_GetObjectItem(ps_name_obj, "UNINSTALL");
	if (uninstall_obj != NULL) {
		int array_size = cJSON_GetArraySize(uninstall_obj);
		for (int i = 0; i < array_size; i++)
		{
			cJSON* app_obj = cJSON_GetArrayItem(uninstall_obj, i);
			string ps_name(app_obj->valuestring);
			config->ps_name.ps_uninstall.push_back(ps_name);
		}
		config->ps_name.ps_uninstall.push_back(string(""));
	}
	return 0;
}

static int parse_fulldisk(cJSON *ps_full_obj, struct Config *config)
{
	cJSON *limit_obj;
	cJSON *uninstall_obj;
	cJSON *camera_delete_obj;
	cJSON *mul_delete_obj;

	limit_obj = cJSON_GetObjectItem(ps_full_obj, "LIMIT");
	if (limit_obj != NULL) {
		config->fulldisk.limit = limit_obj->valuedouble;
	}
	else
		return -1;

	uninstall_obj = cJSON_GetObjectItem(ps_full_obj, "UNINSTALL_APP");
	if (uninstall_obj != NULL) {
		config->fulldisk.uninstall_app = uninstall_obj->valueint;
	}
	else
		return -1;

	camera_delete_obj = cJSON_GetObjectItem(ps_full_obj, "CAMERA_DELETE");
	if (camera_delete_obj != NULL) {
		config->fulldisk.camera_delete = camera_delete_obj->valueint;
	}
	else
		return -1;

	mul_delete_obj = cJSON_GetObjectItem(ps_full_obj, "MUL_DELETE");
	if (camera_delete_obj != NULL) {
		int array_size = cJSON_GetArraySize(mul_delete_obj);
		for (int i = 0; i < array_size; i++)
		{
			struct MulDelete mul_delete;
			cJSON* mul_obj = cJSON_GetArrayItem(mul_delete_obj, i);
			cJSON* name_obj = cJSON_GetObjectItem(mul_obj, "NAME");
			if (name_obj == NULL)
				continue;
			mul_delete.name = string(name_obj->valuestring);

			cJSON* delete_obj = cJSON_GetObjectItem(mul_obj, "DELETE");
			if (delete_obj == NULL)
				continue;
			mul_delete.mul_delete = delete_obj->valueint;

			int found = 0;
			for (found = 0; found < config->multi.mul_others.size(); found++)
			{
				if (config->multi.mul_others[found].name.compare(mul_delete.name) == 0)
				{
					mul_delete.path = config->multi.mul_others[found].multimedia_path;
					break;
				}				
			}
			if (found == config->multi.mul_others.size())
				continue;
			config->fulldisk.mul_delete.push_back(mul_delete);
		}
	}
	return 0;
}


struct Traceinfo
{
	FILE *fp;
	string filename;
	double time;
	string line;
};

#define SPRINTF_TRACE_PATH_INPUT(BUF, TYPE, PATH, NAME, PS_NAME) \
    sprintf(BUF, "%s/TRACE_%s_%s_%s.input", PATH, NAME, TYPE, PS_NAME);
#define SPRINTF_TRACE_PATH_OUTPUT(BUF, TYPE, PATH, NAME) \
    sprintf(BUF, "%s/TRACE_%s_%s.input", PATH, NAME, TYPE);
#define SPRINTF_TRACE_PATH_INPUT_NUM(BUF, TYPE, PATH, NAME, PS_NAME, NUM) \
    sprintf(BUF, "%s/TRACE_%s%d_%s_%s.input", PATH, NAME, NUM, TYPE, PS_NAME);
#define SPRINTF_TRACE_PATH_OUTPUT_NUM(BUF, TYPE, PATH, NAME, NUM) \
    sprintf(BUF, "%s/TRACE_%s%d_%s.input", PATH, NAME, NUM, TYPE);

static int do_trace_merge(struct App *app, struct PSName *ps_name, string type, int num)
{
	vector<struct Traceinfo*> vector_trace;
	vector<string> *PSName; 
	char line[2048];
	FILE *input_fp, *output_fp;
	struct Traceinfo* trace;
	char buf[PATH_MAX];
	int ret = 0;
	int init = 1;

	memset(buf, 0, PATH_MAX);
	if (num == 0) {
		SPRINTF_TRACE_PATH_OUTPUT(buf, type.c_str(), app->path, app->name);
	}
	else {
		SPRINTF_TRACE_PATH_OUTPUT_NUM(buf, type.c_str(), app->path, app->name, num);
	}

	output_fp = fopen(buf, "r");
	if (output_fp != NULL) {
		fclose(output_fp);
		return 0;
	}

	if (type.compare("install") == 0)
		PSName = &(ps_name->ps_install);
	else if (type.compare("loading") == 0)
		PSName = &(ps_name->ps_loading);
	else if (type.compare("update") == 0)
		PSName = &(ps_name->ps_update);
	else if (type.compare("uninstall") == 0)
		PSName = &(ps_name->ps_uninstall);
	else
		return -1;

	for (vector<string>::iterator it = PSName->begin(); it != PSName->end(); ++it)
	{
		trace = new Traceinfo;
		if (trace == NULL)
			return -1;
		memset(buf, 0, PATH_MAX);
		if (num == 0) {
			SPRINTF_TRACE_PATH_INPUT(buf, type.c_str(), app->path, app->name, (*it).c_str());
		}
		else {
			SPRINTF_TRACE_PATH_INPUT_NUM(buf, type.c_str(), app->path, app->name, (*it).c_str(), num);
		}
		input_fp = fopen(buf, "r");
		if (input_fp == NULL) {
			delete trace;
			continue;
		}
		memset(line, 0, 2048);
		if (fgets(line, 2048, input_fp) == NULL)
		{
			delete trace;
			continue;
		}
		else {
			trace->fp = input_fp;
			trace->line = string(line);
			trace->time = parse_time(string(line));
			trace->filename = string(buf);
			vector_trace.push_back(trace);
		}
	}

	trace = new Traceinfo;
	if (trace == NULL)
		return -1;
	memset(buf, 0, PATH_MAX);
	if (num == 0) {
		SPRINTF_TRACE_PATH_INPUT(buf, type.c_str(), app->path, app->name, app->ps_name);
	}
	else {
		SPRINTF_TRACE_PATH_INPUT_NUM(buf, type.c_str(), app->path, app->name, app->ps_name, num);
	}

	input_fp = fopen(buf, "r");
	if (input_fp == NULL) 
		delete trace;
	else {
		memset(line, 0, 2048);
		if (fgets(line, 2048, input_fp) == NULL) {
			cout << "Remove " << string(buf) << endl;
			unlink(buf);
			delete trace;
		}
		else 
		{
			trace->fp = input_fp;
			trace->line = string(line);
			trace->time = parse_time(string(line));
			trace->filename = string(buf);
			vector_trace.push_back(trace);
		}
	}

	memset(buf, 0, PATH_MAX);
	if (num == 0) {
		SPRINTF_TRACE_PATH_OUTPUT(buf, type.c_str(), app->path, app->name);
	}
	else {
		SPRINTF_TRACE_PATH_OUTPUT_NUM(buf, type.c_str(), app->path, app->name, num);
	}

	output_fp = fopen(buf, "w");
	if (output_fp == NULL) {
		ret = -1;
		goto out;
	}

	while (!vector_trace.empty())
	{
		double min_time = DBL_MAX;
		char new_line[2048];
		vector<struct Traceinfo*>::iterator min_it = vector_trace.begin();
		vector<struct Traceinfo*>::iterator it;
		for (it = vector_trace.begin(); it != vector_trace.end(); ++it)
		{
			if (min_time > (*it)->time) {
				min_time = (*it)->time;
				min_it = it;
			}
		}
		
		fprintf(output_fp, "%s", (*min_it)->line.c_str());

		if (fgets(new_line, 2048, (*min_it)->fp) == NULL) {
			struct Traceinfo *delete_trace;
			delete_trace = (*min_it);
			fclose(delete_trace->fp);
			cout << "Remove " << delete_trace->filename << endl;
			unlink(delete_trace->filename.c_str());
			vector_trace.erase(min_it);
			delete delete_trace;
		}
		else {
			(*min_it)->line = string(new_line);
			(*min_it)->time = parse_time(string(new_line));
		}
	}

out:
	while (!vector_trace.empty())
	{
		trace = vector_trace.back();
		vector_trace.pop_back();
		fclose(trace->fp);
		delete trace;
	}
	if (output_fp != NULL)
		fclose(output_fp);

	return ret; 
}

static double parse_time(string line)
{
	string substring;
	size_t found;
	double time;

	found = line.find("\t");
	if (found == string::npos)
		return -1;
	substring = line.substr(0, found);

	time = atof(substring.c_str());

	return time;
}


int trace_merge(struct Config *config)
{
	int i = 0, k = 0;
	int ret = 0;
	char buf[PATH_MAX+8];
/*	
	if (strstr(config->backup_path, "NULL") != NULL) {
		memset(buf, 0, PATH_MAX + 8);
		sprintf("rm -r %s", config->backup_path);
		system(buf);
		memset(buf, 0, PATH_MAX + 8);
		sprintf("mkdir %s", config->backup_path);
		system(buf);
	}
*/
	for (i = 0; i < config->basic_app.app_count; i++)
	{
		int loading_file = config->basic_app.apps[i].loading_file;
		ret = do_trace_merge(&(config->basic_app.apps[i]), &(config->ps_name), string("install"), 0);
		if (loading_file == 0)
			ret = do_trace_merge(&(config->basic_app.apps[i]), &(config->ps_name), string("loading"), 0);
		else {
			for (k = 1; k <= loading_file; k++)
				ret = do_trace_merge(&(config->basic_app.apps[i]), &(config->ps_name), string("loading"), k);
		}
		ret = do_trace_merge(&(config->basic_app.apps[i]), &(config->ps_name), string("update"), 0);
	}

	for (i = 0; i < config->normal_app.app_count; i++)
	{
		int loading_file = config->normal_app.apps[i].loading_file;
		if (loading_file == 0)
			ret = do_trace_merge(&(config->normal_app.apps[i]), &(config->ps_name), string("loading"), 0);
		else {
			for (k = 1; k <= loading_file; k++)
				ret = do_trace_merge(&(config->normal_app.apps[i]), &(config->ps_name), string("loading"), k);
		}
		ret = do_trace_merge(&(config->normal_app.apps[i]), &(config->ps_name), string("update"), 0);
		ret = do_trace_merge(&(config->normal_app.apps[i]), &(config->ps_name), string("install"), 0);
		ret = do_trace_merge(&(config->normal_app.apps[i]), &(config->ps_name), string("uninstall"), 0);
	}

	return ret;
}


#define SPRINTF_TRACE_PATH_PREFIX(BUF, TYPE, PATH, NAME) \
    sprintf(BUF, "%s/TRACE_%s_%s_", PATH, NAME, TYPE);
static int do_set_background_map(struct Config *config, char *file, struct App *cur_app)
{
	string type;
	vector<string> *PSName; 
	struct PSName *ps_name = &(config->ps_name);
	int i, found = 0;

	if (strstr(file, ".input") == NULL)
		return 0;
	if (strstr(file, cur_app->name) == NULL)
		return 0;
	if (strstr(file, cur_app->ps_name) != NULL)
		return 0;

	for (i = 0; i < config->basic_app.app_count; i++)
	{
		if (strstr(file, config->basic_app.apps[i].ps_name) != NULL)
		{
			config->basic_app.apps[i].bg_file.push_back(string(file));
			found = 1;
			break;
		}
	}

	if (found == 0)
	{
		for (i = 0; i < config->normal_app.app_count; i++)
		{
			config->normal_app.apps[i].bg_file.push_back(string(file));
			found = 1;
			break;
		}
	}

	return 0;
}

int set_background_map(struct Config *config)
{
	int i = 0;
	DIR *dir_app_info;
	struct dirent *dir_app_entry;

	for (i = 0; i < config->basic_app.app_count; i++)
	{
		dir_app_info = opendir(config->basic_app.apps[i].path);
		if (dir_app_info == NULL)
			continue;
		while (dir_app_entry = readdir(dir_app_info))
		{
			char filename[PATH_MAX];
			if (dir_app_entry->d_type != DT_REG)
				continue;
			memset(filename, 0, PATH_MAX);
			snprintf(filename, PATH_MAX, "%s/%s", config->basic_app.apps[i].path, dir_app_entry->d_name);
			do_set_background_map(config, filename, &(config->basic_app.apps[i]));
		}
		closedir(dir_app_info);
	}

	for (i = 0; i < config->normal_app.app_count; i++)
	{
		dir_app_info = opendir(config->normal_app.apps[i].path);
		if (dir_app_info == NULL)
			continue;
		while (dir_app_entry = readdir(dir_app_info))
		{
			char filename[PATH_MAX];
			if (dir_app_entry->d_type != DT_REG)
				continue;
			memset(filename, 0, PATH_MAX);
			snprintf(filename, PATH_MAX, "%s/%s", config->normal_app.apps[i].path, dir_app_entry->d_name);
			do_set_background_map(config, filename, &(config->normal_app.apps[i]));
		}
		closedir(dir_app_info);
	}
}





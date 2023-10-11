#include <limits.h>
#include <vector>
#include <iostream>
#include <string>
#include <map>

using namespace std;

#define MAX_NAME	200
#define DEFAULT_DAY	50

struct MulConf
{
	string name;
	string multimedia_path;
	int take_count;
	int delete_count;
	int init_count;
	unsigned long long min_size;
	unsigned long long max_size;
};

struct CacheInfo
{
	string path;
	double max_cache_size;
	double evict_ratio;
	double unique_ratio;
	double onetimes_ratio;
	double zipf_slope;
	int max_ref;
};

struct App
{
	char name[MAX_NAME];
	char path[PATH_MAX+1];
	char ps_name[MAX_NAME];
	double update_cycle;
	double loading_cycle;
	double bg_cycle;
	int loading_file;
	vector<string> bg_file;
	vector<CacheInfo> cache_info;
};

struct Multimedia 
{
	struct MulConf mul_camera;
	vector<struct MulConf> mul_others;
};

struct BasicApp
{
	double default_update_cycle;
	double default_loading_cycle;
	double default_bg_cycle;
	struct App* apps;
	int app_count;
};

struct NormalApp
{
	double default_update_cycle;
	double default_loading_cycle;
	double default_bg_cycle;
	double install_cycle;
	double uninstall_cycle;
	struct App* apps;
	int app_count;
};

struct PSName
{
	vector<string> ps_install;
	vector<string> ps_update;
	vector<string> ps_loading;
	vector<string> ps_uninstall;
};

struct MulDelete
{
	string name;
	string path;
	int mul_delete;
};

struct FullDisk
{
	double limit;
	int uninstall_app;
	int camera_delete;
	vector<struct MulDelete> mul_delete;	
};

struct Config
{
	char mount_dir[PATH_MAX + 1];
	char INIT_FILEMAP[PATH_MAX + 1];
	char backup_path[PATH_MAX + 1];
	struct Multimedia multi;
	struct BasicApp basic_app;
	struct NormalApp normal_app;
	struct PSName ps_name;
	struct FullDisk fulldisk;
};

int parse_config(char *config_name, struct Config *config);
int trace_merge(struct Config *config);
int set_background_map(struct Config *config);
int free_background_map(struct Config *config);


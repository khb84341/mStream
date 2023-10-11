#include "traceConfig.h"
#include "dbReplay.h"
#include <map>
#include <list>
using namespace std; 

#define SPRINTF_LOADING_PATH(BUF, PATH, NAME) \
	sprintf(BUF, "%s/TRACE_%s_loading.input", PATH, NAME);
#define SPRINTF_LOADING_PATH_NUM(BUF, PATH, NAME, NUM) \
	sprintf(BUF, "%s/TRACE_%s%d_loading.input", PATH, NAME, NUM);
#define SPRINTF_UPDATE_PATH(BUF, PATH, NAME) \
	sprintf(BUF, "%s/TRACE_%s_update.input", PATH, NAME);
#define SPRINTF_INSTALL_PATH(BUF, PATH, NAME) \
	sprintf(BUF, "%s/TRACE_%s_install.input", PATH, NAME);
#define SPRINTF_UNINSTALL_PATH(BUF, PATH, NAME) \
	sprintf(BUF, "%s/TRACE_%s_uninstall.input", PATH, NAME);


#define TRACE_MERGE		0x01
#define INITFILE	0x02
#define PREV_OUT	0x04
#define VERBOSE	0x08
#define SYSFS	0x10
#define FSYNCTIME	0x20

enum REPLAY_TYPE
{
	REPLAY_LOADING = 0,
	REPLAY_UPDATE,
	REPLAY_INSTALL,
	REPLAY_UNINSTALL,
	REPLAY_CAMERA,
	REPLAY_CAMERA_DELETE,
	REPLAY_MULTI,
	REPLAY_MULTI_DELETE,
	REPLAY_BG
};

struct CacheFileInfo
{
	int fileID;
	long long int filesize;
	int ref;
};

struct CacheRef
{
	list<CacheFileInfo*> filelist;
	int total_ref;
};

struct CacheSize
{
	string path;
	long long int filesize;
};

struct CacheDirInfo
{
	string path;
	long long max_cache_size;
	long long cur_cache_size;
	double evict_ratio;
	double unique_ratio;
	double onetimes_ratio;
	double zipf_slope;
	int max_ref;

	int idcount;
	int c;
	map<string, string> file_map;		// trace_path, new_path
	map<string, string> file_rmap;		// new_path, trace_path
	map<string, unsigned long long> file_smap;	// new_path, file_size;
	list<string> lru_list;
	struct CacheRef cacheref;

	vector<long long int> sizevec;
	list<CacheSize> recent_cache;
};

struct CacheManager
{
	vector<struct CacheDirInfo> cache_list;
};

struct DBManager
{
	map<string, struct DBInfo*> db_map;
};

struct ReplayJob
{
	enum REPLAY_TYPE type;
	char name[MAX_NAME];
	char path[PATH_MAX + 1];
	double curTime;
	double cycle;
	int curLoading;
	int maxLoading;
	vector<string> bgjob;
	struct CacheManager cache;
	struct DBManager db;
	unsigned long long mul_info[2];
	
	struct ReplayJob* loadJob;
};

struct ReplayFile
{
	string mount_dir;
	string input_name;
	struct ReplayJob *job;
	double curTime;
};

int one_replay(char type, char *prefix, char *path);

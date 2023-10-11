#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <mntent.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <fstab.h>
#include <iostream>
#include <list>
#include <string>
#include <sys/wait.h>
#include <linux/ioctl.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <math.h>
#include "simpleReplay.h"

using namespace std;

#define DAYTIME	(24*60*60)

struct Config *config;
string prev_out_name;
double IG_curTime;
string IG_outPath;
int IG_mode;
int mode_flag;

static int trace_replay(char *config_name, int day);
int print_help(void);
int trace_init(void);
int do_trace_replay(double day);
int replay_basic_install(void);
int replay_init_multimedia(void);
struct ReplayJob* create_replayjob(enum REPLAY_TYPE type, const char *name, const char *path, double cycle);
struct ReplayJob* create_replayjob(enum REPLAY_TYPE type, struct ReplayJob *replay);
int init_replayjob(list<struct ReplayJob*> *ReplayJob_queue, list<struct App*> *Normal_list);
int load_replayjob(list <struct ReplayJob*> *ReplayJob_queue, list<struct App*> *ins_list, list<struct App*> *unins_list, double* day);
int store_replayjob(list <struct ReplayJob*> *ReplayJob_queue, list<struct App*> *ins_list, double day);
int insert_replayqueue(list<struct ReplayJob*> *ReplayJob_queue, struct ReplayJob* job);
struct ReplayJob* create_bgjob(struct App* app);
int uninstall_replayqueue(list<struct ReplayJob*> *ReplayJob_queue, const char *name);

int replay_loading(struct ReplayJob* replay);
int replay_loading(struct ReplayJob* replay, list<struct ReplayFile> *update_list, list<struct ReplayFile> *bg_list);
int replay_update(struct ReplayJob* replay);
int replay_update(struct ReplayJob* replay, list<struct ReplayFile> *update_list);
int replay_install(list<struct ReplayJob*> *jobqueue, list<struct App*> *ins_list, list<struct App*> *unins_list, double curTime);
int replay_uninstall(list<struct ReplayJob*> *jobqueue, list<struct App*> *ins_list, list<struct App*> *unins_list, double curTime);
int replay_camera(struct ReplayJob* replay);
int replay_camera_delete(struct ReplayJob* replay);
int replay_bg(struct ReplayJob* replay);
int replay_bg(struct ReplayJob* replay, list<struct ReplayFile> *update_list);

int app_install(list<struct ReplayJob*> *jobqueue, list<struct App*> *ins_list, list<struct App*> *unins_list, 
	list<struct App*>::iterator it, double load, double update, double bg, int curload);

static double get_utilization(void);
static int do_fulldisk(list <struct ReplayJob*> *queue, list<struct App*> *ins_list, 
						list<struct App*> *unins_list, double curTime);
static void init_db_manager(struct ReplayJob *replay_loading, char *mount_dir, string app_name, string app_path, string app_ps, int total_file);
double randn (double mu, double sigma);

struct replay_stat
{
	int install;
	int update;
	int loading;
	int uninstall;
	int camera_create;
	int camera_delete;
};

struct replay_stat replay_stat;

int print_help(void)
{
	printf("traceReplay 1.1(190421) Version\n");
	printf("190421: READ replay\n");
	printf("Usage: ./traceReplay [OPTION] JSONFile\n");
	printf("[OPTION]\n");
	printf("-i: initilization (INIT_FILE in JSONFile)\n");
	printf("-d: end day\n");
	printf("-p: prev output (replay_n.out)\n");
	printf("-l: print sysfs\n");
	printf("-G [OUTPUT PATH]: Input Generator mode\n");
	return 0;
}

int main (int argc, char *argv[])
{
	FILE* init_fp;
	int opt, i;
	int day = DEFAULT_DAY;
	FILE *fOut;
	mode_flag = 0;
	IG_mode = 0;
	IG_curTime = 0.0;

	while ((opt = getopt(argc, argv, "hd:Mip:vlG:f")) != EOF) {
		switch (opt) {
		case 'h':
			print_help();
			goto out;
		case 'd': 
			day = atoi(optarg);
			break;
		case 'i':
			mode_flag |= INITFILE;
			break;
		case 'p':
			mode_flag |= PREV_OUT;
			prev_out_name = string(optarg);
			break;
		case 'v':
			mode_flag |= VERBOSE;
			break;
		case 'l':
			mode_flag |= SYSFS;
			printf("print STAT\n");
			break;
		case 'f':
			mode_flag |= FSYNCTIME;
			break;
		case 'G':
			IG_mode = 1;
			IG_outPath = string(optarg);
			fOut = fopen(IG_outPath.c_str(), "w");
			cout << "MODE IG" << endl;
			if (fOut == NULL) {
				cout << "Failed: IG output: " << IG_outPath << endl;
				goto out;
			}
			fclose(fOut);
			break;
		default:
			print_help();
			goto out;
		}
	}

	if ((argc-optind) != 1) {
		print_help();
		goto out;
	}

	trace_replay(argv[optind], day);

out:
	return 0;
}

static int trace_replay(char *config_name, int day)
{
	int app_count;
	config = new struct Config;

	if (parse_config(config_name, config) < 0)
		goto out;

	if (mode_flag & INITFILE) {
		trace_merge(config);	
	}

	set_background_map(config);

	if (mode_flag & INITFILE) {
		trace_init();
	}
	do_trace_replay((double)day);
	
	app_count = config->basic_app.app_count;
	if (app_count > 0) {
		delete[] config->basic_app.apps;
	}
	app_count = config->normal_app.app_count;
	if (app_count > 0) {
		delete[] config->normal_app.apps;
	}

out:
	delete config;

	return 0;
}

int trace_init(void)
{
	char cmd[PATH_MAX];
	if (strcmp(config->INIT_FILEMAP, "NULL") == 0)
		return -1;

//	execlp("./simpleReplay", "./simpleReplay", "-i", config->INIT_FILEMAP, NULL);
	memset(cmd, 0, PATH_MAX);
	sprintf(cmd, "./simpleReplay -p %s -i %s", config->mount_dir, config->INIT_FILEMAP);
	create_init_files(config->mount_dir, config->INIT_FILEMAP);
	// system(cmd);
	if (mode_flag & VERBOSE)
		printf("[%3.2lf] %s\n", (double)0.0, cmd);
	return 0;
}


//#define DYNAMIC_TEST
int do_trace_replay(double day)
{
	list<struct ReplayJob*> ReplayJob_queue;
	list<struct App*> Install_list;
	list<struct App*> Uninstall_list;
	list<struct ReplayJob*>::iterator iter;
	list<struct ReplayFile> Update_list;
	list<struct ReplayFile> BG_list;

	double curTime = 0;
	double util = 0;
	double logprint = 0;
	double testchange = 10;

#ifdef DYNAMIC_TEST
	int test_type = 1;
#endif

	memset(&replay_stat, 0, sizeof(struct replay_stat));

	if (init_replayjob(&ReplayJob_queue, &Uninstall_list) < 0)
		return -1;
	if (mode_flag & PREV_OUT) {
		load_replayjob(&ReplayJob_queue, &Install_list, &Uninstall_list, &curTime);
		cout << "Start "<< curTime << " DAY" << endl;
	} else {
		replay_init_multimedia();
		replay_basic_install();
	}

	if (ReplayJob_queue.empty())
		return -1;

	while (1)
	{
		struct ReplayJob* replay = ReplayJob_queue.front();

		ReplayJob_queue.pop_front();

		if (mode_flag & SYSFS) {
			if ((logprint < replay->curTime) || (replay->curTime > day)) {
				pid_t pid;
				char arg[10];
				int status = 0;
				int logprint_int = int(logprint);

//				if (replay->curTime > day)
//					logprint = replay->curTime;

				pid = fork();
				if (pid == 0) {
					sprintf(arg, "%d", logprint_int);
					printf("%s %s %s\n", "/bin/sh", "print_result.sh", arg);
					execl("/bin/sh", "/bin/sh", "print_result.sh", arg, NULL);
					exit(1);
				}
				else
					wait(&status);

				logprint = logprint + 2;
			}
		}

		if ((testchange  < replay->curTime)) {
	#ifdef DYNAMIC_TEST
				if (test_type == 0)
					test_type = 1;
				else 
					test_type = 0;
	#endif
			testchange = testchange + 10; 
		}

#ifdef DYNAMIC_TEST
		if (replay->type == REPLAY_LOADING) {
			if (test_type == 1) {
				if (strstr(replay->name, "cnn"))
					replay->cycle = 0.72;
				else if (strstr(replay->name, "youtube"))
					replay->cycle = 0.72;
				else if (strstr(replay->name, "twitter"))
					replay->cycle = 7;
				else if (strstr(replay->name, "clash"))
					replay->cycle = 0.49;
				else if (strstr(replay->name, "aliexpress"))
					replay->cycle = 3.33;
				else if (strstr(replay->name, "gmail"))
					replay->cycle = 7;
			}
			else {
				if (strstr(replay->name, "cnn"))
					replay->cycle = 4;
				else if (strstr(replay->name, "youtube"))
					replay->cycle = 4.5;
				else if (strstr(replay->name, "twitter"))
					replay->cycle = 1;
				else if (strstr(replay->name, "clash"))
					replay->cycle = 4.3;
				else if (strstr(replay->name, "aliexpress"))
					replay->cycle = 0.4;
				else if (strstr(replay->name, "gmail"))
					replay->cycle = 0.6;
			}
		}
#endif



#if 0
		if (replay->curTime > 3) {
			if (strchr(replay->name, "youtube")
				replay->cycle = 3;
		}
#endif

		if (replay->curTime > day) {
			insert_replayqueue(&ReplayJob_queue, replay);
			replay = ReplayJob_queue.front();
			break;
		}

		if (IG_mode == 1) {
			if (replay->curTime - curTime > 0.0) {
				double itime = replay->curTime - curTime;
				IG_curTime = IG_curTime + (DAYTIME * itime);
			}
		} else {
			if (replay->curTime - curTime > 0.0) {
				string ioctl_path = string(config->mount_dir);
				int fd = open(ioctl_path.c_str(), O_RDONLY | O_DIRECTORY);
				double dtime = replay->curTime * DAYTIME;
				unsigned long long simul_time = (unsigned long long) dtime;
#define F2FS_IOC_SIMUL_TIME	_IOW(0xf5, 12, __u64)
				int ret = ioctl(fd, F2FS_IOC_SIMUL_TIME, &simul_time);
//				if (ret < 0)
//					cout << "Error: Failed simul system call\n" << endl;
				close(fd);
			}
		}
		switch (replay->type)
		{
			case REPLAY_LOADING:
//				replay_loading(replay);
				replay_loading(replay, &Update_list, &BG_list);
			break;
			case REPLAY_UPDATE:
//				replay_update(replay);
				replay_update(replay, &Update_list);
			break;
			case REPLAY_INSTALL:
				replay_install(&ReplayJob_queue, &Install_list, &Uninstall_list, replay->curTime);
			break;
			case REPLAY_UNINSTALL:
				replay_uninstall(&ReplayJob_queue, &Install_list, &Uninstall_list, replay->curTime);
			break;
			case REPLAY_BG:
//				replay_bg(replay);
				replay_bg(replay, &BG_list);
			break;
			case REPLAY_CAMERA:
				replay_camera(replay);
			break;
			case REPLAY_CAMERA_DELETE:
				replay_camera_delete(replay);
			break;
			case REPLAY_MULTI:
				replay_camera(replay);
			break;
			case REPLAY_MULTI_DELETE:
				replay_camera_delete(replay);
			break;
		}
		curTime = replay->curTime;
		if (replay->type == REPLAY_UPDATE)
			replay->curTime = replay->curTime + randn(replay->cycle, 7);
		else
			replay->curTime = replay->curTime + replay->cycle;
		insert_replayqueue(&ReplayJob_queue, replay);

		int try_count = 1;
		while (1) {
			util = get_utilization();
			if (mode_flag & VERBOSE)
				printf("[Util] %lf\n", util);
			if (try_count % 10 == 0) {
//				printf("Try Uninstall & Install\n");
//				replay_uninstall(&ReplayJob_queue, &Install_list, &Uninstall_list, curTime);
//				replay_install(&ReplayJob_queue, &Install_list, &Uninstall_list, curTime);
			}
			if (try_count >= 31) {
				printf("Try Uninstall\n");
				replay_uninstall(&ReplayJob_queue, &Install_list, &Uninstall_list, curTime);
				try_count = 0;
			}
			if (util >= config->fulldisk.limit) {
				do_fulldisk (&ReplayJob_queue, &Install_list, &Uninstall_list, curTime);
				util = get_utilization();
				try_count++;
			} else
				break;
		}
	}
	store_replayjob(&ReplayJob_queue, &Install_list, day);

	while (!ReplayJob_queue.empty())
	{
		struct ReplayJob* job = ReplayJob_queue.front();
		ReplayJob_queue.pop_front();
		if (job != NULL) {
			delete job;
		}
	}

	return 0;
}

int replay_loading(struct ReplayJob* replay)
{
	char buf[PATH_MAX];
	char cmd[PATH_MAX];
	memset(buf, 0, PATH_MAX);
	if (replay->maxLoading == 0) {
		SPRINTF_LOADING_PATH(buf, replay->path, replay->name);
	}
	else {
		SPRINTF_LOADING_PATH_NUM(buf, replay->path, replay->name, replay->curLoading);
		replay->curLoading++;
		if (replay->curLoading > replay->maxLoading)
			replay->curLoading = 1;
	}
	memset(cmd, 0, PATH_MAX);
	sprintf(cmd, "./simpleReplay -p %s %s", config->mount_dir, buf);
	if (mode_flag & VERBOSE) 
		printf("[%3.2lf] %s\n", replay->curTime, cmd);
//	system(cmd);
	do_trace_replay(config->mount_dir, buf, replay, replay->curTime);
	replay_stat.loading++;

	return 0;
}

int replay_loading(struct ReplayJob* replay, list<struct ReplayFile> *update_list, list<struct ReplayFile> *bg_list)
{
	char buf[PATH_MAX];
	char cmd[PATH_MAX];
	struct ReplayFile loadfile;
	memset(buf, 0, PATH_MAX);
	if (replay->maxLoading == 0) {
		SPRINTF_LOADING_PATH(buf, replay->path, replay->name);
	}
	else {
		SPRINTF_LOADING_PATH_NUM(buf, replay->path, replay->name, replay->curLoading);
		replay->curLoading++;
		if (replay->curLoading > replay->maxLoading)
			replay->curLoading = 1;
	}
	memset(cmd, 0, PATH_MAX);
	sprintf(cmd, "./simpleReplay -p %s %s", config->mount_dir, buf);
	if (mode_flag & VERBOSE) 
		printf("[%3.2lf] %s\n", replay->curTime, cmd);
//	system(cmd);
//	do_trace_replay(config->mount_dir, buf, replay, replay->curTime);

	loadfile.mount_dir = string(config->mount_dir);
	loadfile.input_name = string(buf);
	loadfile.job = replay;
	loadfile.curTime = replay->curTime;

	if (!(update_list->empty()))
	{
		do_trace_replay(&loadfile, update_list);
	}
	else if (!(bg_list->empty()))
	{
		do_trace_replay(&loadfile, bg_list);
	}
	else
		do_trace_replay(config->mount_dir, buf, replay, replay->curTime);

	replay_stat.loading++;

	return 0;
}

int replay_update(struct ReplayJob* replay)
{
	char buf[PATH_MAX];
	char cmd[PATH_MAX];
	memset(buf, 0, PATH_MAX);
	SPRINTF_UPDATE_PATH(buf, replay->path, replay->name);
	memset(cmd, 0, PATH_MAX);
	sprintf(cmd, "./simpleReplay -p %s %s", config->mount_dir, buf);
	if (mode_flag & VERBOSE)	
		printf("[%3.2lf] %s\n", replay->curTime, cmd);
	// system(cmd);
	do_trace_replay(config->mount_dir, buf, replay, replay->curTime);

	replay_stat.update++;

	return 0;
}

int replay_update(struct ReplayJob* replay, list<struct ReplayFile> *update_list)
{
	char buf[PATH_MAX];
	char cmd[PATH_MAX];
	struct ReplayFile updatefile;
	memset(buf, 0, PATH_MAX);
	SPRINTF_UPDATE_PATH(buf, replay->path, replay->name);
	memset(cmd, 0, PATH_MAX);
	sprintf(cmd, "./simpleReplay -p %s %s", config->mount_dir, buf);
	if (mode_flag & VERBOSE)	
		printf("[%3.2lf] %s\n", replay->curTime, cmd);
	// system(cmd);
//	do_trace_replay(config->mount_dir, buf, replay, replay->curTime);
	updatefile.mount_dir = string(config->mount_dir);
	updatefile.input_name = string(buf);
	updatefile.job = replay;
	updatefile.curTime = replay->curTime;
	update_list->push_back(updatefile);

	replay_stat.update++;

	return 0;
}

int replay_bg(struct ReplayJob* replay)
{
	char buf[PATH_MAX];
	char cmd[PATH_MAX];
	int size = replay->bgjob.size();
	vector<string>::iterator it;
	string str_path;

	if (size == 0)
		return 0;

	int value;
	value = rand() % size;
	it = replay->bgjob.begin();
	advance(it, value);
	str_path = (*it);

	memset(buf, 0, PATH_MAX);
	sprintf(buf, "%s", str_path.c_str());
	memset(cmd, 0, PATH_MAX);
	sprintf(cmd, "./simpleReplay -p %s %s", config->mount_dir, buf);
	if (mode_flag & VERBOSE)	
		printf("[%3.2lf] %s\n", replay->curTime, cmd);
	do_trace_replay(config->mount_dir, buf, replay, replay->curTime);
	//system(cmd);

	return 0;
}

int replay_bg(struct ReplayJob* replay, list<struct ReplayFile> *update_list)
{
	char buf[PATH_MAX];
	char cmd[PATH_MAX];
	int size = replay->bgjob.size();
	vector<string>::iterator it;
	string str_path;
	struct ReplayFile bgfile;

	if (size == 0)
		return 0;

	int value;
	value = rand() % size;
	it = replay->bgjob.begin();
	advance(it, value);
	str_path = (*it);

	memset(buf, 0, PATH_MAX);
	sprintf(buf, "%s", str_path.c_str());
	memset(cmd, 0, PATH_MAX);
	sprintf(cmd, "./simpleReplay -p %s %s", config->mount_dir, buf);
	if (mode_flag & VERBOSE)	
		printf("[%3.2lf] %s\n", replay->curTime, cmd);

	bgfile.mount_dir = string(config->mount_dir);
	bgfile.input_name = string(buf);
	bgfile.job = replay;
	bgfile.curTime = replay->curTime;
	update_list->push_back(bgfile);

	return 0;
}



int replay_basic_install(void)
{
	char buf[PATH_MAX];
	char cmd[PATH_MAX];
	int i;

	for (i = 0; i < config->basic_app.app_count; i++)
	{
		memset(buf, 0, PATH_MAX);
		memset(cmd, 0, PATH_MAX);
		SPRINTF_INSTALL_PATH(buf, config->basic_app.apps[i].path, config->basic_app.apps[i].name);
		sprintf(cmd, "./simpleReplay -p %s %s", config->mount_dir, buf);
		if (mode_flag & VERBOSE)	
			printf("[0.0] %s\n", cmd);
		do_trace_replay(config->mount_dir, buf, NULL, 0);
		// system(cmd);
		replay_stat.install++;
	}

	return 0;
}


int replay_init_multimedia(void)
{
	struct ReplayJob *camera_job;
	camera_job = create_replayjob(REPLAY_CAMERA, config->multi.mul_camera.multimedia_path.c_str(), config->multi.mul_camera.name.c_str(), 0);
	camera_job->mul_info[0] = config->multi.mul_camera.min_size;
	camera_job->mul_info[1] = config->multi.mul_camera.max_size;

	for (int i = 0; i < config->multi.mul_camera.init_count; i++) {
		replay_camera(camera_job);
	}
	delete camera_job;

	for (int cnt = 0; cnt < config->multi.mul_others.size(); cnt++)
	{
        struct MulConf mul_conf = config->multi.mul_others[cnt];
		struct ReplayJob *multi_job = create_replayjob(REPLAY_MULTI, mul_conf.multimedia_path.c_str(), mul_conf.name.c_str(), 0);
		multi_job->mul_info[0] = mul_conf.min_size;
		multi_job->mul_info[1] = mul_conf.max_size;

		for (int i = 0; i < mul_conf.init_count; i++)
		{
			replay_camera(multi_job);
		}
		delete multi_job;
	}

	return 0;
}


int replay_install(list<struct ReplayJob*> *jobqueue, list<struct App*> *ins_list, list<struct App*> *unins_list, double curTime)
{
	char buf[PATH_MAX];
	char cmd[PATH_MAX];
	int size = unins_list->size();
	list<struct App*>::iterator it;
	int value;
	struct ReplayJob* replay_loading;
	struct ReplayJob* replay_update;
	struct ReplayJob* replay_bg;
	struct App* app;
	int i;

	if (size == 0)
		return 0;

	value = rand() % size;
	it = unins_list->begin();
	advance(it, value);
	app = (*it);
	unins_list->erase(it);
	ins_list->push_back(app);

	replay_loading = create_replayjob(REPLAY_LOADING, app->name, app->path, app->loading_cycle);
	if (replay_loading == NULL)
		return -1;
	replay_loading->curTime = curTime + app->loading_cycle;
	replay_loading->curLoading = 1;
	replay_loading->maxLoading = app->loading_file;
	// cache
	for (i = 0; i < app->cache_info.size(); i++)
	{
		struct CacheInfo cacheInfo = app->cache_info[i];
		struct CacheDirInfo cacheDirInfo;
		int ret;
		cacheDirInfo.path = cacheInfo.path;
		cacheDirInfo.max_cache_size = cacheInfo.max_cache_size;
		cacheDirInfo.cur_cache_size = 0;
		cacheDirInfo.evict_ratio = cacheInfo.evict_ratio;
		cacheDirInfo.unique_ratio = cacheInfo.unique_ratio;
		cacheDirInfo.onetimes_ratio = cacheInfo.onetimes_ratio;
		cacheDirInfo.zipf_slope = cacheInfo.zipf_slope;
		cacheDirInfo.max_ref = cacheInfo.max_ref;
		cacheDirInfo.cacheref.total_ref = 0;

		replay_loading->cache.cache_list.push_back(cacheDirInfo);

		ret = init_cache(config->mount_dir, "data", &(replay_loading->cache.cache_list[i]));
		ret = init_cache(config->mount_dir, "data/media/0/Android", &(replay_loading->cache.cache_list[i]));

		cout << "[Cache register]:" << cacheDirInfo.path << endl;
	}
	// db
	init_db_manager(replay_loading, config->mount_dir, string(app->name), string(app->path), string(app->ps_name), app->loading_file);

	replay_update = create_replayjob(REPLAY_UPDATE, app->name, app->path, app->update_cycle);
	if (replay_update == NULL) {
		delete replay_loading;
		return -1;
	}
	replay_update->curTime = curTime + randn(app->update_cycle, 7);
	replay_update->loadJob = replay_loading;

	if (app->update_cycle != 0)
	{
		int update_100 = app->update_cycle * 100;
		int value = rand() % update_100;
		double newCurTime = (double)value / (double)100;
		replay_update->curTime = curTime + newCurTime;
	}

	replay_bg = create_bgjob(app);
	if (replay_bg != NULL) {
		replay_bg->curTime = curTime + app->bg_cycle;
		replay_bg->loadJob = replay_loading;
	}

	insert_replayqueue(jobqueue, replay_loading);
	insert_replayqueue(jobqueue, replay_update);
	if (replay_bg != NULL)
		insert_replayqueue(jobqueue, replay_bg);

	memset(buf, 0, PATH_MAX);
	SPRINTF_INSTALL_PATH(buf, app->path, app->name);
	memset(cmd, 0, PATH_MAX);
	sprintf(cmd, "./simpleReplay -p %s %s", config->mount_dir, buf);
	if (mode_flag & VERBOSE)	
		printf("[%3.2lf] %s\n", curTime, cmd);
	do_trace_replay(config->mount_dir, buf, replay_bg, curTime);
	// system(cmd);
	replay_stat.install++;

	return 0;
}

int app_install(list<struct ReplayJob*> *jobqueue, list<struct App*> *ins_list, list<struct App*> *unins_list, 
	list<struct App*>::iterator it, double load, double update, double bg, int curload)
{
	struct ReplayJob* replay_loading;
	struct ReplayJob* replay_update;
	struct ReplayJob* replay_bg;
	struct App* app;
	int i;

	app = (*it);
	unins_list->erase(it);
	ins_list->push_back(app);

	replay_loading = create_replayjob(REPLAY_LOADING, app->name, app->path, app->loading_cycle);
	if (replay_loading == NULL)
		return -1;
	replay_loading->curTime = load;
	replay_loading->maxLoading = app->loading_file;
	replay_loading->curLoading = curload;
	for (i = 0; i < app->cache_info.size(); i++)
	{
		struct CacheInfo cacheInfo = app->cache_info[i];
		struct CacheDirInfo cacheDirInfo;
		int ret;
		cacheDirInfo.path = cacheInfo.path;
		cacheDirInfo.max_cache_size = cacheInfo.max_cache_size;
		cacheDirInfo.cur_cache_size = 0;
		cacheDirInfo.evict_ratio = cacheInfo.evict_ratio;
		cacheDirInfo.unique_ratio = cacheInfo.unique_ratio;
		cacheDirInfo.onetimes_ratio = cacheInfo.onetimes_ratio;
		cacheDirInfo.zipf_slope = cacheInfo.zipf_slope;
		cacheDirInfo.max_ref = cacheInfo.max_ref;
		cacheDirInfo.cacheref.total_ref = 0;

		replay_loading->cache.cache_list.push_back(cacheDirInfo);

		ret = init_cache(config->mount_dir, "data", &(replay_loading->cache.cache_list[i]));
		ret = init_cache(config->mount_dir, "data/media/0/Android", &(replay_loading->cache.cache_list[i]));
		cout << "[Cache register]:" << cacheDirInfo.path << endl;
	}

	init_db_manager(replay_loading, config->mount_dir, string(app->name), string(app->path), string(app->ps_name), app->loading_file);


	replay_update = create_replayjob(REPLAY_UPDATE, app->name, app->path, app->update_cycle);
	if (replay_update == NULL) {
		delete replay_loading;
		return -1;
	}
	replay_update->curTime = update;
	
	replay_bg = create_bgjob(app);
	if (replay_bg != NULL)
		replay_bg->curTime = bg;

	if (update != 0)
	{
		int update_100 = update * 100;
		int value = rand() % update_100;
		double newCurTime = (double)value / (double)100;
		replay_update->curTime = newCurTime;
	}

	insert_replayqueue(jobqueue, replay_loading);
	insert_replayqueue(jobqueue, replay_update);
	if (replay_bg != NULL)
		insert_replayqueue(jobqueue, replay_bg);

	return 0;
}

int replay_uninstall(list<struct ReplayJob*> *jobqueue, list<struct App*> *ins_list, list<struct App*> *unins_list, double curTime)
{
	char buf[PATH_MAX];
	char cmd[PATH_MAX];
	int size = ins_list->size();
	list<struct App*>::iterator it;
	int value;
	struct ReplayJob* replay_loading;
	struct ReplayJob* replay_update;
	struct App* app;

	if (size == 0)
		return 0;
	value = rand() % size;
	it = ins_list->begin();
	advance(it, value);
	app = (*it);
	ins_list->erase(it);
	unins_list->push_back(app);
	
	uninstall_replayqueue(jobqueue, app->path);
	
	memset(buf, 0, PATH_MAX);
	SPRINTF_UNINSTALL_PATH(buf, app->path, app->name);
	memset(cmd, 0, PATH_MAX);
	sprintf(cmd, "./simpleReplay -p %s %s", config->mount_dir, buf);
	if (mode_flag & VERBOSE)
		printf("[%3.2lf] %s\n", curTime, cmd);
//	system(cmd);
	do_trace_replay(config->mount_dir, buf, NULL, curTime);
	replay_stat.uninstall++;

	return 0;
}

int replay_camera(struct ReplayJob* replay)
{
	char cmd[PATH_MAX];
	unsigned long long size = replay->mul_info[1] - replay->mul_info[0] + 1;
	unsigned long long value = rand() % size;
	char ext_name[PATH_MAX];
	value += replay->mul_info[0];

	memset(cmd, 0, PATH_MAX);
	sprintf(cmd, "./simpleReplay -p %s -C %s -s %llu", config->mount_dir, replay->name, value);
	if (mode_flag & VERBOSE)
		printf("[%3.2lf] %s\n", replay->curTime, cmd);
	memset(ext_name, 0, PATH_MAX);
	if (replay->type == REPLAY_CAMERA)
		sprintf(ext_name, "png");
	else if (replay->type == REPLAY_MULTI)
		sprintf(ext_name, "mp3");
	else
		sprintf(ext_name, "_");

	create_camera_file(config->mount_dir, replay->name, ext_name, value);
	//system(cmd);
	replay_stat.camera_create++;

	return 0;
}

int replay_camera_delete(struct ReplayJob* replay)
{
	char cmd[PATH_MAX];
	memset(cmd, 0, PATH_MAX);
	sprintf(cmd, "./simpleReplay -p %s -R %s", config->mount_dir, replay->name);
	if (mode_flag & VERBOSE)
		printf("[%3.2lf] %s\n", replay->curTime, cmd);
	delete_camera_file(config->mount_dir, replay->name);
	// system(cmd);
	replay_stat.camera_delete++;

	return 0;
}

int init_replayjob(list<struct ReplayJob*> *ReplayJob_queue, list<struct App*> *Normal_list)
{
	int i;
	struct ReplayJob *job;

	if (config->multi.mul_camera.take_count != 0) {
		job = create_replayjob(REPLAY_CAMERA, config->multi.mul_camera.multimedia_path.c_str(), 
						config->multi.mul_camera.name.c_str(), 1/(double)config->multi.mul_camera.take_count);
		if (job == NULL)
			return -1;
		job->mul_info[0] = config->multi.mul_camera.min_size;
		job->mul_info[1] = config->multi.mul_camera.max_size;
		insert_replayqueue(ReplayJob_queue, job);
	}

	if (config->multi.mul_camera.delete_count != 0) {
		job = create_replayjob(REPLAY_CAMERA_DELETE, config->multi.mul_camera.multimedia_path.c_str(), 
					config->multi.mul_camera.name.c_str(), 1/(double)config->multi.mul_camera.delete_count);
		if (job == NULL)
			return -1;
		insert_replayqueue(ReplayJob_queue, job);
	}

	for (i = 0; i < config->multi.mul_others.size(); i++)
	{
		struct MulConf mul_conf = config->multi.mul_others[i];
		if (mul_conf.take_count != 0) {
			job = create_replayjob(REPLAY_MULTI, mul_conf.multimedia_path.c_str(), 
					mul_conf.name.c_str(), 1/(double)mul_conf.take_count);
			if (job == NULL)
				return -1;
			job->mul_info[0] = mul_conf.min_size;
			job->mul_info[1] = mul_conf.max_size;
			insert_replayqueue(ReplayJob_queue, job);
		}
		if (mul_conf.delete_count != 0) {
			job = create_replayjob(REPLAY_MULTI_DELETE, mul_conf.multimedia_path.c_str(), 
					mul_conf.name.c_str(), 1/(double)mul_conf.delete_count);
			if (job == NULL)
				return -1;
			insert_replayqueue(ReplayJob_queue, job);
		}
	}

	job = create_replayjob(REPLAY_INSTALL, "install", "NULL", config->normal_app.install_cycle);
	if (job == NULL)
		return -1;
	insert_replayqueue(ReplayJob_queue, job);

	job = create_replayjob(REPLAY_UNINSTALL, "uninstall", "NULL", config->normal_app.uninstall_cycle);
	if (job == NULL)
		return -1;
	insert_replayqueue(ReplayJob_queue, job);

	for (i = 0; i < config->basic_app.app_count; i++)
	{
		int j;

		if (config->basic_app.apps[i].loading_cycle != 0)
		{
			job = create_replayjob(REPLAY_LOADING, config->basic_app.apps[i].name, 
						config->basic_app.apps[i].path, config->basic_app.apps[i].loading_cycle);
			job->curLoading = 1;
			job->maxLoading = config->basic_app.apps[i].loading_file;
			if (job == NULL)
				return -1;

			for (j = 0; j < config->basic_app.apps[i].cache_info.size(); j++)
			{
				struct CacheInfo cacheInfo = config->basic_app.apps[i].cache_info[j];
				struct CacheDirInfo cacheDirInfo;
				int ret;
				cacheDirInfo.path = cacheInfo.path;
				cacheDirInfo.max_cache_size = cacheInfo.max_cache_size;
				cacheDirInfo.cur_cache_size = 0;
				cacheDirInfo.evict_ratio = cacheInfo.evict_ratio;
				cacheDirInfo.unique_ratio = cacheInfo.unique_ratio;
				cacheDirInfo.onetimes_ratio = cacheInfo.onetimes_ratio;
				cacheDirInfo.zipf_slope = cacheInfo.zipf_slope;
				cacheDirInfo.max_ref = cacheInfo.max_ref;
				cacheDirInfo.cacheref.total_ref = 0;

				job->cache.cache_list.push_back(cacheDirInfo);

				ret = init_cache(config->mount_dir, "data", &(job->cache.cache_list[j]));
				ret = init_cache(config->mount_dir, "data/media/0/Android", &(job->cache.cache_list[j]));
				cout << "[Cache register]:" << cacheDirInfo.path << endl;
			}

			init_db_manager(job, config->mount_dir, string(config->basic_app.apps[i].name), 
				string(config->basic_app.apps[i].path), 
				string(config->basic_app.apps[i].ps_name), job->maxLoading);

			insert_replayqueue(ReplayJob_queue, job);
		}

		if (config->basic_app.apps[i].update_cycle != 0)
		{
			job = create_replayjob(REPLAY_UPDATE, config->basic_app.apps[i].name, 
					config->basic_app.apps[i].path, config->basic_app.apps[i].update_cycle);

			int update_100 = config->basic_app.apps[i].update_cycle * 100;
			int value = rand() % update_100;
			double newCurTime = (double)value / (double)100;
			job->curTime = newCurTime;
			if (job == NULL)
				return -1;
			insert_replayqueue(ReplayJob_queue, job);
		}

		job = create_bgjob(&(config->basic_app.apps[i]));
		if (job != NULL)
			insert_replayqueue(ReplayJob_queue, job);
	}

	for (i = 0; i < config->normal_app.app_count; i++)
	{
		Normal_list->push_back(&(config->normal_app.apps[i]));
	}

	return 0;
}

int load_replayjob(list <struct ReplayJob*> *ReplayJob_queue, list<struct App*> *ins_list, list<struct App*> *unins_list, double *day)
{
	FILE *fp;
	char line[PATH_MAX];
	list<struct App*>::iterator app_it;
	list<struct ReplayJob*>::iterator job_it = ReplayJob_queue->begin();

	fp = fopen(prev_out_name.c_str(), "r");
	if (fp == NULL) {
		cout << "Warning: No exist " << prev_out_name << endl;
		return 0;
	}
	while (fgets(line, PATH_MAX, fp) != NULL)
	{
		char *ptr;
		char type;
		char name[PATH_MAX];
		char path[PATH_MAX];
		double loading_time, update_time, bg_time;
		double num1, num2;
		int curLoading = 1;

		if (line[0] == 'B' && line[1] == '\t')
			type = 'B';
		else if (line[0] == 'N' && line[1] == '\t')
			type = 'N';
		else if (line[0] == 'A' && line[1] == '\t')
			type = 'A';
		else if (line[0] == 'C' && line[1] == '\t')
			type = 'C';
		else if (line[0] == 'D' && line[1] == '\t')
			type = 'D';
		else if (line[0] == 'S' && line[1] == '\t')
			type = 'S';
		else if (line[0] == 'M' && line[1] == '\t')
			type = 'M';
		else
			continue;
		ptr = strtok(line, "\t");
		if (ptr == NULL) continue;
		ptr = strtok(NULL, "\t");
		if (ptr == NULL) continue;
	
		if (type == 'D') {
			*day = atof(ptr);
			continue;
		} else if (type == 'A' || type == 'C')
		{
			num1 = atof(ptr);
			ptr = strtok(NULL, "\t");
			if (ptr == NULL) continue;
			num2 = atof(ptr);
		}
		else if (type == 'S')
		{
			replay_stat.loading = atoi(ptr);
			ptr = strtok(NULL, "\t");
			if (ptr == NULL) continue;
			replay_stat.update = atoi(ptr);
			ptr = strtok(NULL, "\t");
			if (ptr == NULL) continue;
			replay_stat.install = atoi(ptr);
			ptr = strtok(NULL, "\t");
			if (ptr == NULL) continue;
			replay_stat.uninstall = atoi(ptr);
			ptr = strtok(NULL, "\t");
			if (ptr == NULL) continue;
			replay_stat.camera_create = atoi(ptr);
			ptr = strtok(NULL, "\t");
			if (ptr == NULL) continue;
			replay_stat.camera_delete = atoi(ptr);
			continue;
		}
		else if (type == 'M')
		{
			memset(path, 0, sizeof(path));
			sprintf(path, "%s", ptr);
			ptr = strtok(NULL, "\t");
			if (ptr == NULL) continue;
			num1 = atof(ptr);
			ptr = strtok(NULL, "\t");
			if (ptr == NULL) continue;
			num2 = atof(ptr);
		}
		else {
			strncpy(name, ptr, strlen(ptr));
			path[strlen(ptr)] = 0x0;
			ptr = strtok(NULL, "\t");
			if (ptr == NULL) continue;
			strncpy(path, ptr, strlen(ptr));
			path[strlen(ptr)] = 0x0;
			ptr = strtok(NULL, "\t");
			if (ptr == NULL) continue;
			loading_time = atof(ptr);
			ptr = strtok(NULL, "\t");
			if (ptr == NULL) continue;
			update_time = atof(ptr);
			ptr = strtok(NULL, "\t");
			if (ptr == NULL) continue;
			bg_time = atof(ptr);
			ptr = strtok(NULL, "\t");
			if (ptr == NULL) continue;
			curLoading = atoi(ptr);
		}

		if (type == 'B') {
			for (job_it = ReplayJob_queue->begin(); job_it != ReplayJob_queue->end(); ++job_it) 
			{
				if (strcmp((*job_it)->path, path) == 0) {
					if ((*job_it)->type == REPLAY_LOADING) {
						(*job_it)->curTime = loading_time;
						(*job_it)->curLoading = curLoading;
					}
					else if ((*job_it)->type == REPLAY_UPDATE)
						(*job_it)->curTime = update_time;
				}
			}
		}
		else if (type == 'N') {
			for (app_it = unins_list->begin(); app_it != unins_list->end(); ++app_it)
			{
				if (strcmp((*app_it)->path, path) == 0) {
					app_install(ReplayJob_queue, ins_list, unins_list, app_it, 
										loading_time, update_time, bg_time, curLoading);
					break;
				}
			}
		}
		else if (type == 'A') {
			for (job_it = ReplayJob_queue->begin(); job_it != ReplayJob_queue->end(); ++job_it) 
			{
				if ((*job_it)->type == REPLAY_INSTALL)
						(*job_it)->curTime = num1;
				else if ((*job_it)->type == REPLAY_UNINSTALL)
					(*job_it)->curTime = num2;
			}
		}
		else if (type == 'C') {
			for (job_it = ReplayJob_queue->begin(); job_it != ReplayJob_queue->end(); ++job_it) 
			{
				if ((*job_it)->type == REPLAY_CAMERA)
					(*job_it)->curTime = num1;
				else if ((*job_it)->type == REPLAY_CAMERA_DELETE)
					(*job_it)->curTime = num2;
			}
		}
		else if (type == 'M') {
			for (job_it = ReplayJob_queue->begin(); job_it != ReplayJob_queue->end(); ++job_it) 
			{
				if (strcmp((*job_it)->path, path) == 0) {
					if ((*job_it)->type == REPLAY_MULTI)
						(*job_it)->curTime = num1;
					else if ((*job_it)->type == REPLAY_MULTI_DELETE)
						(*job_it)->curTime = num2;
				}
			}
		}
	}

	return 0;
}

int store_replayjob(list <struct ReplayJob*> *ReplayJob_queue, list<struct App*> *ins_list, double day)
{
	list<struct App*>::iterator app_it;
	list<struct ReplayJob*>::iterator job_it = ReplayJob_queue->begin();
	FILE *fp;
	char buf[PATH_MAX];
	memset(buf, 0, PATH_MAX);
	sprintf(buf, "replay_%d.out", (int)day);
	fp = fopen(buf, "w");
	if (fp == NULL)
		return -1;

	double install = 0;
	double uninstall = 0;
	double camera = 0;
	double camera_d = 0;
	fprintf(fp, "D\t%lf\n", day);

	fprintf(fp, "S\t%d\t%d\t%d\t%d\t%d\t%d\t\n",
			replay_stat.loading, replay_stat.update, replay_stat.install, replay_stat.uninstall,
			replay_stat.camera_create, replay_stat.camera_delete);

	for (job_it = ReplayJob_queue->begin(); job_it != ReplayJob_queue->end(); ++job_it) 
	{
		if ((*job_it)->type == REPLAY_INSTALL)
			install = (*job_it)->curTime;
		else if ((*job_it)->type == REPLAY_UNINSTALL)
			uninstall = (*job_it)->curTime;
		else if ((*job_it)->type == REPLAY_CAMERA)
			camera = (*job_it)->curTime;
		else if ((*job_it)->type == REPLAY_CAMERA_DELETE)
			camera_d = (*job_it)->curTime;
	}
	fprintf(fp, "A\t%lf\t%lf\t\n", install, uninstall);
	fprintf(fp, "C\t%lf\t%lf\t\n", camera, camera_d);

	for (int i = 0; i < config->multi.mul_others.size(); i++)
	{
		string name = config->multi.mul_others[i].name;
		double multi = 0;
		double multi_d = 0;
		for (job_it = ReplayJob_queue->begin(); job_it != ReplayJob_queue->end(); ++job_it) 
		{
			if (name.compare((*job_it)->path) == 0) 
			{
				if ((*job_it)->type == REPLAY_MULTI)
					multi = (*job_it)->curTime;
				else if ((*job_it)->type == REPLAY_MULTI_DELETE)
					multi_d = (*job_it)->curTime;
			}
		}
		fprintf(fp, "M\t%s\t%lf\t%lf\t\n", name.c_str(), multi, multi_d);
	}

	for (int i = 0; i < config->basic_app.app_count; i++)
	{
		double loading_time = 0;
		double update_time = 0;
		double bg_time = 0;
		int curLoading = 0;

		for (job_it = ReplayJob_queue->begin(); job_it != ReplayJob_queue->end(); ++job_it) 
		{
			if (strcmp((*job_it)->path, config->basic_app.apps[i].path) == 0) {
				if ((*job_it)->type == REPLAY_LOADING) {
					loading_time = (*job_it)->curTime;
					curLoading = (*job_it)->curLoading;
				}
				else if ((*job_it)->type == REPLAY_UPDATE)
					update_time = (*job_it)->curTime;
				else if ((*job_it)->type == REPLAY_BG)
					bg_time = (*job_it)->curTime;
			}
		}
		fprintf(fp, "B\t%s\t%s\t%lf\t%lf\t%lf\t%d\t\n", config->basic_app.apps[i].name, 
			config->basic_app.apps[i].path, loading_time, update_time, bg_time, curLoading);
	}

	for (app_it = ins_list->begin(); app_it != ins_list->end(); ++app_it)
	{
		double loading_time = 0;
		double update_time = 0;
		double bg_time = 0;
		int curLoading = 0;

		for (job_it = ReplayJob_queue->begin(); job_it != ReplayJob_queue->end(); ++job_it) 
		{
			if (strcmp((*job_it)->path, (*app_it)->path) == 0) {
				if ((*job_it)->type == REPLAY_LOADING) {
					loading_time = (*job_it)->curTime;
					curLoading = (*job_it)->curLoading;
				}
				else if ((*job_it)->type == REPLAY_UPDATE)
					update_time = (*job_it)->curTime;
				else if ((*job_it)->type == REPLAY_BG)
					bg_time = (*job_it)->curTime;
			}
		}
		fprintf(fp, "N\t%s\t%s\t%lf\t%lf\t%lf\t%d\t\n", (*app_it)->name, (*app_it)->path, 
							loading_time, update_time, bg_time, curLoading);
	}

	return 0;
}


struct ReplayJob* create_bgjob(struct App* app)
{
	int i; 
	struct ReplayJob *job; 

	if (app->bg_file.size() == 0)
		return NULL;
	if (app->bg_cycle == 0)
		return NULL;

	job = create_replayjob(REPLAY_BG, app->name, app->path, app->bg_cycle);
	
	if (job == NULL)
		return NULL;

	for (i = 0; i < app->bg_file.size(); i++) {
		job->bgjob.push_back(app->bg_file[i]);
	}

	return job;
}

int insert_replayqueue(list<struct ReplayJob*> *ReplayJob_queue, struct ReplayJob* job)
{
	list<struct ReplayJob*>::iterator iter = ReplayJob_queue->begin();

	for (iter = ReplayJob_queue->begin(); iter != ReplayJob_queue->end(); ++iter) 
	{
		if ((*iter)->curTime > job->curTime) {
			ReplayJob_queue->insert(iter, job);
			break;
		}
	}

	if (iter == ReplayJob_queue->end()) {
		ReplayJob_queue->insert(iter, job);
	}

	return 0;
}

int uninstall_replayqueue(list<struct ReplayJob*> *ReplayJob_queue, const char *path)
{
	list<struct ReplayJob*>::iterator iter = ReplayJob_queue->begin();

	while (iter != ReplayJob_queue->end())
	{
		if (strcmp((*iter)->path, path) == 0) {
			struct ReplayJob *job = (*iter);
			iter = ReplayJob_queue->erase(iter);
			delete job;
			job = NULL;
		}
		else
			iter++;
	}

	return 0;
}

struct ReplayJob* create_replayjob(enum REPLAY_TYPE type, const char *name, const char *path, double cycle)
{
	struct ReplayJob* job;
	job = new struct ReplayJob;
	if (job == NULL)
		return NULL;
	job->type = type;
	memset(job->name, 0, MAX_NAME);
	sprintf(job->name, "%s", name);
	memset(job->path, 0, PATH_MAX + 1);
	sprintf(job->path, "%s", path);
	job->cycle = cycle;
	job->curTime = cycle;
	job->loadJob = NULL;
	return job;
}

struct ReplayJob* create_replayjob(enum REPLAY_TYPE type, struct ReplayJob *replay)
{
	struct ReplayJob* job;
	job = new struct ReplayJob;
	if (job == NULL)
		return NULL;
	job->type = type;
	memset(job->name, 0, MAX_NAME);
	sprintf(job->name, "%s", replay->name);
	memset(job->path, 0, PATH_MAX + 1);
	sprintf(job->path, "%s", replay->path);
	job->cycle = replay->cycle;
	job->curTime = replay->cycle;
	job->curLoading = replay->curLoading;
	job->maxLoading = replay->maxLoading;
	job->loadJob = NULL;
	return job;
}

static struct mntent *get_mount_point(const char *name)
{
	/* Refer to /etc/mtab */
	const char	*mtab = MOUNTED;
	FILE		*fp = NULL;
	struct mntent	*mnt = NULL;
	struct stat	s;
	dev_t mountDevice;

	if (stat(name, &s) < 0) {
		return NULL;
	}

	if ((s.st_mode & S_IFMT) == S_IFBLK)
		mountDevice = s.st_rdev;
    else
		mountDevice = s.st_dev;

	fp = setmntent(mtab, "r");
	if (fp == NULL) {
		perror("Couldn't access /etc/mtab");
		return NULL;
	}

	while ((mnt = getmntent(fp)) != NULL) {
		if (strcmp(name, mnt->mnt_dir) == 0
				|| strcmp(name, mnt->mnt_fsname) == 0)	/* String match. */
			break;
		if (stat(mnt->mnt_fsname, &s) == 0 && s.st_rdev == mountDevice)	/* Match the device. */
			break;
		if (stat(mnt->mnt_dir, &s) == 0 && s.st_dev == mountDevice)	/* Match the directory's mount point. */
			break;
	}
	endmntent(fp);
	return mnt;
}

static double get_utilization(void)
{
	struct statfs s;
	long blocks_used;
	double blocks_percent_used;
	struct fstab* fstabItem;
	char	dir_name[PATH_MAX + 1];
	int ret = 0;
	struct mntent *mountEntry;

	memset(dir_name, 0, PATH_MAX + 1);
	mountEntry = get_mount_point(config->mount_dir);
	if (mountEntry == NULL)
		return -1;

	if (statfs(mountEntry->mnt_dir, &s) != 0) {
		return -1;
	}

	if (s.f_blocks > 0) {
		blocks_used = s.f_blocks - s.f_bfree;
		blocks_percent_used = (double)
			    (blocks_used * 100.0 / (double)(blocks_used + s.f_bavail) + 0.5);
	}

	return blocks_percent_used;
}

static int do_fulldisk(list <struct ReplayJob*> *queue, list<struct App*> *ins_list, 
						list<struct App*> *unins_list, double curTime)
{
	int uninstall_count = config->fulldisk.uninstall_app;
	int camera_delete = config->fulldisk.camera_delete;

	for (int i = 0; i < uninstall_count; i++)
	{
		replay_uninstall(queue, ins_list, unins_list, curTime);
	}

	for (int i = 0; i < camera_delete; i++)
	{
		struct ReplayJob job;
		memset(job.path, 0, PATH_MAX + 1);
		sprintf(job.path, "%s", config->multi.mul_camera.name.c_str());
		memset(job.name, 0, PATH_MAX + 1);
		sprintf(job.name, "%s", config->multi.mul_camera.multimedia_path.c_str());
		job.curTime = curTime;
		replay_camera_delete(&job);
	}

	for (int cnt = 0; cnt < config->fulldisk.mul_delete.size(); cnt++)
	{
		for (int i = 0; i < config->fulldisk.mul_delete[cnt].mul_delete; i++)
		{
			struct ReplayJob job;
			memset(job.path, 0, PATH_MAX + 1);
			sprintf(job.path, "%s", config->fulldisk.mul_delete[cnt].name.c_str());
			memset(job.name, 0, PATH_MAX + 1);
			sprintf(job.name, "%s", config->fulldisk.mul_delete[cnt].path.c_str());
			job.curTime = curTime;
			replay_camera_delete(&job);
		}
	}

	return 0;
}

static string get_clear_path(string trace_path)
{
    string path = trace_path;
    while (1) {
        int found = path.find("//");
        if (found == std::string::npos)
            break;
        path.replace(found, 2, "/");
    }
    return path;
}

string dbtype_to_string(enum DBTYPE type)
{
	if (type == DBTYPE_JOURNAL)
		return string("journal");
	else if (type == DBTYPE_FIXED)
		return string("fixed");
	else if (type == DBTYPE_DELETE)
		return string ("delete");
	
	return string("insert");
}

static void init_db_manager(struct ReplayJob *replay_loading, char *mount_dir, string app_name, string app_path, string app_ps, int total_file)
{
	vector<struct DBInfo*> DBvec;

	analysis_database(DBvec, app_path, app_name, app_ps, total_file); 
	for (int i = 0; i < DBvec.size(); i++) 
	{
		struct DBInfo *result = DBvec[i];
		string path = get_clear_path(string(mount_dir)+string("/")+result->path);
		replay_loading->db.db_map.insert(pair<string, struct DBInfo*> (path, result));

		if (result->type == DBTYPE_INSERT)
			cout << path << " " << dbtype_to_string(result->type) << " " << result->hot_brate << " " << result->cold_brate << " " << result->hot_wrate << endl;
		else
			cout << path << " " << dbtype_to_string(result->type) << endl;
	}

}

double randn (double mu, double sigma)
{
  double U1, U2, W, mult;
  static double X1, X2;
  static int call = 0;

  if (call == 1)
    {
      call = !call;
      return (mu + sigma * (double) X2);
    }

  do
    {
      U1 = -1 + ((double) rand () / RAND_MAX) * 2;
      U2 = -1 + ((double) rand () / RAND_MAX) * 2;
      W = pow (U1, 2) + pow (U2, 2);
    }
  while (W >= 1 || W == 0);

  mult = sqrt ((-2 * log (W)) / W);
  X1 = U1 * mult;
  X2 = U2 * mult;

  call = !call;

  return (mu + sigma * (double) X1);
}




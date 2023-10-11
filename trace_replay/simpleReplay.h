//#include "traceReplay.h"
#include "cacheReplay.h"

#ifndef _SIMPLEREPLAY_H
#define _SIMPLEREPLAY_H
int create_camera_file(char *mount_dir, char* camera_path, char* ext_path, unsigned long long size);
int delete_camera_file(char* mount_dir, char* camera_path);
int create_init_files(char* mount_dir, char* init_name);
double do_trace_replay(char* mount_dir, char *input_name, struct ReplayJob* job, double curTime=0.0);
double do_trace_replay(struct ReplayFile *load_replay, list<struct ReplayFile> *update_list);
int init_cache(char* mount_dir, const char* prefix, struct CacheDirInfo *cacheDirInfo);

extern double IG_curTime;
extern string IG_outPath;
extern int IG_mode;
extern int mode_flag;

#endif

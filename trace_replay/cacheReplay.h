#include "traceReplay.h"

int select_filesize(vector <long long int> *sizevec);
int select_unique(double unique);
struct CacheFileInfo* try_reuse(struct CacheRef *ref);
int select_ref(double onetimes, double c, double zipf_slope, int max_ref);
double calc_c(int max_ref);

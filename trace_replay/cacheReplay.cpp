#ifndef _LARGEFILE_SOURCE
#define _LARGEFILE_SOURCE
#endif
#ifndef __USE_LARGEFILE64
#define __USE_LARGEFILE64
#endif
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cacheReplay.h"

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
#include <math.h>

using namespace std;

double rand_val(void);

int select_filesize(vector<long long int> *sizevec)
{
	int vecsize = (*sizevec).size();
	int random;

	if (vecsize == 0)
		return 16*1024;
	random = rand() % vecsize;

	int	value = (*sizevec)[random];
	
	return value;
}

int select_unique(double unique)
{
	int random = rand() % 1000;
	if (random < unique * 1000)
		return true;

	return false;
}

struct CacheFileInfo* try_reuse(struct CacheRef *fileref)
{
	int refsize = fileref->total_ref;
	int random, number;
	
	if (refsize <= 0)
		return NULL;

	random = rand() % refsize;
	std::list<CacheFileInfo*>::iterator it;

	number = 0;

	for (it = fileref->filelist.begin(); it != fileref->filelist.end(); ++it) {
		struct CacheFileInfo* fileinfo = *(it);
		number += fileinfo->ref;
		if (random < number) {
			fileinfo->ref--;
			if (fileref->total_ref > 0) 
				fileref->total_ref--;
			else {
				cout << "WARNING: total_ref: to fix ref" << endl;
				std::list<CacheFileInfo*>::iterator it2;
				int new_ref = 0;
				for (it2 = fileref->filelist.begin(); it2 != fileref->filelist.end(); ++it2) {
					new_ref += fileinfo->ref;
				}
				fileref->total_ref = new_ref;
			}		
			if (fileinfo->ref <= 0) {
				fileref->filelist.erase(it);
			}
			return fileinfo;
		}
	}
	cout << "ERROR: try_reuse " << refsize << " " << number << endl;
	return NULL;
}

double rand_val(void)
{
	const long  a =      16807;  // Multiplier
    const long  m = 2147483647;  // Modulus
	const long  q =     127773;  // m div a
	const long  r =       2836;  // m mod a
	static long x = 1;           // Random int value
    long        x_div_q;         // x divided by q
	long        x_mod_q;         // x modulo q
	long        x_new;           // New x value

// RNG using integer arithmetic
	x_div_q = x / q;
	x_mod_q = x % q;
	x_new = (a * x_mod_q) - (r * x_div_q);
	if (x_new > 0)
		x = x_new;
	else
		x = x_new + m;

	// Return a random value between 0.0 and 1.0
	return((double) x / m);
}

double calc_c(int max_ref)
{
	int i;
	double c = 0;
	double alpha = 1.0;

	for (i = 1; i <= max_ref; i++)
		c = c + (1/pow((double)i, alpha));
	c = 1.0 / c;

	return c;
}

int select_ref(double onetimes, double c, double zipf_slope, int max_ref)
{
	int random, i;
	double zipf_value;
	double sum_prob;
	double urand;
	double alpha = 1.0;

	random = rand() % 1000;

	if (random < onetimes * 1000)
		return 0;

	do {
		urand = rand_val();
	}
	while ((urand == 0) || (urand == 1));

	sum_prob = 0;
	for (i = 1; i <= max_ref; i++)
	{
		sum_prob = sum_prob + c / pow((double) i, zipf_slope);
		if (sum_prob >= urand) {
			zipf_value = i;
			break;
		}
	}

	return zipf_value;
}

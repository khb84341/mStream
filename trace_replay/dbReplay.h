#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <list>

using namespace std;


enum DBTYPE
{
    DBTYPE_JOURNAL,
    DBTYPE_FIXED,
    DBTYPE_DELETE,
    DBTYPE_INSERT
};

enum DBTEMP
{
    DBTEMP_HOT = 0,
    DBTEMP_WARM = 1,
    DBTEMP_COLD = 2,
};


struct DBInfo
{
	string path;

	DBTYPE type;
	int meta_offset;
	int cold_brate;	// [0-100]
	int hot_brate;	// [0-100]
	int hot_wrate; // [0-100]

	int limit_size; // filesize * 10;

	map<int, int> block_type;
	vector<int> hot_list;
	vector<int> warm_list;
};

void analysis_database(vector<struct DBInfo*> &DBvec, string app_path, string app_name, string app_ps, int total_loading_file);



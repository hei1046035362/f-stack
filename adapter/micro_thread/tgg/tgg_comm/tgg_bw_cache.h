#ifndef __TGG_BW_CACHE_H__
#define __TGG_BW_CACHE_H__

#include <string>
#include <list>
#include <map>
#include <set>
#include<vector>

uint32_t tgg_get_seed();

/// 增删查  gid hash<gid, list<fdid> >
int tgg_add_gid(const char* gid, int64_t fdidcid);
int tgg_add_gid(uint64_t gid, int64_t fdidcid);

int tgg_del_gid(const char* gid);
int tgg_del_gid(uint64_t gid);

int tgg_del_fd4gid(const char* gid, int64_t fdidcid);
int tgg_del_fd4gid(uint64_t gid, int64_t fdidcid);
// 返回格式  list<string(fdid:uid)>
int tgg_get_fdsbygid(const char* gid, std::vector<int64_t>& lst_fd);
int tgg_get_fdsbygid(uint64_t gid, std::vector<int64_t>& lst_fd);
typedef struct st_tgg_fd_list tgg_fd_list;
int tgg_get_fdsbygid(uint64_t gid, tgg_fd_list* lst_fd);
int tgg_get_cidcount_bygid(uint64_t gid);
// 获取所有在线的分组
int tgg_get_allonlinegids(std::vector<uint64_t>& lst_gid);
int tgg_get_gid_count();
void tgg_clean_gid();

/// 增删查  uid  hash<uid, list<fdid> >
int tgg_add_uid(const char* uid, int64_t fdidcid);
int tgg_del_uid(const char* uid);
int tgg_del_fd4uid(const char* uid, int64_t fdidcid);
void tgg_clean_uid();
// 返回格式  list<string(fdid:uid)>
int tgg_get_fdsbyuid(uint64_t uid, std::vector<int64_t>& lst_fd);
int tgg_get_fdsbyuid(const char* uid, std::vector<int64_t>& lst_fd);
int tgg_get_allonlineuids(std::vector<uint64_t>& lst_uid);
int tgg_get_uid_count();

/// 增删查  cid hash<cid, fdid>
int tgg_add_cid(int64_t cid, int64_t fdidcid);
int tgg_del_cid(int64_t cid);
void tgg_clean_cid();
int64_t tgg_get_fdbycid(int64_t cid);
int tgg_get_allonlinecids(std::vector<int64_t>& lst_cids);
int tgg_get_allfds(std::vector<int64_t>& lst_fds);
int tgg_get_cid_count();
// void tgg_clean_allcids_bypid(int prc_id);

/// 增删查  cid->gid映射 hash<cid, list<gid> >
int tgg_add_cidgid(int64_t cid, const char* gid);
int tgg_del_cid_cidgid(int64_t cid);
void tgg_clean_cidgid();
int tgg_get_gidsbycid(int64_t cid, std::vector<std::string>& lst_gid);
// 删除指定cid下的gid   单个用户退出群组使用
int tgg_del_gid_cidgid(int64_t cid, const char* gid);
// 解散群组时联动操作 对群内所有cid执行 tgg_del_gid_cidgid(cid, gid)
void tgg_del_gid_cidgid(const char* gid);

// 返回格式  list<string(uid)>
int tgg_get_gidsbyuid(const char* uid, std::set<std::string>& lst_gid);
// void tgg_iterprint_gidsbyuid(const char* uid = NULL);


/// 增删查  idx hash<idx, NULL>  查询全局有效clientid使用的idx
int tgg_add_idx(int coreid, int64_t idx);
int tgg_del_idx(int coreid, int64_t idx);
int tgg_check_idx_exist(int coreid, int64_t idx);
int tgg_get_allidxs(int coreid, std::vector<int64_t>& lst_idxs);
int tgg_count_idx(int coreid);
void tgg_iter_del_idx(int coreid);

int tgg_add_bwfdx(int64_t bwfdx);
int tgg_del_bwfdx(int64_t bwfdx);
int tgg_check_bwfdx_exist(int64_t bwfdx);
int tgg_get_bwfdx_count();
int tgg_get_bwfdx_bypos(int pos);
void tgg_iter_del_bwfdx(int prc_id);
void tgg_getall_bwfdx(std::vector<int64_t>& vec_bwfdx);

// 获取负载最小的bwfdx
int tgg_get_load_balance(std::vector<int64_t>& vec_bwfdx, unsigned int ipport);


int tgg_check_bwwkkey_exist(const char* bwwkkey);
int tgg_del_bwwkkey(const char* bwwkkey);
int tgg_add_bwwkkey(const char* bwwkkey);
int tgg_get_allbwwkkeys(std::vector<uint64_t>& lst_wkkeys);
int tgg_get_bwwoker_count();

// 排除cid列表 sendgroup时，会有一个排除的cid列表，标准库的set和unordered_set效率太低
int tgg_add_expt_cid(int prc_id, int64_t cid);
int tgg_check_expt_cid_exist(int prc_id, int64_t cid);
void tgg_reset_expt_cid(int prc_id);

void print_hash_statistics();

#endif  // __TGG_BW_CACHE_H__
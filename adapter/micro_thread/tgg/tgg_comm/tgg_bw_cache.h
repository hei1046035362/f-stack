#ifndef __TGG_BW_CACHE_H__
#define __TGG_BW_CACHE_H__

#include <string>
#include <list>
#include <map>
#include <set>
#include<vector>

template<typename type>
void iter_del_list(type* iddata)
{
    if (!iddata) {
        return;
    }
    type* iter = iddata;// 第一个节点不存数据，先删除数据节点
    while(iter->next) {
        type* tmp = iter->next;
        iter->next = iter->next->next;
        memset(tmp, 0, sizeof(type));
        dpdk_rte_free(tmp);
    }
    // 删除第一个节点
    memset(iddata, 0, sizeof(type));
    dpdk_rte_free(iddata);
}

void iter_del_fdlist(void* iddata);

void iter_del_idlist(void* iddata);

/// 增删查  gid hash<gid, list<fdid> >
int tgg_add_gid(const char* gid, int64_t fdidcid);
int tgg_del_gid(const char* gid);
int tgg_del_fd4gid(const char* gid, int64_t fdidcid);
// 返回格式  list<string(fdid:uid)>
int tgg_get_fdsbygid(const char* gid, std::list<int64_t>& lst_fd);
// 获取所有在线的分组
int tgg_get_allonlinegids(std::list<std::string>& lst_gid);
void tgg_clean_gid();

/// 增删查  uid  hash<uid, list<fdid> >
int tgg_add_uid(const char* uid, int64_t fdidcid);
int tgg_del_uid(const char* uid);
int tgg_del_fd4uid(const char* uid, int64_t fdidcid);
void tgg_clean_uid();
// 返回格式  list<string(fdid:uid)>
int tgg_get_fdsbyuid(const char* uid, std::list<int64_t>& lst_fd);

/// 增删查  cid hash<cid, fdid>
int tgg_add_cid(int64_t cid, int64_t fdidcid);
int tgg_del_cid(int64_t cid);
void tgg_clean_cid();
int64_t tgg_get_fdbycid(int64_t cid);
int tgg_get_allonlinecids(std::list<int64_t>& lst_cids);
int tgg_get_allfds(std::list<int64_t>& lst_fds);
// void tgg_clean_allcids_bypid(int prc_id);

/// 增删查  cid->gid映射 hash<cid, list<gid> >
int tgg_add_cidgid(int64_t cid, const char* gid);
int tgg_del_cid_cidgid(int64_t cid);
void tgg_clean_cidgid();
int tgg_get_gidsbycid(int64_t cid, std::list<std::string>& lst_gid);
// 删除指定cid下的gid   单个用户退出群组使用
int tgg_del_gid_cidgid(int64_t cid, const char* gid);
// 解散群组时联动操作 对群内所有cid执行 tgg_del_gid_cidgid(cid, gid)
void tgg_del_gid_cidgid(const char* gid);

// 返回格式  list<string(uid)>
int tgg_get_gidsbyuid(const char* uid, std::set<std::string>& lst_gid);
void tgg_iterprint_gidsbyuid(const char* uid = NULL);


/// 增删查  idx hash<idx, NULL>  查询全局有效clientid使用的idx
int tgg_add_idx(int coreid, int64_t idx);
int tgg_del_idx(int coreid, int64_t idx);
int tgg_check_idx_exist(int coreid, int64_t idx);
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
int tgg_get_load_balance(std::vector<int64_t>& vec_bwfdx);


int tgg_check_bwwkkey_exist(const char* bwwkkey);
int tgg_del_bwwkkey(const char* bwwkkey);
int tgg_add_bwwkkey(const char* bwwkkey);

#endif  // __TGG_BW_CACHE_H__
#ifndef __TGG_BW_CACHE_H__
#define __TGG_BW_CACHE_H__

#include <string>
#include <list>
#include <map>

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
        rte_free(tmp);
    }
    // 删除第一个节点
    memset(iddata, 0, sizeof(type));
    rte_free(iddata);
}

void iter_del_fdlist(void* iddata);

void iter_del_idlist(void* iddata);

/// 增删查  gid hash<gid, list<fd> >
int tgg_add_gid(const char* gid, int fd, int idx);
int tgg_del_gid(const char* gid);
int tgg_del_fd4gid(const char* gid, int fd, int idx);
// 返回格式  list<string(fd:uid)>
int tgg_get_fdsbygid(const char* gid, std::list<std::string>& lst_fd);
// 获取所有在线的分组
int tgg_get_allonlinegids(std::list<std::string>& lst_gid);

/// 增删查  uid  hash<uid, list<fd,idx> >
int tgg_add_uid(const char* uid, int fd, int idx);
int tgg_del_uid(const char* uid);
int tgg_del_fd4uid(const char* uid, int fd, int idx);
// 返回格式  list<string(fd:uid)>
int tgg_get_fdsbyuid(const char* uid, std::list<std::string>& lst_fd);

/// 增删查  cid hash<cid, fd>
int tgg_add_cid(const char* cid, int fd);
int tgg_del_cid(const char* cid);
int tgg_get_fdbycid(const char* cid);
int tgg_get_allonlinecids(std::list<std::string>& lst_cids);
int tgg_get_allfds(std::list<int>& lst_fds);

/// 增删查  uid->gid映射 hash<uid, list<gid> >
int tgg_add_uidgid(const char* uid, const char* gid);
int tgg_del_uid_uidgid(const char* uid);
// 删除指定uid下的gid
int tgg_del_gid_uidgid(const char* uid, const char* gid);
// 遍历删除所有uid中的指定gid
void tgg_del_gid_uidgid(const char* gid);

// 返回格式  list<string(uid)>
int tgg_get_gidsbyuid(const char* uid, std::list<std::string>& lst_gid);
void tgg_iterprint_gidsbyuid(const char* uid = NULL);


/// 增删查  idx hash<idx, NULL>  查询全局有效clientid使用的idx
int tgg_add_idx(int idx);
int tgg_del_idx(int idx);
int tgg_check_idx_exist(int idx);

#endif  // __TGG_BW_CACHE_H__
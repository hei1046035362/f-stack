#ifndef __TGG_BW_CACHE_H__
#endif __TGG_BW_CACHE_H__

#include <string>
#include <list>

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
        memset(tmp, 0, sizeof(type))
        rte_free(tmp);
    }
    // 删除第一个节点
    memset(iddata, 0, sizeof(type))
    rte_free(iddata);
}

void iter_del_fdlist(tgg_fd_list* iddata);

void iter_del_idlist(tgg_list_id* iddata);

/// 增删查  gid hash<gid, list<fd> >
int tgg_add_gid(char* gid, int fd, int idx);
int tgg_del_gid(char* gid);
int tgg_del_fd4gid(char* gid, int fd, int idx);
// 返回格式  list<string(fd:uid)>
std::list<std::string> tgg_get_gid(char* gid);

/// 增删查  uid  hash<uid, list<fd,idx> >
int tgg_add_uid(char* uid, int fd, int idx);
int tgg_del_uid(char* uid);
int tgg_del_fd4uid(char* uid, int fd, int idx);
// 返回格式  list<string(fd:uid)>
std::list<std::string> tgg_get_fdsbyuid(char* uid);

/// 增删查  cid hash<uid, fd>
int tgg_add_cid(char* cid, tgg_cid_data* data);
int tgg_del_cid(char* cid);
int tgg_get_fdbycid(char* cid);

/// 增删查  uid->gid映射 hash<uid, list<gid> >
int tgg_add_uidgid(char* uid, char* gid);
int tgg_del_uid_uidgid(const char* uid);
int tgg_del_gid_uidgid(const char* uid, const char* gid);

// 返回格式  list<string(uid)>
std::list<std::string> tgg_get_uidsbygid(char* gid);

#endif  // __TGG_BW_CACHE_H__
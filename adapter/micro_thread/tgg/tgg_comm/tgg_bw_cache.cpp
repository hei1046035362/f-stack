#include <rte_hash.h>
#include "tgg_common.h"
#include "tgg_bw_cache.h"
#include "tgg_lock_struct.h"
#include "TggLock.hpp"

extern const struct rte_hash *g_gid_hash = NULL;
extern const struct rte_hash *g_uid_hash = NULL;
extern const struct rte_hash *g_cid_hash = NULL;
extern const struct rte_hash *g_giduid_hash = NULL;


typedef void (*tgg_add_data)(void*);

typedef void (*tgg_free_id_data)(void*);

void iter_del_fdlist(tgg_fd_list* iddata)
{
    iter_del_list<tgg_fd_list>(iddata);
}

void iter_del_idlist(tgg_list_id* iddata)
{
    iter_del_list<tgg_list_id>(iddata);
}

// 针对key-value的hash
static int tgg_hash_add_key_value(const rte_hash* hash, const char* key, int fd)
{
    if (strlen(key) != 20) {
        RTE_LOG(ERR, USER1, "[%s][%d]add key failed,check if key[%s] is correct.", __func__, __LINE__, key);
        return -EINVAL; 
    }
    if (!data) {
        RTE_LOG(ERR, USER1, "[%s][%d]add key[%s] failed, invalid data.", __func__, __LINE__, key);
        return -EINVAL; 
    }
    int ret = rte_hash_lookup(hash, key);
    if (ret < 0) {
        int ret = rte_hash_add_key_data(hash, key, data);
        if (ret < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d]add key[%s] failed:%d.", __func__, __LINE__, key, ret);
            return ret;
        }
    } else {
        int* value = NULL;
        ret = rte_hash_lookup_data(hash, key, &value);
        if (ret < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed:%d", __func__, __LINE__, key, ret);
            return -1;
        }
        if(!value) {
            value = dpdk_rte_malloc(sizeof(int));
        }
        if(!value) {
            RTE_LOG(ERR, USER1, "[%s][%d]add key[%s] failed:%d.", __func__, __LINE__, key, ret);
            return -1;
        }
        *value = fd;
        int ret = rte_hash_add_key_data(hash, key, data);
        if (ret < 0) {
            *value = 0;
            rte_free(value);
            RTE_LOG(ERR, USER1, "[%s][%d]add key[%s] failed:%d.", __func__, __LINE__, key, ret);
            return ret;
        }
    }

    return 0;
}

// 针对key-list的hash
static int tgg_hash_add_keywithfdlst(const rte_hash* hash, const char* key, int fd, int idx)
{
    int ret = rte_hash_lookup(hash, gid);
    tgg_fd_list* pdata = NULL;
    if (ret < 0) {// 首次插入
        RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed,hash key not exist:%d", __func__, __LINE__, key, ret);
        pdata = dpdk_rte_malloc(sizeof(tgg_fd_list));
        if(!pdata) {
            return -1;
        }
        pdata->idx = idx;
        pdata->fd = fd;
        pdata->next = dpdk_rte_malloc(sizeof(tgg_fd_list));
        if(!pdata->next) {
            rte_free(pdata);
            return -1;
        }
        memcpy(pdata->next->data, uid, strlen(uid));
        pdata->next->next = NULL;
        int ret = rte_hash_add_key_data(hash, gid, pdata);
        if (ret < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d]add key[%s] failed:%d.", __func__, __LINE__, key, ret);
            memset(pdata->next, 0 ,sizeof(tgg_fd_list));
            rte_free(pdata->next);
            memset(pdata, 0 ,sizeof(tgg_fd_list));
            rte_free(pdata);
            return ret;
        }
    } else {// 已存在节点
        // TODO 要加进程锁
        tgg_fd_list* value = NULL;
        ret = rte_hash_lookup_data(hash, key, &value);
        if (ret < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed:%d", __func__, __LINE__, key, ret);
            return -1;
        }
        pdata = value->next;
        while (pdata->next) {
            // TODO 对于已存在的fd+idx是否要比较，可能会有性能损耗
            if(pdata->next->idx == idx && pdata->next->fd == fd) {
                break;
            }
            pdata = pdata->next;
        }
        if(pdata->next) {
            return 0;
        }
        tgg_fd_list* tmp = dpdk_rte_malloc(sizeof(tgg_fd_list));
        if(!tmp) {
            return -1;
        }
        tmp->idx = idx;
        tmp->fd = fd;
        tmp->next = NULL;
        pdata->next = tmp;
    }

}

// 获取hash value/list
static void* tgg_hash_get_key(const rte_hash* hash, const char* key)
{
    int ret = rte_hash_lookup(hash, key);
    if (ret < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed,hash key not exist:%d", __func__, __LINE__, key, ret);
        return NULL;
    }
    void* pdata = NULL;
    ret = rte_hash_lookup_data(hash, key, &pdata);
    if (ret < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed:%d", __func__, __LINE__, key, ret);
        return NULL;
    }
    return pdata;
}

// 删除整个key
static int tgg_hash_del_key(const rte_hash* hash, const char* key, tgg_free_id_data fp)
{
    tgg_gid_data* pdata = tgg_get_gid(key);
    if (!pdata)
        return -EINVAL;

    // 释放value的空间
    fp((void*)pdata);

    int ret = rte_hash_del_key(hash, key);
    if (ret > 0) {
        // 在并发情况下删除key之后，位置还在，需要删除位置信息，详情参考函数说明
        if (rte_hash_free_key_with_position(hash, ret) < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d]Del key[%s] pos failed:%d", __func__, __LINE__, key, ret);
            return -EINVAL;
        }
    } else {
        RTE_LOG(ERR, USER1, "[%s][%d]Del key[%s] data failed:%d", __func__, __LINE__, key, ret);
        return -EINVAL;
    }
    return 0;
}

// 删除hash value为list中的单个元素,list节点中的值为fd和idx两个元素
static int tgg_hash_del_fdlst4key(const rte_hash* hash, const char* key, int fd, int idx)
{
    int ret = rte_hash_lookup(hash, key);
    if (ret < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed,hash key not exist:%d", __func__, __LINE__, key, ret);
        return -1;
    }
    tgg_fd_list* value = NULL;
    ret = rte_hash_lookup_data(hash, key, &value);
    if (ret < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed:%d", __func__, __LINE__, key, ret);
        return -1;
    }
    pdata = value->next;
    int cmplen = sizeof(idx) + sizeof(fd);
    while (pdata->next) {
        // TODO 对于已存在的fd+idx是否要比较，可能会有性能损耗
        if(pdata->next->idx == idx && pdata->next->fd == fd) {
            tgg_fd_list* tmp = pdata->next->next;
            memset(pdata->next, 0, sizeof(tgg_fd_list));
            rte_free(pdata->next);
            pdata->next = tmp;
            break;
        } else {
            pdata = pdata->next;
        }
    }

    return 0;
}

// 删除hash value为list中的单个元素,list节点中的值为char[]
static int tgg_hash_del_idlst4key(const rte_hash* hash, const char* key, char* id)
{
    int ret = rte_hash_lookup(hash, key);
    if (ret < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed,hash key not exist:%d", __func__, __LINE__, key, ret);
        return -1;
    }
    tgg_list_id* value = NULL;
    ret = rte_hash_lookup_data(hash, key, &value);
    if (ret < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed:%d", __func__, __LINE__, key, ret);
        return -1;
    }
    pdata = value->next;
    while (pdata->next) {
        // TODO 对于已存在的fd+idx是否要比较，可能会有性能损耗
        if(strncmp(pdata->next->data, id, sizeof(pdata->next->data))) {
            tgg_list_id* tmp = pdata->next->next;
            memset(pdata->next, 0, sizeof(tgg_list_id));
            rte_free(pdata->next);
            pdata->next = tmp;
            break;
        } else {
            pdata = pdata->next;
        }
    }

    return 0;
}


/// 增删查  gid
int tgg_add_gid(char* gid, int fd, int idx)
{
    WriteLock(get_gidfd_lock());
    return tgg_hash_add_keywithfdlst(g_gid_hash, gid, fd, idx);
}

std::list<std::string> tgg_get_gid(char* gid)
{
    ReadLock(get_gidfd_lock());
    std::list<std::string> lst_fd;
    tgg_gid_data* value = tgg_hash_get_key(g_gid_hash, gid);
    if(!value) {
        return lst_fd;
    }
    while (value->next) {
        std::string item = std::to_string(value->next->fd) + 
                            std::string(":") + 
                            std::to_string(value->next->idx);
        lst_fd.append(item);
    }
    return lst_fd;
}

int tgg_del_gid(char* gid)
{
    WriteLock(get_gidfd_lock());
    return tgg_hash_del_key(g_gid_hash, gid, iter_del_fdlist);
}

int tgg_del_fd4gid(char* gid, int fd, int idx)
{
    WriteLock(get_gidfd_lock());
    return tgg_hash_del_fdlst4key(g_gid_hash, gid, fd, idx);
}

/// 增删查  uid 用户id 
int tgg_add_uid(char* uid, int fd, int idx)
{
    WriteLock(get_uidfd_lock());
    return tgg_hash_add_keywithfdlst(g_uid_hash, uid, fd, idx);
}

int tgg_del_uid(char* uid)
{
    WriteLock(get_uidfd_lock());
    return tgg_hash_del_key(g_uid_hash, uid, iter_del_fdlist);
}

int tgg_del_fd4uid(char* uid, int fd, int idx)
{
    WriteLock(get_uidfd_lock());
    return tgg_hash_del_fdlst4key(g_uid_hash, uid, fd, idx);
}

std::list<std::string> tgg_get_uid(char* uid);
{
    ReadLock(get_uidfd_lock());
    std::list<std::string> lst_fd;
    tgg_uid_data* value = tgg_hash_get_key(g_uid_hash, uid);
    if(!value) {
        return lst_fd;
    }
    while (value->next) {
        std::string item = std::to_string(value->next->fd) + 
                            std::string(":") + 
                            std::to_string(value->next->idx);
        lst_fd.append(item);
    }
    return lst_fd;
}

/// 增删查  cid
int tgg_add_cid(char* cid, int fd);
{
    WriteLock(get_cidfd_lock());
    return tgg_hash_add_key_value(g_cid_hash, cid, fd);
}

static void free_ciddata(void* data)
{
    int* pdata = (int*)data;
    *pdata = 0;
    rte_free(pdata);
}

int tgg_del_cid(char* cid);
{
    WriteLock(get_cidfd_lock());
    return tgg_hash_del_key(g_cid_hash, gid, free_ciddata);
}

int tgg_get_cid(char* cid);
{
    ReadLock(get_cidfd_lock());
    int* value = (int*)tgg_hash_get_key(g_cid_hash, cid);
    if(!value) {
        return -1;
    }
    return *value;
}

int tgg_add_uidgid(char* uid, char* gid)
{
    WriteLock(get_uidgid_lock());
    int ret = rte_hash_lookup(hash, uid);
    tgg_gid_list* pdata = NULL;
    if (ret < 0) {// 首次插入
        RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed,hash key not exist:%d", __func__, __LINE__, key, ret);
        pdata = dpdk_rte_malloc(sizeof(tgg_gid_list));
        if(!pdata) {
            return -1;
        }
        pdata->data = NULL;
        pdata->next = dpdk_rte_malloc(sizeof(tgg_gid_list));
        if(!pdata->next) {
            rte_free(pdata);
            return -1;
        }
        strncpy(pdata->next->data, gid, strlen(gid));
        pdata->next->next = NULL;
        int ret = rte_hash_add_key_data(hash, uid, pdata);
        if (ret < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d]add key[%s] failed:%d.", __func__, __LINE__, key, ret);
            memset(pdata->next, 0 ,sizeof(tgg_gid_list));
            rte_free(pdata->next);
            memset(pdata, 0 ,sizeof(tgg_gid_list));
            rte_free(pdata);
            return ret;
        }
    } else {// 已存在节点
        tgg_gid_list* value = NULL;
        ret = rte_hash_lookup_data(hash, uid, &value);
        if (ret < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed:%d", __func__, __LINE__, key, ret);
            return -1;
        }
        tgg_gid_list* tmp = dpdk_rte_malloc(sizeof(tgg_gid_list));
        if(!tmp) {
            return -1;
        }
        memcpy(tmp->data, gid, strlen(gid));
        tmp->next = NULL;
        pdata = value->next;
        while (pdata->next) {
            pdata = pdata->next;
        }
        pdata->next = tmp;
    }

    return 0;

}

// 删掉hash<uid,list<gid>>中的一整个uid
int tgg_del_uid_uidgid(const char* uid)
{
    WriteLock(get_uidgid_lock());
    if(!uid || !strcmp(uid,"")) {
        return 0;
    }
    return tgg_hash_del_key(g_giduid_hash, uid, iter_del_idlist);
}

int tgg_del_gid_uidgid(const char* uid, const char* gid)
{
    WriteLock(get_uidgid_lock());
    if(!gid || !strcmp(gid,"")) {
        return 0;
    }
    return tgg_hash_del_idlst4key(g_giduid_hash, uid, gid);
}

std::list<std::string> tgg_get_uidsbygid(char* gid)
{
    ReadLock(get_uidgid_lock());
    tgg_gid_list* value = (tgg_gid_list*)tgg_hash_get_key(g_giduid_hash, gid);
    if(!value) {
        return lst_fd;
    }
    while (value->next) {
        lst_fd.append(std::string(value->next->data));
    }
    return lst_fd;
}

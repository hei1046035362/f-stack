#include <rte_hash.h>
#include <rte_malloc.h>
#include <rte_hash_crc.h>
#include "tgg_common.h"
#include "tgg_bw_cache.h"
#include "tgg_lock.h"
#include "comm/TggLock.hpp"

extern const struct rte_hash *g_gid_hash;
extern const struct rte_hash *g_uid_hash;
extern const struct rte_hash *g_cid_hash;
extern const struct rte_hash *g_uidgid_hash;
extern const struct rte_hash *g_idx_hash;


typedef void (*tgg_add_data)(void*);

typedef void (*tgg_free_id_data)(void*);

void iter_del_fdlist(void* iddata)
{
    iter_del_list<tgg_fd_list>((tgg_fd_list*)iddata);
}

void iter_del_idlist(void* iddata)
{
    iter_del_list<tgg_list_id>((tgg_list_id*)iddata);
}

// 针对key-value的hash
static int tgg_hash_add_key_value(const rte_hash* hash, const char* key, int fd)
{
    if (strlen(key) != 20) {
        RTE_LOG(ERR, USER1, "[%s][%d]add key failed,check if key[%s] is correct.", __func__, __LINE__, key);
        return -EINVAL; 
    }
    if (!key || strlen(key) <= 0) {
        RTE_LOG(ERR, USER1, "[%s][%d]add key[%s] failed, invalid data.", __func__, __LINE__, key);
        return -EINVAL; 
    }
    int* value = NULL;
    int ret = rte_hash_lookup_with_hash(hash, key, rte_hash_crc(key, strlen(key), 0));
    if (ret < 0) {
        value = (int*)dpdk_rte_malloc(sizeof(int));
        if(!value) {
            RTE_LOG(ERR, USER1, "[%s][%d]add key[%s] failed:%d.", __func__, __LINE__, key, ret);
            return -1;
        }
        *value = fd;
        int ret = rte_hash_add_key_with_hash_data(hash, key, rte_hash_crc(key, strlen(key), 0), value);
        if (ret < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d]add key[%s] failed:%d.", __func__, __LINE__, key, ret);
            rte_free(value);
            return ret;
        }
    } else {
        ret = rte_hash_lookup_with_hash_data(hash, key, rte_hash_crc(key, strlen(key), 0), (void**)&value);
        if (ret < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed:%d", __func__, __LINE__, key, ret);
            return -1;
        }
        *value = fd;
        // int ret = rte_hash_add_key_data(hash, key, value);
        // if (ret < 0) {
        //     *value = 0;
        //     RTE_LOG(ERR, USER1, "[%s][%d]add key[%s] failed:%d.", __func__, __LINE__, key, ret);
        //     return ret;
        // }
    }

    return 0;
}

// 针对key-list的hash
static int tgg_hash_add_keywithfdlst(const rte_hash* hash, const char* key, int fd, int idx)
{
    int ret = rte_hash_lookup_with_hash(hash, key, rte_hash_crc(key, strlen(key), 0));
    tgg_fd_list* pdata = NULL;
    if (ret < 0) {// 首次插入
        RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed,hash key not exist:%d", __func__, __LINE__, key, ret);
        pdata = (tgg_fd_list*)dpdk_rte_malloc(sizeof(tgg_fd_list));
        if(!pdata) {
            return -1;
        }
        pdata->next = (tgg_fd_list*)dpdk_rte_malloc(sizeof(tgg_fd_list));
        if(!pdata->next) {
            rte_free(pdata);
            return -1;
        }
        pdata->next->idx = idx;
        pdata->next->fd = fd;
        pdata->next->next = NULL;
        int ret = rte_hash_add_key_with_hash_data(hash, key, rte_hash_crc(key, strlen(key), 0), pdata);
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
        ret = rte_hash_lookup_with_hash_data(hash, key, rte_hash_crc(key, strlen(key), 0), (void**)&value);
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
        tgg_fd_list* tmp = (tgg_fd_list*)dpdk_rte_malloc(sizeof(tgg_fd_list));
        if(!tmp) {
            return -1;
        }
        tmp->idx = idx;
        tmp->fd = fd;
        tmp->next = NULL;
        pdata->next = tmp;
    }
    return 0;
}

// 获取hash value/list
static void* tgg_hash_get_key(const rte_hash* hash, const char* key)
{
    int ret = rte_hash_lookup_with_hash(hash, key, rte_hash_crc(key, strlen(key), 0));
    if (ret < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed,hash key not exist:%d", __func__, __LINE__, key, ret);
        return NULL;
    }
    void* pdata = NULL;
    ret = rte_hash_lookup_with_hash_data(hash, key, rte_hash_crc(key, strlen(key), 0), &pdata);
    if (ret < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed:%d", __func__, __LINE__, key, ret);
        return NULL;
    }
    return pdata;
}

// 删除整个key
static int tgg_hash_del_key(const rte_hash* hash, const char* key, tgg_free_id_data fp)
{
    tgg_gid_data* pdata = (tgg_gid_data*)tgg_hash_get_key(hash, key);
    if (!pdata)
        return -EINVAL;

    // 释放value的空间
    fp((void*)pdata);

    int ret = rte_hash_del_key_with_hash(hash, key, rte_hash_crc(key, strlen(key), 0));
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
    int ret = rte_hash_lookup_with_hash(hash, key, rte_hash_crc(key, strlen(key), 0));
    if (ret < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed,hash key not exist:%d", __func__, __LINE__, key, ret);
        return -1;
    }
    tgg_fd_list* value = NULL;
    ret = rte_hash_lookup_with_hash_data(hash, key, rte_hash_crc(key, strlen(key), 0), (void**)&value);
    if (ret < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed:%d", __func__, __LINE__, key, ret);
        return -1;
    }
    tgg_fd_list* pdata = value->next;
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
static int tgg_hash_del_idlst4key(const rte_hash* hash, const char* key, const char* id)
{
    int ret = rte_hash_lookup_with_hash(hash, key, rte_hash_crc(key, strlen(key), 0));
    if (ret < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed,hash key not exist:%d", __func__, __LINE__, key, ret);
        return -1;
    }
    tgg_list_id* value = NULL;
    ret = rte_hash_lookup_with_hash_data(hash, key, rte_hash_crc(key, strlen(key), 0), (void**)&value);
    if (ret < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed:%d", __func__, __LINE__, key, ret);
        return -1;
    }
    tgg_list_id* pdata = value->next;
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
int tgg_add_gid(const char* gid, int fd, int idx)
{
    WriteLock lock(get_gidfd_lock());
    return tgg_hash_add_keywithfdlst(g_gid_hash, gid, fd, idx);
}

std::list<std::string> tgg_get_gid(const char* gid)
{
    ReadLock lock(get_gidfd_lock());
    std::list<std::string> lst_fd;
    tgg_gid_data* value = (tgg_gid_data*)tgg_hash_get_key(g_gid_hash, gid);
    if(!value) {
        return lst_fd;
    }
    while (value->next) {
        std::string item = std::to_string(value->next->fd) + 
                            std::string(":") + 
                            std::to_string(value->next->idx);
        lst_fd.push_back(item);
    }
    return lst_fd;
}

int tgg_del_gid(const char* gid)
{
    WriteLock lock(get_gidfd_lock());
    return tgg_hash_del_key(g_gid_hash, gid, iter_del_fdlist);
}

int tgg_del_fd4gid(const char* gid, int fd, int idx)
{
    WriteLock lock(get_gidfd_lock());
    return tgg_hash_del_fdlst4key(g_gid_hash, gid, fd, idx);
}

/// 增删查  uid 用户id 
int tgg_add_uid(const char* uid, int fd, int idx)
{
    WriteLock lock(get_uidfd_lock());
    return tgg_hash_add_keywithfdlst(g_uid_hash, uid, fd, idx);
}

int tgg_del_uid(const char* uid)
{
    WriteLock lock(get_uidfd_lock());
    return tgg_hash_del_key(g_uid_hash, uid, iter_del_fdlist);
}

int tgg_del_fd4uid(const char* uid, int fd, int idx)
{
    WriteLock lock(get_uidfd_lock());
    return tgg_hash_del_fdlst4key(g_uid_hash, uid, fd, idx);
}

std::list<std::string> tgg_get_uid(const char* uid)
{
    ReadLock lock(get_uidfd_lock());
    std::list<std::string> lst_fd;
    tgg_uid_data* value = (tgg_uid_data*)tgg_hash_get_key(g_uid_hash, uid);
    if(!value) {
        return lst_fd;
    }
    while (value->next) {
        std::string item = std::to_string(value->next->fd) + 
                            std::string(":") + 
                            std::to_string(value->next->idx);
        lst_fd.push_back(item);
    }
    return lst_fd;
}

/// 增删查  cid
int tgg_add_cid(const char* cid, int fd)
{
    WriteLock lock(get_cidfd_lock());
    return tgg_hash_add_key_value(g_cid_hash, cid, fd);
}

static void free_ciddata(void* data)
{
    int* pdata = (int*)data;
    *pdata = 0;
    rte_free(pdata);
}

int tgg_del_cid(const char* cid)
{
    WriteLock lock(get_cidfd_lock());
    return tgg_hash_del_key(g_cid_hash, cid, free_ciddata);
}

int tgg_get_fdbycid(const char* cid)
{
    ReadLock lock(get_cidfd_lock());
    int* value = (int*)tgg_hash_get_key(g_cid_hash, cid);
    if(!value) {
        return -1;
    }
    return *value;
}

int tgg_add_uidgid(const char* uid, const char* gid)
{
    int ret = rte_hash_lookup_with_hash(g_uidgid_hash, uid, rte_hash_crc(uid, strlen(uid), 0));
    tgg_gid_list* pdata = NULL;
    if (ret < 0) {// 首次插入
        RTE_LOG(ERR, USER1, "[%s][%d]Get hash key uid[%s] data failed,hash key not exist:%d", __func__, __LINE__, uid, ret);
        pdata = (tgg_gid_list*)dpdk_rte_malloc(sizeof(tgg_gid_list));
        if(!pdata) {
            return -1;
        }
        memset(pdata->data, 0, sizeof(pdata->data));
        pdata->next = (tgg_gid_list*)dpdk_rte_malloc(sizeof(tgg_gid_list));
        if(!pdata->next) {
            rte_free(pdata);
            return -1;
        }
        strncpy(pdata->next->data, gid, strlen(gid));
        pdata->next->next = NULL;
        WriteLock lock(get_uidgid_lock());
        int ret = rte_hash_add_key_with_hash_data(g_uidgid_hash, uid, rte_hash_crc(uid, strlen(uid), 0), pdata);
        if (ret < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d]add key[%s] failed:%d.", __func__, __LINE__, uid, ret);
            memset(pdata->next, 0 ,sizeof(tgg_gid_list));
            rte_free(pdata->next);
            memset(pdata, 0 ,sizeof(tgg_gid_list));
            rte_free(pdata);
            return ret;
        }
    } else {// 已存在节点
        tgg_gid_list* value = NULL;
        WriteLock lock(get_uidgid_lock());
        ret = rte_hash_lookup_with_hash_data(g_uidgid_hash, uid, rte_hash_crc(uid, strlen(uid), 0), (void**)&value);
        if (ret < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed:%d", __func__, __LINE__, uid, ret);
            return -1;
        }
        tgg_gid_list* tmp = (tgg_gid_list*)dpdk_rte_malloc(sizeof(tgg_gid_list));
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
    WriteLock lock(get_uidgid_lock());
    if(!uid || !strcmp(uid,"")) {
        return 0;
    }
    return tgg_hash_del_key(g_uidgid_hash, uid, iter_del_idlist);
}

int tgg_del_gid_uidgid(const char* uid, const char* gid)
{
    WriteLock lock(get_uidgid_lock());
    if(!gid || !strcmp(gid,"")) {
        return 0;
    }
    return tgg_hash_del_idlst4key(g_uidgid_hash, uid, gid);
}

std::list<std::string> tgg_get_gidsbyuid(const char* uid)
{
    std::list<std::string> lst_fd;
    ReadLock lock(get_uidgid_lock());
    tgg_gid_list* value = (tgg_gid_list*)tgg_hash_get_key(g_uidgid_hash, uid);
    if(!value) {
        return lst_fd;
    }
    while (value->next) {
        lst_fd.push_back(std::string(value->next->data));
    }
    return lst_fd;
}

void tgg_iterprint_gidsbyuid()
{
    std::list<std::string> lst_fd;
    ReadLock lock(get_uidgid_lock());
    char* key = NULL;
    tgg_gid_list* value = NULL;
    uint32_t next = 0;
    int ret = 0;
    while (1) {
        ret = rte_hash_iterate(g_uidgid_hash, (const void**)&key, (void**)&value, &next);
        if (-ENOENT == ret) {
            printf("iter to the end.\n");
            break;
        }
        else if (ret < 0) {
            printf("catch an error\n");
            break;
        }
        if(!value) {
            printf("key[%s]'s value is empty\n", key);
            break;
        }
        printf("uid:%s\n", key);
        tgg_gid_list* tmp = value->next;
        while (tmp) {
            lst_fd.push_back(std::string(tmp->data));
            printf("gid:%s\n", tmp->data);
            tmp = tmp->next;
        }
    }
}

int tgg_add_idx(int idx)
{
    int ret = rte_hash_lookup_with_hash(g_idx_hash, &idx, rte_hash_crc(&idx, sizeof(int), 0));
    if (ret < 0) {// 首次插入
        return rte_hash_add_key_with_hash(g_idx_hash, &idx, rte_hash_crc(&idx, sizeof(int), 0));
    }
    RTE_LOG(ERR, USER1, "[%s][%d] idx: %d already exist.", __func__, __LINE__, idx);
    return -1;
}
int tgg_del_idx(int idx)
{
    int ret = rte_hash_del_key_with_hash(g_idx_hash, &idx, rte_hash_crc(&idx, sizeof(idx), 0));
    if (ret > 0) {
        // 在并发情况下删除key之后，位置还在，需要删除位置信息，详情参考函数说明
        if (rte_hash_free_key_with_position(g_idx_hash, ret) < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d]Del idx[%d] pos failed:%d.\n", __func__, __LINE__, idx, ret);
            return -EINVAL;
        }
    } else {
        RTE_LOG(ERR, USER1, "[%s][%d]Del idx[%d] data failed:%d.\n", __func__, __LINE__, idx, ret);
        return -EINVAL;
    }
    return 0;
}
int tgg_check_idx_exist(int idx)
{
    return rte_hash_del_key_with_hash(g_idx_hash, &idx, rte_hash_crc(&idx, sizeof(idx), 0));
}

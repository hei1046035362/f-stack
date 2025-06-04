#include <rte_hash.h>
#include <rte_malloc.h>
#include <rte_hash_crc.h>
#include "tgg_common.h"
#include "tgg_bw_cache.h"
#include "tgg_lock.h"
#include "comm/TggLock.hpp"
#include "comm/log.hpp"

extern const struct rte_hash *g_gid_hash;
extern const struct rte_hash *g_uid_hash;
extern const struct rte_hash *g_cid_hash;
extern const struct rte_hash *g_cidgid_hash;
extern const struct rte_hash *g_idx_hash;
extern const struct rte_hash *g_bwfdx_hash;
extern const struct rte_hash *g_bwwkkey_hash;


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

#if 0
// key为 const char*
// 针对key-value的hash
static int tgg_hash_add_key_value(const rte_hash* hash, const char* key, int fdid)
{
    if (strlen(key) != 20) {
        RTE_LOG(ERR, USER1, "[%s][%d]add key failed,check if key[%s] is correct.\n", __FILE__, __LINE__, key);
        return -EINVAL; 
    }
    if (!key || strlen(key) <= 0) {
        RTE_LOG(ERR, USER1, "[%s][%d]add key[%s] failed, invalid data.\n", __FILE__, __LINE__, key);
        return -EINVAL; 
    }
    int* value = NULL;
    int ret = rte_hash_lookup_with_hash(hash, key, rte_hash_crc(key, strlen(key), 0));
    if (ret < 0) {
        value = (int*)dpdk_rte_malloc(sizeof(int));
        if(!value) {
            RTE_LOG(ERR, USER1, "[%s][%d]add key[%s] failed:%d.\n", __FILE__, __LINE__, key, ret);
            return -1;
        }
        *value = fdid;
        int ret = rte_hash_add_key_with_hash_data(hash, key, rte_hash_crc(key, strlen(key), 0), value);
        if (ret < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d]add key[%s] failed:%d.\n", __FILE__, __LINE__, key, ret);
            rte_free(value);
            return ret;
        }
    } else {
        ret = rte_hash_lookup_with_hash_data(hash, key, rte_hash_crc(key, strlen(key), 0), (void**)&value);
        if (ret < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed:%d\n", __FILE__, __LINE__, key, ret);
            return -1;
        }
        *value = fdid;
    }

    return 0;
}
#endif

// 针对key-list的hash
static int tgg_hash_add_keywithfdlst(const rte_hash* hash, const char* key, int key_len, int fdid)
{
    int ret = rte_hash_lookup_with_hash(hash, key, rte_hash_crc(key, key_len, 0));
    tgg_fd_list* pdata = NULL;
    if (ret < 0) {// 首次插入
        // RTE_LOG(INFO, USER1, "[%s][%d]Get key[%s] data failed,hash key not exist:%d\n", __FILE__, __LINE__, key, ret);
        pdata = (tgg_fd_list*)dpdk_rte_malloc(sizeof(tgg_fd_list));
        if(!pdata) {
            return -1;
        }
        pdata->next = (tgg_fd_list*)dpdk_rte_malloc(sizeof(tgg_fd_list));
        if(!pdata->next) {
            rte_free(pdata);
            return -1;
        }
        pdata->next->fdid = fdid;
        pdata->next->next = NULL;
        int ret = rte_hash_add_key_with_hash_data(hash, key, rte_hash_crc(key, key_len, 0), pdata);
        if (ret < 0) {
            LOG_ERROR("add key[%s] failed:%d.", key, ret);
            memset(pdata->next, 0 ,sizeof(tgg_fd_list));
            rte_free(pdata->next);
            memset(pdata, 0 ,sizeof(tgg_fd_list));
            rte_free(pdata);
            return ret;
        }
    } else {// 已存在节点
        // TODO 要加进程锁
        tgg_fd_list* value = NULL;
        ret = rte_hash_lookup_with_hash_data(hash, key, rte_hash_crc(key, key_len, 0), (void**)&value);
        if (ret < 0) {
            LOG_ERROR("Get key[%s] data failed:%d", key, ret);
            return -1;
        }
        pdata = value->next;
        while (pdata->next) {
            // TODO 对于已存在的fd+idx是否要比较，可能会有性能损耗
            if(pdata->next->fdid == fdid) {
                LOG_WARNING("Duplicate key[%s] found.", key);
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
        tmp->fdid = fdid;
        tmp->next = NULL;
        pdata->next = tmp;
    }
    return 0;
}

// 获取hash value/list
static void* tgg_hash_get_value(const rte_hash* hash, const char* key, int key_len)
{
    // int ret = rte_hash_lookup_with_hash(hash, key, rte_hash_crc(key, key_len, 0));
    // if (ret < 0) {
    //     RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed,hash key not exist:%d\n", __FILE__, __LINE__, key, ret);
    //     return NULL;
    // }
    void* pdata = NULL;
    int ret = rte_hash_lookup_with_hash_data(hash, key, rte_hash_crc(key, key_len, 0), &pdata);
    if (ret < 0) {
        LOG_ERROR("Get key[%s] data failed:%d", key, ret);
        return NULL;
    }
    return pdata;
}

// 删除整个key
static int tgg_hash_del_key(const rte_hash* hash, const char* key, int key_len, tgg_free_id_data fp)
{
    tgg_gid_data* pdata = (tgg_gid_data*)tgg_hash_get_value(hash, key, key_len);
    if (!pdata)
        return -EINVAL;

    // 释放value的空间
    fp((void*)pdata);

    int ret = rte_hash_del_key_with_hash(hash, key, rte_hash_crc(key, key_len, 0));
    if (ret < 0) {
    //     // 在并发情况下删除key之后，位置还在，需要删除位置信息，详情参考函数说明
    //     if (rte_hash_free_key_with_position(hash, ret) < 0) {
    //         RTE_LOG(ERR, USER1, "[%s][%d]Del key[%s] pos failed:%d\n", __FILE__, __LINE__, key, ret);
    //         return -EINVAL;
    //     }
    // } else {
        LOG_ERROR("Del key[%s] data failed:%d", key, ret);
        return -EINVAL;
    }
    return 0;
}

// 删除hash value为list中的单个元素,list节点中的值为fd和idx两个元素
static int tgg_hash_del_fdlst4key(const rte_hash* hash, const char* key, int key_len, int fdid)
{
    // int ret = rte_hash_lookup_with_hash(hash, key, rte_hash_crc(key, key_len, 0));
    // if (ret < 0) {
    //     RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed,hash key not exist:%d\n", __FILE__, __LINE__, key, ret);
    //     return -1;
    // }
    tgg_fd_list* value = NULL;
    int ret = rte_hash_lookup_with_hash_data(hash, key, rte_hash_crc(key, key_len, 0), (void**)&value);
    if (ret < 0 || !value) {
        LOG_ERROR("Get key[%s] data failed:%d value:%p", key, ret, value);
        return -1;
    }
    tgg_fd_list* pdata = value;
    while (pdata->next) {
        // TODO 对于已存在的fd+idx是否要比较，可能会有性能损耗
        if(pdata->next->fdid == fdid) {
            tgg_fd_list* tmp = pdata->next->next;
            memset(pdata->next, 0, sizeof(tgg_fd_list));
            rte_free(pdata->next);
            pdata->next = tmp;
            break;
        } else {
            pdata = pdata->next;
        }
    }
    if(!value->next) {
        // 没有元素了，就把key也删除
        // TODO 有没有更好的方式，不用重复创建相同的key
        memset(value, 0, sizeof(tgg_fd_list));
        rte_free(value);
        rte_hash_del_key_with_hash(hash, key, rte_hash_crc(key, key_len, 0));
    }

    return 0;
}

static int tgg_hash_get_allkeys(const rte_hash* hash, std::list<std::string>& lst_items)
{
    char* key = NULL;
    int* value = NULL;
    uint32_t next = 0;
    int ret = 0;
    while (1) {
        ret = rte_hash_iterate(hash, (const void**)&key, (void**)&value, &next);
        if (-ENOENT == ret) {
            LOG_ERROR("iter to the end.");
            break;
        } else if (ret < 0) {
            LOG_ERROR("catch an error");
            return -1;
        }
        lst_items.push_back(key);
    }
    return 0;
}


// key 为int

// 获取hash value/list
static void* tgg_hash_get_intkey_value(const rte_hash* hash, int key)
{
    int ret = rte_hash_lookup_with_hash(hash, &key, rte_hash_crc(&key, sizeof(int), 0));
    if (ret < 0) {
        LOG_ERROR("Get key[%d] data failed,hash key not exist:%d", key, ret);
        return NULL;
    }
    void* pdata = NULL;
    ret = rte_hash_lookup_with_hash_data(hash, &key, rte_hash_crc(&key, sizeof(int), 0), &pdata);
    if (ret < 0) {
        LOG_ERROR("Get key[%d] data failed:%d", key, ret);
        return NULL;
    }
    return pdata;
}

// 删除整个key
static int tgg_hash_del_intkey(const rte_hash* hash, int key, tgg_free_id_data fp)
{
    tgg_gid_data* pdata = (tgg_gid_data*)tgg_hash_get_intkey_value(hash, key);
    if (!pdata)
        return -EINVAL;

    // 释放value的空间
    fp((void*)pdata);

    int ret = rte_hash_del_key_with_hash(hash, &key, rte_hash_crc(&key, sizeof(int), 0));
    if (ret < 0) {
    //     // 在并发情况下删除key之后，位置还在，需要删除位置信息，详情参考函数说明
    //     if (rte_hash_free_key_with_position(hash, ret) < 0) {
    //         RTE_LOG(ERR, USER1, "[%s][%d]Del key[%d] pos failed:%d\n", __FILE__, __LINE__, key, ret);
    //         return -EINVAL;
    //     }
    // } else {
        LOG_ERROR("Del key[%d] data failed:%d", key, ret);
        return -EINVAL;
    }
    return 0;
}

// 删除hash value为list中的单个元素,list节点中的值为char[]
static int tgg_hash_del_idlst4intkey(const rte_hash* hash, int key, const char* id)
{
    int ret = rte_hash_lookup_with_hash(hash, &key, rte_hash_crc(&key, sizeof(int), 0));
    if (ret < 0) {
        LOG_ERROR("Get key[%d] data failed,hash key not exist:%d", key, ret);
        return -1;
    }
    tgg_list_id* value = NULL;
    ret = rte_hash_lookup_with_hash_data(hash, &key, rte_hash_crc(&key, sizeof(int), 0), (void**)&value);
    if (ret < 0) {
        LOG_ERROR("Get key[%d] data failed:%d", key, ret);
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
    if(!pdata->next) {
        // 没有元素了，就把key也删除
        // TODO 有没有更好的方式，不用重复创建相同的key
        memset(pdata, 0, sizeof(tgg_list_id));
        rte_free(pdata);
        rte_hash_del_key_with_hash(hash, &key, rte_hash_crc(&key, sizeof(int), 0));
    }
    return 0;
}


static int tgg_hash_get_all_intkeys(const rte_hash* hash, std::list<int>& lst_items)
{
    int* key = NULL;
    int* value = NULL;
    uint32_t next = 0;
    int ret = 0;
    while (1) {
        ret = rte_hash_iterate(hash, (const void**)&key, (void**)&value, &next);
        if (-ENOENT == ret) {
            LOG_ERROR("iter to the end.");
            break;
        } else if (ret < 0) {
            LOG_ERROR("catch an error");
            return -1;
        }
        lst_items.push_back(*key);
    }
    return 0;
}




#define APROPRIAT_HASH_KEY(key, len)\
    char _key[len] = {0};\
    memcpy(_key, key, strlen(key))

/// 增删查  gid
int tgg_add_gid(const char* gid, int fdid)
{
    LOG_DEBUG("add gid[%s] fdid[%d].", gid, fdid);
    APROPRIAT_HASH_KEY(gid, TGG_GID_LEN);
    WriteLock lock(get_gidfd_lock());
    return tgg_hash_add_keywithfdlst(g_gid_hash, _key, TGG_GID_LEN, fdid);
}

int tgg_get_fdsbygid(const char* gid, std::list<int>& lst_fd)
{
    APROPRIAT_HASH_KEY(gid, TGG_GID_LEN);
    ReadLock lock(get_gidfd_lock());
    tgg_gid_data* value = (tgg_gid_data*)tgg_hash_get_value(g_gid_hash, _key, TGG_GID_LEN);
    if(!value) {
        return -1;
    }
    while (value->next) {
        lst_fd.push_back(value->next->fdid);
        value = value->next;
    }
    return 0;
}

int tgg_del_gid(const char* gid)
{
    LOG_DEBUG("iter del fdid for gid[%s].", gid);
    APROPRIAT_HASH_KEY(gid, TGG_GID_LEN);
    WriteLock lock(get_gidfd_lock());
    return tgg_hash_del_key(g_gid_hash, _key, TGG_GID_LEN, iter_del_fdlist);
}

int tgg_del_fd4gid(const char* gid, int fdid)
{
    LOG_DEBUG("del fdid[%d] for gid[%s].", fdid, gid);
    APROPRIAT_HASH_KEY(gid, TGG_GID_LEN);
    WriteLock lock(get_gidfd_lock());
    return tgg_hash_del_fdlst4key(g_gid_hash, _key, TGG_GID_LEN, fdid);
}

int tgg_get_allonlinegids(std::list<std::string>& lst_gid)
{
    WriteLock lock(get_gidfd_lock());
    return tgg_hash_get_allkeys(g_gid_hash, lst_gid);
}

/// 增删查  uid 用户id 
int tgg_add_uid(const char* uid, int fdid)
{
    LOG_DEBUG("add uid[%s] fdid[%d].", uid, fdid);
    APROPRIAT_HASH_KEY(uid, TGG_UID_LEN);
    WriteLock lock(get_uidfd_lock());
    return tgg_hash_add_keywithfdlst(g_uid_hash, _key, TGG_UID_LEN, fdid);
}

int tgg_del_uid(const char* uid)
{
    LOG_DEBUG("iter del fdid for uid[%s].", uid);
    APROPRIAT_HASH_KEY(uid, TGG_UID_LEN);
    WriteLock lock(get_uidfd_lock());
    return tgg_hash_del_key(g_uid_hash, _key, TGG_UID_LEN, iter_del_fdlist);
}

int tgg_del_fd4uid(const char* uid, int fdid)
{
    LOG_DEBUG("del fdid[%d] for uid[%s].", fdid, uid);
    APROPRIAT_HASH_KEY(uid, TGG_UID_LEN);
    WriteLock lock(get_uidfd_lock());
    return tgg_hash_del_fdlst4key(g_uid_hash, _key, TGG_UID_LEN, fdid);
}

int tgg_get_fdsbyuid(const char* uid, std::list<int>& lst_fd)
{
    APROPRIAT_HASH_KEY(uid, TGG_UID_LEN);
    ReadLock lock(get_uidfd_lock());
    tgg_uid_data* value = (tgg_uid_data*)tgg_hash_get_value(g_uid_hash, _key, TGG_UID_LEN);
    if(!value) {
        return -1;
    }
    while (value->next) {
        lst_fd.push_back(value->next->fdid);
        value = value->next;
    }
    return 0;
}

/// 增删查  cid
int tgg_add_cid(int cid, int fdid)
{
    LOG_DEBUG("add cid[%d] fdid[%d].", cid, fdid);
    WriteLock lock(get_cidfd_lock());
    int* value = (int*)dpdk_rte_malloc(sizeof(int));
    if(!value) {
        LOG_ERROR("[%s][%d]add key[%d] failed.", cid);
        return -1;
    }
    *value = fdid;
    return rte_hash_add_key_with_hash_data(g_cid_hash, &cid, rte_hash_crc(&cid, sizeof(int), 0), value);

}

static void free_ciddata(void* data)
{
    int* pdata = (int*)data;
    *pdata = 0;
    rte_free(pdata);
}

int tgg_del_cid(int cid)
{
    LOG_DEBUG("del fdid for cid[%d].", cid);
    WriteLock lock(get_cidfd_lock());
    return tgg_hash_del_intkey(g_cid_hash, cid, free_ciddata);
}

int tgg_get_fdbycid(int cid)
{
    ReadLock lock(get_cidfd_lock());
    int* value = (int*)tgg_hash_get_intkey_value(g_cid_hash, cid);
    if(!value) {
        return -1;
    }
    return *value;
}

int tgg_get_allonlinecids(std::list<int>& lst_cids)
{
    WriteLock lock(get_cidfd_lock());
    return tgg_hash_get_all_intkeys(g_cid_hash, lst_cids);
}

// 删除某个进程的所有pid，单个进程异常退出时调用
void tgg_clean_allcids_bypid(int prc_id)
{
    char* key = NULL;
    int* value = NULL;
    uint32_t next = 0;
    int ret = 0;
    WriteLock lock(get_cidfd_lock());
    while (1) {
        ret = rte_hash_iterate(g_cid_hash, (const void**)&key, (void**)&value, &next);
        if (-ENOENT == ret) {
            LOG_DEBUG("iter to the end.");
            break;
        } else if (ret < 0) {
            LOG_ERROR("catch an error");
            return;
        }
        if(*key >> 24 == prc_id) {
            free_ciddata(value);
            rte_hash_del_key(g_cid_hash, key);
        }
    }
    return ;
}

int tgg_get_allfds(std::list<int>& lst_fds)
{
    int* key = NULL;
    int* value = NULL;
    uint32_t next = 0;
    int ret = 0;
    while (1) {
        WriteLock lock(get_cidfd_lock());
        ret = rte_hash_iterate(g_cid_hash, (const void**)&key, (void**)&value, &next);
        if (-ENOENT == ret) {
            LOG_DEBUG("iter to the end.");
            break;
        } else if (ret < 0) {
            LOG_ERROR("catch an error");
            return -1;
        }
        if(!value) {
            continue;
        }
        lst_fds.push_back(*value);
    }
    return 0;
}


int tgg_add_cidgid(int cid, const char* gid)
{
    LOG_DEBUG("add gid[%s] for cid[%d].", gid, cid);
    // APROPRIAT_HASH_KEY(uid, TGG_UID_LEN);
    int ret = rte_hash_lookup_with_hash(g_cidgid_hash, &cid, rte_hash_crc(&cid, sizeof(int), 0));
    tgg_gid_list* pdata = NULL;
    if (ret < 0) {// 首次插入
        // RTE_LOG(ERR, USER1, "[%s][%d]Get hash key uid[%s] data failed,hash key not exist:%d\n", __FILE__, __LINE__, uid, ret);
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
        WriteLock lock(get_cidgid_lock());
        int ret = rte_hash_add_key_with_hash_data(g_cidgid_hash, &cid, rte_hash_crc(&cid, sizeof(int), 0), pdata);
        if (ret < 0) {
            LOG_ERROR("add cid[%d] failed:%d.", cid, ret);
            memset(pdata->next, 0 ,sizeof(tgg_gid_list));
            rte_free(pdata->next);
            memset(pdata, 0 ,sizeof(tgg_gid_list));
            rte_free(pdata);
            return ret;
        }
    } else {// 已存在节点
        tgg_gid_list* value = NULL;
        WriteLock lock(get_cidgid_lock());
        ret = rte_hash_lookup_with_hash_data(g_cidgid_hash, &cid, rte_hash_crc(&cid, sizeof(int), 0), (void**)&value);
        if (ret < 0) {
            LOG_ERROR("Get cid[%d] data failed:%d", cid, ret);
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

int tgg_get_gidsbycid(int cid, std::list<std::string>& lst_gid)
{
    // APROPRIAT_HASH_KEY(uid, TGG_UID_LEN);
    ReadLock lock(get_cidgid_lock());
    tgg_gid_list* value = (tgg_gid_list*)tgg_hash_get_intkey_value(g_cidgid_hash, cid);
    if(!value) {
        LOG_ERROR("cid [%d] not exist in gid hash.", cid);
        return -1;
    }
    while (value->next) {
        lst_gid.push_back(std::string(value->next->data));
        value = value->next;
    }
    return 0;
}

// 删掉hash<cid,list<gid>>中的一整个cid
int tgg_del_cid_cidgid(int cid)
{
    LOG_DEBUG("del all gids for cid[%d].", cid);
    WriteLock lock(get_cidgid_lock());
    if(cid <= 0) {
        LOG_ERROR("del all gids for cid[%d] failed.", cid);        
        return -1;
    }
    return tgg_hash_del_intkey(g_cidgid_hash, cid, iter_del_idlist);
}

int tgg_del_gid_cidgid(int cid, const char* gid)
{
    LOG_DEBUG("del gid[%s] for cid[%d].", gid, cid);
    WriteLock lock(get_cidgid_lock());
    if(cid <= 0) {
        LOG_ERROR("del gid[%s] for cid[%d] failed.", gid, cid);        
        return -1;
    }
    return tgg_hash_del_idlst4intkey(g_cidgid_hash, cid, gid);
}

int tgg_get_gidsbyuid(const char* uid, std::set<std::string>& set_gid)
{
    std::list<int> lstFds;
    tgg_get_fdsbyuid(uid, lstFds);// 通过uid找到cid列表
    std::list<int>::iterator it = lstFds.begin();
    while(it != lstFds.end()) {
        int cid = tgg_get_cli_cid(*it & 0xf, *it >> 8);
        std::list<std::string> lstGids;
        tgg_get_gidsbycid(cid, lstGids);// 通过cid找到gid列表
        set_gid.insert(std::make_move_iterator(lstGids.begin()), 
             std::make_move_iterator(lstGids.end()));
        it++;
    }
    return 0;
}

void tgg_del_gid_cidgid(const char* gid)
{
    std::list<int> fds;
    tgg_get_fdsbygid(gid, fds);
    // 这里没有复用tgg_get_fdsbygid中的循环是为了减少加锁的时间
    std::list<int>::iterator it = fds.begin();
    while (it != fds.end()) {
        int cid = tgg_get_cli_cid(*it & 0xf, *it >> 8);
        if(cid <= 0) {
            // TODO 调试+兜底:防止连接已关闭但是gid hash中的fd还在，出现这个日志，说明释放逻辑依然存在问题
            LOG_ERROR("Del gid[%s] for cidgid failed: cid%d is not avaliable.", gid, cid);            
            tgg_del_fd4gid(gid, *it);
            continue;
        }
        tgg_del_gid_cidgid(cid, gid);
        it++;
    }
}

void tgg_iterprint_gidsbyuid(const char* uid)
{
    std::list<std::string> lst_fd;
    ReadLock lock(get_cidgid_lock());
    char* key = NULL;
    tgg_gid_list* value = NULL;
    uint32_t next = 0;
    int ret = 0;
    while (1) {
        ret = rte_hash_iterate(g_cidgid_hash, (const void**)&key, (void**)&value, &next);
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
        if(uid && !strncmp(key, uid, strlen(key))) {
            printf("find key:%s\n", uid);
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
    ReadLock lock(get_idxhsh_lock());
    // 只添加key  且不需要value时，不需要先查找
    // int ret = rte_hash_lookup_with_hash(g_idx_hash, &idx, rte_hash_crc(&idx, sizeof(int), 0));
    // if (ret < 0) {// 首次插入
        return rte_hash_add_key_with_hash(g_idx_hash, &idx, rte_hash_crc(&idx, sizeof(int), 0));
    // }
    // RTE_LOG(ERR, USER1, "[%s][%d] idx: %d already exist.\n", __FILE__, __LINE__, idx);
    // return -1;
}
int tgg_del_idx(int idx)
{
    ReadLock lock(get_idxhsh_lock());
    int ret = rte_hash_del_key_with_hash(g_idx_hash, &idx, rte_hash_crc(&idx, sizeof(idx), 0));
    if (ret < 0) {
    //     // 在并发情况下删除key之后，位置还在，需要删除位置信息，详情参考函数说明
    //     if (rte_hash_free_key_with_position(g_idx_hash, ret) < 0) {
    //         RTE_LOG(ERR, USER1, "[%s][%d]Del idx[%d] pos failed:%d.\n", __FILE__, __LINE__, idx, ret);
    //         return -EINVAL;
    //     }
    // } else {
        LOG_ERROR("Del idx[%d] data failed:%d.", idx, ret);
        return -EINVAL;
    }
    return 0;
}
int tgg_check_idx_exist(int idx)
{
    return rte_hash_lookup_with_hash(g_idx_hash, &idx, rte_hash_crc(&idx, sizeof(int), 0));
}

int tgg_add_bwfdx(int bwfdx)
{
    ReadLock lock(get_bwfdxhsh_lock());
    // TODO  不查询直接插入，根据返回码判断插入成功、失败、已存在等
    // int ret = rte_hash_lookup_with_hash(g_bwfdx_hash, &bwfdx, rte_hash_crc(&bwfdx, sizeof(int), 0));
    // if (ret < 0) {// 首次插入
        return rte_hash_add_key_with_hash(g_bwfdx_hash, &bwfdx, rte_hash_crc(&bwfdx, sizeof(int), 0));
    // }
    // RTE_LOG(ERR, USER1, "[%s][%d] bwfdx: %d already exist.\n", __FILE__, __LINE__, bwfdx);
    // return -1;
}

int tgg_del_bwfdx(int bwfdx)
{
    ReadLock lock(get_bwfdxhsh_lock());
    int ret = rte_hash_del_key_with_hash(g_bwfdx_hash, &bwfdx, rte_hash_crc(&bwfdx, sizeof(bwfdx), 0));
    if (ret < 0) {
    //     // 在并发情况下删除key之后，位置还在，需要删除位置信息，详情参考函数说明
    //     if (rte_hash_free_key_with_position(g_bwfdx_hash, ret) < 0) {
    //         RTE_LOG(ERR, USER1, "[%s][%d]Del bwfdx[%d] pos failed:%d.\n", __FILE__, __LINE__, bwfdx, ret);
    //         return -EINVAL;
    //     }
    // } else {
        LOG_ERROR("Del bwfdx[%d] data failed:%d.", bwfdx, ret);
        return -EINVAL;
    }
    return ret;
}

int tgg_check_bwfdx_exist(int bwfdx)
{
    return rte_hash_lookup_with_hash(g_bwfdx_hash, &bwfdx, rte_hash_crc(&bwfdx, sizeof(int), 0));
}

int tgg_get_bwfdx_count()
{
    ReadLock lock(get_bwfdxhsh_lock());
    return rte_hash_count(g_bwfdx_hash);
}

int tgg_get_bwfdx_bypos(int pos)
{
    ReadLock lock(get_bwfdxhsh_lock());
    int* key = NULL;
    if (rte_hash_get_key_with_position(g_bwfdx_hash, pos, (void**)(&key)) < 0)
        return -1;
    return *key;
}

int tgg_get_load_balance()
{
    // 假设负载是一个简单的整数，表示负载量
    int min_load = 0, cur_load = 0;
    int bwfdx = -1, cur_bwfdx = -1;
    void *key;
    void *value;
    uint32_t index = 0;
    int ret;
    printf("unit count: %d\n", rte_hash_count(g_bwfdx_hash));
    ReadLock lock(get_bwfdxhsh_lock());
    // 遍历哈希表，找到负载最小的 fd
    ret = rte_hash_iterate(g_bwfdx_hash, (const void**)&key, (void**)&value, &index);
    if (ret < 0) {
        if (ret == -ENOENT) {
            // 哈希表为空，返回错误或采取相应措施
            LOG_ERROR("Hash table is empty.");
            return -1;
        } else {
            // 其他错误
            LOG_ERROR("rte_hash_iterate");
            return -1;
        }
    }
    bwfdx = *(int *)key;
    min_load = tgg_get_bwfdx_load(bwfdx & 0xf, bwfdx >> 8);

    while ((ret = rte_hash_iterate(g_bwfdx_hash, (const void**)&key, (void**)&value, &index)) >= 0) {
        cur_bwfdx = *(int *)key;
        cur_load = tgg_get_bwfdx_load(cur_bwfdx & 0xf, cur_bwfdx >> 8);
        if (cur_load < min_load) {
            bwfdx = cur_bwfdx;
            min_load = cur_load;
        }
    }

    // 找到最小负载的 fd 后，可以对其进行相应操作，例如将新的请求发送到该 fd
    if (bwfdx!= -1) {
        // 这里不更新负载，外面可能会失败
        LOG_INFO("Assigning new request to fd %d with load %d", bwfdx, min_load);
        return bwfdx;
    } else {
        LOG_ERROR("No valid fd found.");
        return -1;
    }
}

void tgg_iter_del_bwfdx(int prc_id)
{
    int *key;
    int *value;
    uint32_t index;
    int ret;
    ReadLock lock(get_bwfdxhsh_lock());

    // 开始迭代哈希表
    ret = rte_hash_iterate(g_bwfdx_hash, (const void**)&key, (void**)&value, &index);
    while (ret >= 0) {
        // 删除当前键
        if( ((*key) & 0xf) == prc_id ) {
            ret = rte_hash_del_key(g_bwfdx_hash, key);
            if (ret < 0) {
                if (ret == -ENOENT) {
                    // 键不存在，可能已经被删除或未添加成功
                    LOG_ERROR("Key not found during deletion.");
                } else {
                    LOG_ERROR("rte_hash_del_key");
                }
            } else {
                printf("Deleted key: %d\n", *key);
            }
        }
        // 继续迭代
        ret = rte_hash_iterate(g_bwfdx_hash, (const void**)&key, (void**)&value, &index);
    }
    if (ret < 0 && ret!= -ENOENT) {
        LOG_ERROR("rte_hash_iterate");
    }
}

int tgg_add_bwwkkey(const char* bwwkkey)
{
    LOG_DEBUG("add bw worker key[%s].", bwwkkey);
    APROPRIAT_HASH_KEY(bwwkkey, TGG_BWWKKEY_LEN);
    ReadLock lock(get_bwwkkeyhsh_lock());
    // TODO  不查询直接插入，根据返回码判断插入成功、失败、已存在等
    // int ret = rte_hash_lookup_with_hash(g_bwfdx_hash, &bwfdx, rte_hash_crc(&bwfdx, sizeof(int), 0));
    // if (ret < 0) {// 首次插入
        return rte_hash_add_key_with_hash(g_bwwkkey_hash, _key, rte_hash_crc(_key, TGG_BWWKKEY_LEN, 0));
    // }
    // RTE_LOG(ERR, USER1, "[%s][%d] bwfdx: %d already exist.\n", __FILE__, __LINE__, bwfdx);
    // return -1;
}

int tgg_del_bwwkkey(const char* bwwkkey)
{
    LOG_DEBUG("del bw worker key[%s].", bwwkkey);
    APROPRIAT_HASH_KEY(bwwkkey, TGG_BWWKKEY_LEN);
    ReadLock lock(get_bwwkkeyhsh_lock());
    int ret = rte_hash_del_key_with_hash(g_bwwkkey_hash, _key, rte_hash_crc(_key, TGG_BWWKKEY_LEN, 0));
    if (ret < 0) {
    //     // 在并发情况下删除key之后，位置还在，需要删除位置信息，详情参考函数说明
    //     if (rte_hash_free_key_with_position(g_bwwkkey_hash, ret) < 0) {
    //         RTE_LOG(ERR, USER1, "[%s][%d]Del bwwkkey[%s] pos failed:%d.\n", __FILE__, __LINE__, bwwkkey, ret);
    //         return -EINVAL;
    //     }
    // } else {
        LOG_ERROR("Del bwwkkey[%s] data failed:%d.", bwwkkey, ret);
        return -EINVAL;
    }
    return 0;
}

int tgg_check_bwwkkey_exist(const char* bwwkkey)
{
    APROPRIAT_HASH_KEY(bwwkkey, TGG_BWWKKEY_LEN);
    ReadLock lock(get_bwwkkeyhsh_lock());
    return rte_hash_lookup_with_hash(g_bwwkkey_hash, _key, rte_hash_crc(_key, TGG_BWWKKEY_LEN, 0));
}

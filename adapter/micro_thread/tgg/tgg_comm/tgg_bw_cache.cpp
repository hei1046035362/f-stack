#include <rte_hash.h>
#include <rte_cuckoo_hash.h>
#include <rte_malloc.h>
#include <rte_hash_crc.h>
#include "tgg_common.h"
#include "tgg_bw_cache.h"
#include "tgg_lock.h"
#include "comm/TggLock.hpp"
#include "comm/log.hpp"
#include "comm/common.hpp"

extern const struct rte_hash *g_gid_hash;
extern const struct rte_hash *g_uid_hash;
extern const struct rte_hash *g_cid_hash;
extern const struct rte_hash *g_cidgid_hash;
extern const struct rte_hash *g_idx_hash[];
extern const struct rte_hash *g_bwfdx_hash;
extern const struct rte_hash *g_bwwkkey_hash;
extern struct rte_hash *g_expt_cid_hash[];// sendgroup时，要排除的cid列表，标准库的set和unordered_set效率太低

extern struct rte_rcu_qsbr *g_gid_rcu;
extern struct rte_rcu_qsbr *g_uid_rcu;
extern struct rte_rcu_qsbr *g_cid_rcu;
extern struct rte_rcu_qsbr *g_cidgid_rcu;
extern struct rte_rcu_qsbr *g_bwfdx_rcu;
extern struct rte_rcu_qsbr *g_bwwkkey_rcu;


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

// 针对key-list的hash
static int tgg_hash_add_keywithfdlst(const rte_hash* hash, const char* key, int key_len, int64_t fdidcid)
{
    tgg_fd_hash_value *node_list;

    // 查找或创建哈希表项
    if (rte_hash_lookup_with_hash_data(hash, key, rte_hash_crc(key, key_len, 0), (void**)&node_list) < 0) {
        node_list = (tgg_fd_hash_value *)dpdk_rte_malloc(sizeof(tgg_fd_hash_value));
        if (!node_list) {
            LOG_ERROR("add hash[%s] key[%s] failed, malloc node_list failed.", hash->name, key);
            return -1;
        }
        node_list->list = NULL;
        rte_rwlock_init(&node_list->lock);
        if (rte_hash_add_key_with_hash_data(hash, key, rte_hash_crc(key, key_len, 0), node_list) < 0) {
            LOG_ERROR("add hash[%s] key[%s] failed.", hash->name, key);
            dpdk_rte_free(node_list);
            return -1;
        }
    }

    // 检查是否已存在 fdidcid
    rte_rwlock_read_lock(&node_list->lock);
    tgg_fd_list *current = node_list->list;
    while (current) {
        if (current->fdidcid == fdidcid) {
            rte_rwlock_read_unlock(&node_list->lock);
            LOG_WARNING("Duplicate hash[%s] key[%s] found.", hash->name, key);
            return 0; // 重复的 fdidcid
        }
        current = current->next;
    }
    rte_rwlock_read_unlock(&node_list->lock);

    // 分配新节点
    tgg_fd_list *new_node = (tgg_fd_list *)dpdk_rte_malloc(sizeof(tgg_fd_list));
    if (!new_node) {
        return -1;
    }
    new_node->fdidcid = fdidcid;
    new_node->next = NULL;

    // 获取写锁，添加节点
    rte_rwlock_write_lock(&node_list->lock);
    new_node->next = node_list->list;
    node_list->list = new_node;
    rte_rwlock_write_unlock(&node_list->lock);

    return 0;
}

// 获取hash value/list
static void* tgg_hash_get_value(const rte_hash* hash, const char* key, int key_len)
{
    void* pdata = NULL;
    int ret = rte_hash_lookup_with_hash_data(hash, key, rte_hash_crc(key, key_len, 0), &pdata);
    if (ret < 0) {
        LOG_DEBUG("Get key[%s] data failed:%d", key, ret);
        return NULL;
    }
    return pdata;
}

// 删除整个key
static int tgg_hash_del_key(const rte_hash* hash, rte_rcu_qsbr *rcu, const char* key, int key_len, tgg_free_id_data fp)
{
    tgg_fd_hash_value *node_list;

    // 查找哈希表项
    if (rte_hash_lookup_with_hash_data(hash, key, rte_hash_crc(key, key_len, 0), (void**)&node_list) < 0) {
        LOG_WARNING("delete hash[%s] key[%s] not found.", hash->name, key);
        return -1; // 键不存在
    }

    // 获取写锁，清空链表
    rte_rwlock_write_lock(&node_list->lock);
    tgg_fd_list *current = node_list->list;
    tgg_fd_list *tmp;
    while (current) {
        LOG_DEBUG("deleted hash[%s] key[%s] node[%ld].", hash->name, key, current->fdidcid);
        tmp = current;
        current = current->next;
        dpdk_rte_free(tmp); // 归还节点到内存池
    }
    node_list->list = NULL;
    rte_rwlock_write_unlock(&node_list->lock);

    // 删除哈希表项
    int ret = rte_hash_del_key_with_hash(hash, key, rte_hash_crc(key, key_len, 0));
    if (ret >= 0) {
        // 在并发情况下删除key之后，位置还在，需要删除位置信息，详情参考函数说明
        if (rte_hash_free_key_with_position(hash, ret) < 0) {
            LOG_ERROR("Del hash[%s] key[%s] pos failed:%d", hash->name, key, ret);
            return -EINVAL;
        }
        // rte_rcu_qsbr_synchronize(rcu, RTE_QSBR_THRID_INVALID);// 等待所有读者退出
        LOG_INFO("delete hash[%s] key[%s].", hash->name, key);
        // 释放value的空间
        rte_free(node_list);
    } else {
        LOG_ERROR("Del hash[%s] key[%s] data failed:%d", hash->name, key, ret);
        return -EINVAL;
    }
    return 0;
}

// 删除hash value为list中的单个元素,list节点中的值为fd和idx两个元素
static int tgg_hash_del_fdlst4key(const rte_hash* hash, rte_rcu_qsbr *rcu, const char* key, int key_len, int64_t fdidcid)
{
    tgg_fd_hash_value *node_list;

    // 查找哈希表项
    if (rte_hash_lookup_with_hash_data(hash, key, rte_hash_crc(key, key_len, 0), (void**)&node_list) < 0) {
        LOG_WARNING("delete hash[%s] node[%ld] failed, key[%s] not found.", hash->name, fdidcid, key);
        return -1; // 键不存在
    }

    // 获取写锁，删除节点
    rte_rwlock_write_lock(&node_list->lock);
    tgg_fd_list *current = node_list->list;
    tgg_fd_list *prev = NULL;
    int found = 0;

    // 查找并删除节点
    while (current) {
        if (current->fdidcid == fdidcid) {
            if (prev) {
                prev->next = current->next;
            } else {
                node_list->list = current->next;
            }
            dpdk_rte_free(current); // 归还节点到内存池
            LOG_INFO("deleted hash[%s] key[%s] node[%ld].", hash->name, key, fdidcid);
            found = 1;
            break;
        }
        prev = current;
        current = current->next;
    }

    // 检查是否是最后一个节点
    if (found && node_list->list == NULL) {
        // 最后一个节点，释放 value 并删除 key
        rte_rwlock_write_unlock(&node_list->lock);
        int ret = rte_hash_del_key_with_hash(hash, key, rte_hash_crc(key, key_len, 0));
        if (ret >= 0) {
            // 在并发情况下删除key之后，位置还在，需要删除位置信息，详情参考函数说明
            if (rte_hash_free_key_with_position(hash, ret) < 0) {
                LOG_ERROR("Del hash[%s] key[%s] pos failed:%d", hash->name, key, ret);
                return -EINVAL;
            }
            // rte_rcu_qsbr_synchronize(rcu, RTE_QSBR_THRID_INVALID);// 等待所有读者退出
            LOG_INFO("delete hash[%s] key[%s].", hash->name, key);
            // 释放value的空间
            rte_free(node_list);
            return 0;
        } else {
            LOG_ERROR("Del hash[%s] key[%s] data failed:%d", hash->name, key, ret);
            return -EINVAL;
        }
    }

    if(!found) {
        LOG_ERROR("hash[%s] key[%s] node[%ld] not found.", hash->name, key, fdidcid);
    }
    rte_rwlock_write_unlock(&node_list->lock);
    return found ? 0 : -1; // 返回是否找到并删除
}

static int tgg_hash_get_allkeys(const rte_hash* hash, std::vector<std::string>& lst_items)
{
    char* key = NULL;
    int* value = NULL;
    uint32_t next = 0;
    int ret = 0;
    lst_items.reserve(RESERVED_SIZE_FOR_GID_CIDS);
    while (1) {
        ret = rte_hash_iterate(hash, (const void**)&key, (void**)&value, &next);
        if (-ENOENT == ret) {
            // LOG_DEBUG("iter to the end.");
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
static void* tgg_hash_get_intkey_value(const rte_hash* hash, int64_t key)
{
    void* pdata = NULL;
    int ret = rte_hash_lookup_with_hash_data(hash, &key, rte_hash_crc(&key, sizeof(int64_t), 0), &pdata);
    if (ret < 0) {
        LOG_DEBUG("Get key[%d] data failed:%d", key, ret);
        return NULL;
    }
    return pdata;
}

static int tgg_hash_add_intkeywithfdlst(const rte_hash* hash, int64_t key, const char* data)
{
    tgg_fd_hash_svalue *node_list;
    int data_len = strlen(data);
    if(data_len > TGG_GID_LEN || data_len <= 0) {
        LOG_ERROR("add hash[%s] key[%ld] node[%s] failed, invalid node length[%d].", hash->name, key, data, data_len);
        return -1;
    }

    // 查找或创建哈希表项
    if (rte_hash_lookup_with_hash_data(hash, &key, rte_hash_crc(&key, sizeof(int64_t), 0), (void**)&node_list) < 0) {
        node_list = (tgg_fd_hash_svalue*)dpdk_rte_malloc(sizeof(tgg_fd_hash_svalue));
        if (!node_list) {
            LOG_ERROR("add hash[%s] key[%ld] node[%s] failed, malloc node_list failed.", hash->name, key, data);
            return -1;
        }
        node_list->list = NULL;
        rte_rwlock_init(&node_list->lock);
        if (rte_hash_add_key_with_hash_data(hash, &key, rte_hash_crc(&key, sizeof(int64_t), 0), node_list) < 0) {
            LOG_ERROR("add hash[%s] key[%ld] node[%s] failed.", hash->name, key, data);
            dpdk_rte_free(node_list);
            return -1;
        }
    }

    // 检查是否已存在 fdidcid
    rte_rwlock_read_lock(&node_list->lock);
    tgg_list_id *current = node_list->list;
    while (current) {
        if (!strncmp(current->data, data, data_len)) {
            rte_rwlock_read_unlock(&node_list->lock);
            LOG_WARNING("Duplicate hash[%s] key[%ld] data[%s] found.", hash->name, key, data);
            return 0; // 重复的 节点
        }
        current = current->next;
    }
    rte_rwlock_read_unlock(&node_list->lock);

    // 分配新节点
    tgg_list_id *new_node = (tgg_list_id*)dpdk_rte_malloc(sizeof(tgg_list_id));
    if (!new_node) {
        LOG_ERROR("malloc node for hash[%s] key[%ld] node[%s] failed.", hash->name, key, data);
        return -1;
    }
    memcpy(new_node->data, data, data_len);
    new_node->next = NULL;

    // 获取写锁，添加节点
    rte_rwlock_write_lock(&node_list->lock);
    new_node->next = node_list->list;
    node_list->list = new_node;
    rte_rwlock_write_unlock(&node_list->lock);
    LOG_INFO("added hash[%s] key[%ld] node[%s].", hash->name, key, data);
    return 0;
}

static int tgg_hash_del_intkey_value(const rte_hash* hash, int64_t key, void* value)
{
    // 删除哈希表项
    int ret = rte_hash_del_key_with_hash(hash, &key, rte_hash_crc(&key, sizeof(int64_t), 0));
    if (ret >= 0) {
        // 在并发情况下删除key之后，位置还在，需要删除位置信息，详情参考函数说明
        if (rte_hash_free_key_with_position(hash, ret) < 0) {
            LOG_ERROR("Del hash[%s] key[%ld] pos failed:%d", hash->name, key, ret);
            return -EINVAL;
        }
        // rte_rcu_qsbr_synchronize(rcu, RTE_QSBR_THRID_INVALID);// 等待所有读者退出
        LOG_INFO("delete hash[%s] key[%ld].", hash->name, key);
        // 释放value的空间
        dpdk_rte_free(value);
    } else {
        LOG_ERROR("Del hash[%s] key[%ld] data failed:%d", hash->name, key, ret);
        return -EINVAL;
    }
    return 0;
}

// 删除整个key
static int tgg_hash_del_intkey(const rte_hash* hash, rte_rcu_qsbr *rcu, int64_t key, tgg_free_id_data fp)
{
    tgg_fd_hash_svalue *node_list;

    // 查找哈希表项
    if (rte_hash_lookup_with_hash_data(hash, &key, rte_hash_crc(&key, sizeof(int64_t), 0), (void**)&node_list) < 0) {
        LOG_INFO("delete hash[%s] key[%ld] not found.", hash->name, key);
        return -1; // 键不存在
    }

    // 获取写锁，清空链表
    rte_rwlock_write_lock(&node_list->lock);
    tgg_list_id *current = node_list->list;
    tgg_list_id *tmp;
    while (current) {
        LOG_DEBUG("deleted hash[%s] key[%ld] node[%s].", hash->name, key, current->data);
        tmp = current;
        current = current->next;
        dpdk_rte_free(tmp); // 归还节点到内存池
    }
    node_list->list = NULL;
    rte_rwlock_write_unlock(&node_list->lock);
    return tgg_hash_del_intkey_value(hash, key, (void*)node_list);
    // // 删除哈希表项
    // int ret = rte_hash_del_key_with_hash(hash, &key, rte_hash_crc(&key, sizeof(int64_t), 0));
    // if (ret >= 0) {
    //     // 在并发情况下删除key之后，位置还在，需要删除位置信息，详情参考函数说明
    //     if (rte_hash_free_key_with_position(hash, ret) < 0) {
    //         LOG_ERROR("Del hash[%s] key[%ld] pos failed:%d", hash->name, key, ret);
    //         return -EINVAL;
    //     }
    //     // rte_rcu_qsbr_synchronize(rcu, RTE_QSBR_THRID_INVALID);// 等待所有读者退出
    //     LOG_INFO("delete hash[%s] key[%ld].", hash->name, key);
    //     // 释放value的空间
    //     rte_free(node_list);
    // } else {
    //     LOG_ERROR("Del hash[%s] key[%ld] data failed:%d", hash->name, key, ret);
    //     return -EINVAL;
    // }
    // return 0;

}

// 删除hash value为list中的单个元素,list节点中的值为char[]
static int tgg_hash_del_idlst4intkey(const rte_hash* hash, rte_rcu_qsbr *rcu, int64_t key, const char* id)
{
    tgg_fd_hash_svalue *node_list;
    int id_len = strlen(id);
    if(id_len > TGG_GID_LEN || id_len <= 0) {
        LOG_ERROR("del hash[%s] key[%ld] node[%s] failed, invalid node length[%d].", hash->name, key, id, id_len);
        return -1;
    }

    // 查找哈希表项
    if (rte_hash_lookup_with_hash_data(hash, &key, rte_hash_crc(&key, sizeof(int64_t), 0), (void**)&node_list) < 0) {
        LOG_ERROR("del hash[%s] key[%ld] node[%s] failed:%d, key not found.", hash->name, key, id);
        return -1; // 键不存在
    }

    // 获取写锁，删除节点
    rte_rwlock_write_lock(&node_list->lock);
    tgg_list_id *current = node_list->list;
    tgg_list_id *prev = NULL;
    int found = 0;

    // 查找并删除节点
    while (current) {
        if (!strncmp(current->data, id, id_len)) {
            if (prev) {
                prev->next = current->next;
            } else {
                node_list->list = current->next;
            }
            LOG_INFO("delete hash[%s] key[%d] node[%s].", hash->name, key, id);
            dpdk_rte_free(current); // 归还节点到内存池
            found = 1;
            break;
        }
        prev = current;
        current = current->next;
    }

    // 检查是否是最后一个节点
    if (found && node_list->list == NULL) {
        // 最后一个节点，释放 value 并删除 key
        rte_rwlock_write_unlock(&node_list->lock);
        int ret = rte_hash_del_key_with_hash(hash, &key, rte_hash_crc(&key, sizeof(int64_t), 0));
        if (ret >= 0) {
            // 在并发情况下删除key之后，位置还在，需要删除位置信息，详情参考函数说明
            if (rte_hash_free_key_with_position(hash, ret) < 0) {
                LOG_ERROR("Del hash[%s] key[%d] pos failed:%d", hash->name, key, ret);
                return -EINVAL;
            }
            // rte_rcu_qsbr_synchronize(rcu, RTE_QSBR_THRID_INVALID);// 等待所有读者退出
            LOG_INFO("delete hash[%s] key[%d].", hash->name, key);
            // 释放value的空间
            rte_free(node_list);
            return 0;
        } else {
            LOG_ERROR("Del hash[%s] key[%d] data failed:%d", hash->name, key, ret);
            return -EINVAL;
        }
    }

    rte_rwlock_write_unlock(&node_list->lock);
    if(!found) {
        LOG_ERROR("hash[%s] key[%d] node[%s] not found.", hash->name, key, id);
    }
    return found ? 0 : -1; // 返回是否找到并删除
}


static int tgg_hash_get_all_intkeys(const rte_hash* hash, std::vector<int64_t>& lst_items)
{
    int count = rte_hash_count(hash);
    if(count <= 0) {
        return 0;
    }
    lst_items.reserve(count);
    int64_t* key = NULL;
    int* value = NULL;
    uint32_t next = 0;
    int ret = 0;
    while (1) {
        ret = rte_hash_iterate(hash, (const void**)&key, (void**)&value, &next);
        if (-ENOENT == ret) {
            LOG_DEBUG("iter to the end.");
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
int tgg_add_gid(const char* gid, int64_t fdidcid)
{
    LOG_DEBUG("add gid[%s] fdidcid[%lld].", gid, fdidcid);
    APROPRIAT_HASH_KEY(gid, TGG_GID_LEN);
    // WriteLock lock(get_gidfd_lock());
    return tgg_hash_add_keywithfdlst(g_gid_hash, _key, TGG_GID_LEN, fdidcid);
}

int tgg_get_fdsbygid(const char* gid, std::vector<int64_t>& lst_fd)
{
    lst_fd.reserve(RESERVED_SIZE_FOR_GID_CIDS);
    APROPRIAT_HASH_KEY(gid, TGG_GID_LEN);
    tgg_gid_data* value = (tgg_gid_data*)tgg_hash_get_value(g_gid_hash, _key, TGG_GID_LEN);
    if(!value) {
        LOG_DEBUG("get fdlist failed by gid[%s], value is empty.", gid);
        return -1;
    }
    ReadLock lock(&value->lock);
    tgg_fd_list* current = value->list;
    while (current) {
        lst_fd.push_back(current->fdidcid);
        current = current->next;
    }
    return 0;
}

int tgg_del_gid(const char* gid)
{
    LOG_DEBUG("iter del fdidcid for gid[%s].", gid);
    APROPRIAT_HASH_KEY(gid, TGG_GID_LEN);
    // WriteLock lock(get_gidfd_lock());
    return tgg_hash_del_key(g_gid_hash, g_gid_rcu, _key, TGG_GID_LEN, iter_del_fdlist);
}

void tgg_clean_gid()
{
    LOG_DEBUG("clean gids.");
    if(rte_hash_count(g_gid_hash) <= 0) {
        LOG_DEBUG("gid hash is empty.");
        return;
    }
    std::list<const char*> keys_to_delete; // 预存待删键

    // 阶段1：遍历并标记待删键
    uint32_t iter = 0;
    const char *key;
    int *value;
    while (rte_hash_iterate(g_gid_hash, (const void**)&key, (void**)&value, &iter) >= 0) {
        if (strlen(key) > 0) {
            LOG_DEBUG("add delete gid:%s.", key);
            keys_to_delete.push_back(key);
        } else {
            LOG_WARNING("Ignore invalid gid:%s", key);
        }
    }

    // 阶段2：批量删除并同步RCU
    for (auto del_key : keys_to_delete) {
        if(tgg_del_gid(del_key) < 0) {
            LOG_WARNING("delete gid:%s failed.", key);
        } else {
            LOG_DEBUG("deleted gid:%s.", key);
        }
    }
}

int tgg_del_fd4gid(const char* gid, int64_t fdidcid)
{
    LOG_DEBUG("del fdidcid[%lld] for gid[%s].", fdidcid, gid);
    APROPRIAT_HASH_KEY(gid, TGG_GID_LEN);
    return tgg_hash_del_fdlst4key(g_gid_hash, g_gid_rcu, _key, TGG_GID_LEN, fdidcid);
}

int tgg_get_allonlinegids(std::vector<std::string>& lst_gid)
{
    return tgg_hash_get_allkeys(g_gid_hash, lst_gid);
}

int tgg_get_gid_count()
{
    return rte_hash_count(g_gid_hash);
}

/// 增删查  uid 用户id 
int tgg_add_uid(const char* uid, int64_t fdidcid)
{
    LOG_DEBUG("add uid[%s] fdidcid[%lld].", uid, fdidcid);
    APROPRIAT_HASH_KEY(uid, TGG_UID_LEN);
    return tgg_hash_add_keywithfdlst(g_uid_hash, _key, TGG_UID_LEN, fdidcid);
}

int tgg_del_uid(const char* uid)
{
    LOG_DEBUG("iter del fdidcid for uid[%s].", uid);
    APROPRIAT_HASH_KEY(uid, TGG_UID_LEN);
    return tgg_hash_del_key(g_uid_hash, g_uid_rcu, _key, TGG_UID_LEN, iter_del_fdlist);
}

void tgg_clean_uid()
{
    LOG_DEBUG("clean uids.");
    if(rte_hash_count(g_uid_hash) <= 0) {
        LOG_DEBUG("uid hash is empty.");
        return;
    }
    std::list<const char*> keys_to_delete; // 预存待删键

    // 阶段1：遍历并标记待删键
    uint32_t iter = 0;
    const char *key;
    int *value;
    while (rte_hash_iterate(g_uid_hash, (const void**)&key, (void**)&value, &iter) >= 0) {
        if (strlen(key) > 0) {
            LOG_DEBUG("add delete uid:%s.", key);
            keys_to_delete.push_back(key);
        } else {
            LOG_WARNING("Ignore invalid uid:%s", key);
        }
    }

    // 阶段2：批量删除并同步RCU
    for (auto del_key : keys_to_delete) {
        if(tgg_del_uid(del_key) < 0) {
            LOG_WARNING("delete uid:%s failed.", key);
        } else {
            LOG_DEBUG("deleted uid:%s.", key);
        }
    }
}

int tgg_del_fd4uid(const char* uid, int64_t fdidcid)
{
    LOG_DEBUG("del fdidcid[%lld] for uid[%s].", fdidcid, uid);
    APROPRIAT_HASH_KEY(uid, TGG_UID_LEN);
    // WriteLock lock(get_uidfd_lock());
    return tgg_hash_del_fdlst4key(g_uid_hash, g_uid_rcu, _key, TGG_UID_LEN, fdidcid);
}

int tgg_get_fdsbyuid(const char* uid, std::vector<int64_t>& lst_fd)
{
    APROPRIAT_HASH_KEY(uid, TGG_UID_LEN);
    // ReadLock lock(get_uidfd_lock());
    tgg_uid_data* value = (tgg_uid_data*)tgg_hash_get_value(g_uid_hash, _key, TGG_UID_LEN);
    if(!value) {
        LOG_DEBUG("get fdidcid by uid[%s] failed.", uid);
        return -1;
    }
    lst_fd.reserve(RESERVED_SIZE_FOR_UID_CIDS);
    ReadLock lock(&value->lock);
    tgg_fd_list* current = value->list;
    while (current) {
        lst_fd.push_back(current->fdidcid);
        current = current->next;
    }
    return 0;
}

int tgg_get_allonlineuids(std::vector<std::string>& lst_uid)
{
    return tgg_hash_get_allkeys(g_uid_hash, lst_uid);
}

int tgg_get_uid_count()
{
    return rte_hash_count(g_uid_hash);
}


/// 增删查  cid
int tgg_add_cid(int64_t cid, int64_t fdidcid)
{
    LOG_DEBUG("add cid[%d] fdidcid[%lld].", cid, fdidcid);
    int64_t* value = (int64_t*)dpdk_rte_malloc(sizeof(int64_t));
    if(!value) {
        LOG_ERROR("[%s][%d]add key[%d] failed.", cid);
        return -1;
    }
    *value = fdidcid;
    return rte_hash_add_key_with_hash_data(g_cid_hash, &cid, rte_hash_crc(&cid, sizeof(int64_t), 0), value);

}

int tgg_del_cid(int64_t cid)
{
    LOG_DEBUG("del fdidcid for cid[%d].", cid);
    int64_t *value;
    // 查找哈希表项
    if (rte_hash_lookup_with_hash_data(g_cid_hash, &cid, rte_hash_crc(&cid, sizeof(int64_t), 0), (void**)&value) < 0) {
        LOG_ERROR("del hash[%s] key[%ld] failed, key not found.", g_cid_hash->name, cid);
        return -1; // 键不存在
    }
    return tgg_hash_del_intkey_value(g_cid_hash, cid, value);
    // return tgg_hash_del_intkey(g_cid_hash, g_cid_rcu, cid, free_ciddata);
}

void tgg_clean_cid()
{
    LOG_DEBUG("clean cids.");
    int count = rte_hash_count(g_cid_hash);
    if(count <= 0) {
        LOG_DEBUG("cid hash is empty.");
        return;
    }
    std::vector<int> keys_to_delete; // 预存待删键
    keys_to_delete.reserve(count);

    // 阶段1：遍历并标记待删键
    uint32_t iter = 0;
    int *key, *value;
    while (rte_hash_iterate(g_cid_hash, (const void**)&key, (void**)&value, &iter) >= 0) {
        if (*key > 0) {
            LOG_DEBUG("add delete cid:%d", *key);
            keys_to_delete.push_back(*key);
        } else {
            LOG_WARNING("Ignore invalid cid:%d", *key);
        }
    }

    // 阶段2：批量删除并同步RCU
    for (auto del_key : keys_to_delete) {
        if (tgg_del_cid(del_key) < 0 ) {
            LOG_WARNING("delete cid:%d failed.", del_key);
        } else {
            LOG_INFO("deleted cid:%d.", del_key);
        }
    }
}

int64_t tgg_get_fdbycid(int64_t cid)
{
    int64_t* value = (int64_t*)tgg_hash_get_intkey_value(g_cid_hash, cid);
    if(!value) {
        LOG_DEBUG("get fd by cid:%ld failed, value is NULL.", cid);
        return -1;
    }
    return *value;
}

int tgg_get_allonlinecids(std::vector<int64_t>& lst_cids)
{
    return tgg_hash_get_all_intkeys(g_cid_hash, lst_cids);
}

int tgg_get_allfds(std::vector<int64_t>& lst_fds)
{
    int64_t* key = NULL;
    int64_t* value = NULL;
    uint32_t next = 0;
    int ret = 0;
    int count = rte_hash_count(g_cid_hash);
    if(count <= 0) {
        return 0;
    }
    lst_fds.reserve(count);
    while (1) {
        // WriteLock lock(get_cidfd_lock());
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
int tgg_get_cid_count()
{
    return rte_hash_count(g_cid_hash);
}

int tgg_add_cidgid(int64_t cid, const char* gid)
{
    LOG_DEBUG("add gid[%s] for cid[%d].", gid, cid);
    APROPRIAT_HASH_KEY(gid, TGG_UID_LEN);
    return tgg_hash_add_intkeywithfdlst(g_cidgid_hash, cid, _key);

}

int tgg_get_gidsbycid(int64_t cid, std::vector<std::string>& lst_gid)
{
    // APROPRIAT_HASH_KEY(uid, TGG_UID_LEN);
    // ReadLock lock(get_cidgid_lock());
    tgg_fd_hash_svalue* value = (tgg_gid_list*)tgg_hash_get_intkey_value(g_cidgid_hash, cid);
    if(!value) {
        LOG_INFO("cid [%d] not exist in gid hash.", cid);
        return -1;
    }
    lst_gid.reserve(RESERVED_SIZE_FOR_GID_CIDS);
    ReadLock lock(&(value->lock));
    tgg_list_id *current = value->list;
    while (current) {
        lst_gid.push_back(std::string(current->data));
        current = current->next;
    }
    return 0;
}

// 删掉hash<cid,list<gid>>中的一整个cid
int tgg_del_cid_cidgid(int64_t cid)
{
    LOG_DEBUG("del all gids for cid[%d].", cid);
    // WriteLock lock(get_cidgid_lock());
    if(cid <= 0) {
        LOG_ERROR("del all gids for cid[%d] failed.", cid);        
        return -1;
    }
    return tgg_hash_del_intkey(g_cidgid_hash, g_cidgid_rcu, cid, iter_del_idlist);
}
void tgg_clean_cidgid()
{
    LOG_DEBUG("clean cids in cidgid.");
    int count = rte_hash_count(g_cidgid_hash);
    if(count <= 0) {
        LOG_DEBUG("cidgid hash is empty.");
        return;
    }
    std::vector<int> keys_to_delete; // 预存待删键
    keys_to_delete.reserve(count);
    // 阶段1：遍历并标记待删键
    uint32_t iter = 0;
    int *key, *value;
    while (rte_hash_iterate(g_cidgid_hash, (const void**)&key, (void**)&value, &iter) >= 0) {
        if (*key > 0) {
            LOG_DEBUG("add delete cid:%d in cidgid.", *key);
            keys_to_delete.push_back(*key);
        } else {
            LOG_WARNING("Ignore invalid cid:%d in cidgid.", *key);
        }
    }

    // 阶段2：批量删除并同步RCU
    for (auto del_key : keys_to_delete) {
        if (tgg_del_cid_cidgid(del_key) < 0 ) {
            LOG_WARNING("delete cid:%d in cidgid failed.", del_key);
        } else {
            LOG_INFO("deleted cid:%d in cidgid.", del_key);
        }
    }
}
int tgg_del_gid_cidgid(int64_t cid, const char* gid)
{
    LOG_DEBUG("del gid[%s] for cid[%d].", gid, cid);
    // WriteLock lock(get_cidgid_lock());
    if(cid <= 0) {
        LOG_ERROR("del gid[%s] for cid[%d] failed.", gid, cid);        
        return -1;
    }
    return tgg_hash_del_idlst4intkey(g_cidgid_hash, g_cidgid_rcu, cid, gid);
}

int tgg_get_gidsbyuid(const char* uid, std::set<std::string>& set_gid)
{
    std::vector<int64_t> lstFds;
    tgg_get_fdsbyuid(uid, lstFds);// 通过uid找到cid列表
    std::vector<int64_t>::iterator it = lstFds.begin();
    while(it != lstFds.end()) {
        int cid = GET_CID_FDCID_MASK(*it);
        std::vector<std::string> lstGids;
        tgg_get_gidsbycid(cid, lstGids);// 通过cid找到gid列表
        set_gid.insert(std::make_move_iterator(lstGids.begin()), 
             std::make_move_iterator(lstGids.end()));
        it++;
    }
    return 0;
}

void tgg_del_gid_cidgid(const char* gid)
{
    std::vector<int64_t> fdcids;
    tgg_get_fdsbygid(gid, fdcids);
    // 这里没有复用tgg_get_fdsbygid中的循环是为了减少加锁的时间
    std::vector<int64_t>::iterator it = fdcids.begin();
    while (it != fdcids.end()) {
        int cid = GET_CID_FDCID_MASK(*it);
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
    std::vector<std::string> lst_fd;
    lst_fd.reserve(RESERVED_SIZE_FOR_GID_CIDS);
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
        ReadLock lock(&(value->lock));
        if(!value) {
            printf("key[%s]'s value is empty\n", key);
            break;
        }
        if(uid && !strncmp(key, uid, strlen(key))) {
            printf("find key:%s\n", uid);
        }
        printf("uid:%s\n", key);
        tgg_list_id* tmp = value->list;
        while (tmp) {
            lst_fd.push_back(std::string(tmp->data));
            printf("gid:%s\n", tmp->data);
            tmp = tmp->next;
        }
    }
}

int tgg_add_idx(int coreid, int64_t idx)
{
    return rte_hash_add_key_with_hash(g_idx_hash[coreid], &idx, rte_hash_crc(&idx, sizeof(int64_t), 0));
}
int tgg_del_idx(int coreid, int64_t idx)
{
    int ret = rte_hash_del_key_with_hash(g_idx_hash[coreid], &idx, rte_hash_crc(&idx, sizeof(int64_t), 0));
    if (ret >= 0) {
        // 在并发情况下删除key之后，位置还在，需要删除位置信息，详情参考函数说明
        if (rte_hash_free_key_with_position(g_idx_hash[coreid], ret) < 0) {
            LOG_ERROR("Del idx[%d] pos:%d failed.", idx, ret);
            return -EINVAL;
        }
        LOG_INFO("delete idx[%d] for core[%d].", idx, coreid);
    } else {
        LOG_ERROR("Del idx[%d] data failed:%d.", idx, ret);
        return -EINVAL;
    }
    return 0;
}

int tgg_check_idx_exist(int coreid, int64_t idx)
{
    return rte_hash_lookup_with_hash(g_idx_hash[coreid], &idx, rte_hash_crc(&idx, sizeof(int64_t), 0));
}

int tgg_get_allidxs(int coreid, std::vector<int64_t>& lst_idxs)
{
    return tgg_hash_get_all_intkeys(g_idx_hash[coreid], lst_idxs);
}

int tgg_count_idx(int coreid)
{
    return rte_hash_count(g_idx_hash[coreid]);
}

void tgg_iter_del_idx(int coreid)
{
    int count = rte_hash_count(g_idx_hash[coreid]);
    if(count <= 0) {
        return ;
    }
    std::vector<int64_t> keys_to_delete; // 预存待删键
    keys_to_delete.reserve(count);

    // 阶段1：遍历并标记待删键
    uint32_t iter = 0;
    int64_t *key, *value;
    while (rte_hash_iterate(g_idx_hash[coreid], (const void**)&key, (void**)&value, &iter) >= 0) {
        if ((*key & 0xFF) == coreid) {
            keys_to_delete.push_back(*key);
        }
    }

    // 阶段2：批量删除并同步RCU
    for (auto del_key : keys_to_delete) {
        LOG_WARNING("abnormal delete coreid[%d] idx:%d", coreid, del_key);
        tgg_del_idx(coreid, del_key);
    }
}

int tgg_add_bwfdx(int64_t bwfdx)
{
    if(bwfdx <= 0) {
        LOG_ERROR("invlude bwfdx[%d].", bwfdx);
        return -1;
    }
    // WriteLock lock(get_bwfdxhsh_lock());
    return rte_hash_add_key_with_hash(g_bwfdx_hash, &bwfdx, rte_hash_crc(&bwfdx, sizeof(int64_t), 0));
}

int tgg_del_bwfdx(int64_t bwfdx)
{
    // WriteLock lock(get_bwfdxhsh_lock());
    int ret = rte_hash_del_key_with_hash(g_bwfdx_hash, &bwfdx, rte_hash_crc(&bwfdx, sizeof(int64_t), 0));
    if (ret < 0) {
        LOG_ERROR("Del bwfdx[%d] data failed:%d.", bwfdx, ret);
        return -EINVAL;
    }
    if(rte_hash_free_key_with_position(g_bwfdx_hash, ret) < 0) {
        LOG_ERROR("free key[%ld] pos[%d] failed.", bwfdx, ret);
    }
    LOG_INFO("Del bwfdx[%d].", bwfdx);
    // rte_rcu_qsbr_synchronize(g_bwfdx_rcu, RTE_QSBR_THRID_INVALID);
    return ret;
}

int tgg_check_bwfdx_exist(int64_t bwfdx)
{
    return rte_hash_lookup_with_hash(g_bwfdx_hash, &bwfdx, rte_hash_crc(&bwfdx, sizeof(int64_t), 0));
}

int tgg_get_bwfdx_count()
{
    return rte_hash_count(g_bwfdx_hash);
}

int tgg_get_bwfdx_bypos(int pos)
{
    int64_t* key = NULL;
    if (rte_hash_get_key_with_position(g_bwfdx_hash, pos, (void**)(&key)) < 0)
        return -1;
    return *key;
}

void tgg_getall_bwfdx(std::vector<int64_t>& vec_bwfdx)
{
    void *key;
    void *value;
    uint32_t index = 0;
    int ret;
    vec_bwfdx.clear();
    // ReadLock lock(get_bwfdxhsh_lock());
    while ((ret = rte_hash_iterate(g_bwfdx_hash, (const void**)&key, (void**)&value, &index)) >= 0) {
        vec_bwfdx.push_back(*(int64_t *)key);
    }
}

int tgg_get_load_balance(std::vector<int64_t>& vec_bwfdx, int64_t ipport)
{
    // uint64_t now = get_system_ms();
    size_t count = vec_bwfdx.size();
    if(count > 0) {
        return vec_bwfdx[ipport % count];
    }
    LOG_ERROR("Get load balance failed, no bwfdx found.");
    return -1;
}

void tgg_iter_del_bwfdx(int prc_id)
{
    int count = rte_hash_count(g_bwfdx_hash);
    if(count <= 0) {
        return ;
    }
    std::vector<int64_t> keys_to_delete; // 预存待删键
    keys_to_delete.reserve(count);

    // 阶段1：遍历并标记待删键
    uint32_t iter = 0;
    int64_t *key, *value;
    while (rte_hash_iterate(g_bwfdx_hash, (const void**)&key, (void**)&value, &iter) >= 0) {
        if ((*key & 0xFF) == prc_id) {
            keys_to_delete.push_back(*key);
        }
    }

    // 阶段2：批量删除并同步RCU
    for (auto del_key : keys_to_delete) {
        // 先删除对应的workerkey
        std::string workerkey = tgg_get_bwfdx_workerkey(prc_id, del_key >> 8);
        tgg_del_bwwkkey(workerkey.c_str());

        int pos = rte_hash_del_key_with_hash(g_bwfdx_hash, &del_key, rte_hash_crc(&del_key, sizeof(int64_t), 0));
        if (pos >= 0) {
            if(rte_hash_free_key_with_position(g_bwfdx_hash, pos) < 0 ) { // 标记释放位置
                LOG_ERROR("remove key[%d] pos:%d failed.", del_key, pos);
            }
            LOG_INFO("delete bwfdx[%d] for prc_id[%d].", del_key, prc_id);
        } else {
            LOG_ERROR("delete key[%d] error:%d.", del_key, pos);
        }
    }
}

int tgg_add_bwwkkey(const char* bwwkkey)
{
    LOG_DEBUG("add bw worker key[%s].", bwwkkey);
    APROPRIAT_HASH_KEY(bwwkkey, TGG_BWWKKEY_LEN);
    return rte_hash_add_key_with_hash(g_bwwkkey_hash, _key, rte_hash_crc(_key, TGG_BWWKKEY_LEN, 0));
}

int tgg_del_bwwkkey(const char* bwwkkey)
{
    LOG_DEBUG("del bw worker key[%s].", bwwkkey);
    APROPRIAT_HASH_KEY(bwwkkey, TGG_BWWKKEY_LEN);
    int ret = rte_hash_del_key_with_hash(g_bwwkkey_hash, _key, rte_hash_crc(_key, TGG_BWWKKEY_LEN, 0));
    if (ret >= 0) {
        // 在并发情况下删除key之后，位置还在，需要删除位置信息，详情参考函数说明
        if (rte_hash_free_key_with_position(g_bwwkkey_hash, ret) < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d]Del bwwkkey[%s] pos failed:%d.\n", __FILE__, __LINE__, bwwkkey, ret);
            return -EINVAL;
        }
        // rte_rcu_qsbr_synchronize(g_bwwkkey_rcu, RTE_QSBR_THRID_INVALID);
        LOG_INFO("delete bwwkkey[%s].", bwwkkey);
    } else {
        LOG_ERROR("Del bwwkkey[%s] data failed:%d.", bwwkkey, ret);
        return -EINVAL;
    }
    return 0;
}

int tgg_check_bwwkkey_exist(const char* bwwkkey)
{
    APROPRIAT_HASH_KEY(bwwkkey, TGG_BWWKKEY_LEN);
    return rte_hash_lookup_with_hash(g_bwwkkey_hash, _key, rte_hash_crc(_key, TGG_BWWKKEY_LEN, 0));
}

int tgg_get_allbwwkkeys(std::vector<std::string>& lst_wkkeys)
{
    return tgg_hash_get_allkeys(g_bwwkkey_hash, lst_wkkeys);
}

int tgg_get_bwwoker_count()
{
    return rte_hash_count(g_bwwkkey_hash);
}

int tgg_add_expt_cid(int prc_id, int64_t cid)
{
    return rte_hash_add_key_with_hash(g_expt_cid_hash[prc_id], &cid, rte_hash_crc(&cid, sizeof(int64_t), 0));
}

int tgg_check_expt_cid_exist(int prc_id, int64_t cid)
{
    if(rte_hash_count(g_expt_cid_hash[prc_id]) <= 0) {
        return -1;
    }
    return rte_hash_lookup_with_hash(g_expt_cid_hash[prc_id], &cid, rte_hash_crc(&cid, sizeof(int64_t), 0));
}

void tgg_reset_expt_cid(int prc_id)
{
    if(rte_hash_count(g_expt_cid_hash[prc_id]) <= 0) {
        return;
    }
    rte_hash_reset(g_expt_cid_hash[prc_id]);
}

void print_hash_statistics()
{
    if(rte_eal_process_type() != RTE_PROC_PRIMARY) {
        return;
    }
    LOG_WARNING("****************rte_hash stats*****************");
    for (int i = 0; i < MAX_LCORE_COUNT; ++i)
    {
        if(g_idx_hash[i])
            LOG_WARNING("%s cur count:%ld", g_idx_hash[i]->name, rte_hash_count(g_idx_hash[i]));
        if(g_expt_cid_hash[i])
            LOG_WARNING("%s cur count:%ld", g_expt_cid_hash[i]->name, rte_hash_count(g_expt_cid_hash[i]));
    }
    LOG_WARNING("%s cur count:%ld", g_gid_hash->name, rte_hash_count(g_gid_hash));
    LOG_WARNING("%s cur count:%ld", g_uid_hash->name, rte_hash_count(g_uid_hash));
    LOG_WARNING("%s cur count:%ld", g_cid_hash->name, rte_hash_count(g_cid_hash));
    LOG_WARNING("%s cur count:%ld", g_cidgid_hash->name, rte_hash_count(g_cidgid_hash));
    LOG_WARNING("%s cur count:%ld", g_bwfdx_hash->name, rte_hash_count(g_bwfdx_hash));
    LOG_WARNING("%s cur count:%ld", g_bwwkkey_hash->name, rte_hash_count(g_bwwkkey_hash));
}
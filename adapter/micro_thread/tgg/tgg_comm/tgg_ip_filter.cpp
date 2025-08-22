#include <rte_eal.h>
#include <rte_malloc.h>
#include <rte_errno.h>
#include <rte_hash_crc.h>
#include <cstdint>
#include <string>
#include <fstream>
#include <vector>
#include <cstring>
#include <cerrno>
#include <arpa/inet.h>
#include <list>
#include "tgg_ip_filter.h"
#include "comm/log.hpp"

// 在原有全局变量后添加
struct SharedControl {
    struct rte_hash* whitelist_ptr; // 原子指针
    struct rte_hash* blacklist_ptr;
    struct rte_hash* excludelist_ptr;
    uint32_t version; // 原子版本号
} __rte_cache_aligned; // 缓存行对齐

constexpr const char* IP_FILTER_SYNC = "ipfilter_shared_ctrl";// primary和secondary共享内存，通知secondary过滤器有变化
static struct SharedControl* g_shared_ctrl = NULL;
static rte_memzone* s_zone_share_ctrl = NULL;
struct rte_hash* g_ip_whitelist = NULL;
struct rte_hash* g_ip_blacklist = NULL;
struct rte_hash* g_ip_excludelist = NULL;

struct rte_hash* g_ip_whitelist_origin = NULL;
struct rte_hash* g_ip_blacklist_origin = NULL;
struct rte_hash* g_ip_excludelist_origin = NULL;

struct rte_hash* g_ip_whitelist_bkup = NULL;
struct rte_hash* g_ip_blacklist_bkup = NULL;
struct rte_hash* g_ip_excludelist_bkup = NULL;
// Constants for hash table configuration
constexpr uint32_t HASH_ENTRIES = 1 << 20; // 1M entries, power of 2 for performance
constexpr const char* WHITELIST_SHM_NAME = "whitelist_hash";
constexpr const char* BLACKLIST_SHM_NAME = "blacklist_hash";
constexpr const char* EXCLUDELIST_SHM_NAME = "excludelist_hash";
constexpr const char* WHITELIST_SHM_NAME_BACKUP = "whitelist_bkup_hash";
constexpr const char* BLACKLIST_SHM_NAME_BACKUP = "blacklist_bkup_hash";
constexpr const char* EXCLUDELIST_SHM_NAME_BACKUP = "excludelist_bkup_hash";
constexpr const char* WHITELIST_FILE = "whitelist.txt";
constexpr const char* BLACKLIST_FILE = "blacklist.txt";
constexpr const char* EXCLUDELIST_FILE = "excludelist.txt";


static struct rte_memzone *
find_memzone(const char *name)
{
    unsigned int socket_id = rte_socket_id();
    char mz_name[RTE_MEMZONE_NAMESIZE];
    struct rte_memzone *memzone;

    snprintf(mz_name, RTE_MEMZONE_NAMESIZE, "%s_%u", name, socket_id);
    memzone = (struct rte_memzone *)rte_memzone_lookup(mz_name);
    if (!memzone) {
        LOG_ERROR("memzone[%s] not found.", mz_name);
        return NULL;
    }
    return memzone;
}

static struct rte_memzone *
make_memzone(const char *name, size_t size)
{
    unsigned int socket_id = rte_socket_id();
    struct rte_memzone *memzone;
    char mz_name[RTE_MEMZONE_NAMESIZE];

    snprintf(mz_name, RTE_MEMZONE_NAMESIZE, "%s_%u", name, socket_id);
    memzone = (struct rte_memzone *)rte_memzone_lookup(mz_name);
    if (memzone != NULL && memzone->len != size) {
        memset(memzone->addr, 0, memzone->len);
        rte_memzone_free(memzone);
        memzone = NULL;
        LOG_ERROR("memzone[%s] found, but len[%lu] not match[%lu].", mz_name, memzone->len, size);
    }
    if (memzone == NULL) {
        memzone = (struct rte_memzone *)rte_memzone_reserve_aligned(mz_name, size, socket_id,
                RTE_MEMZONE_2MB, RTE_CACHE_LINE_SIZE);
        if (memzone == NULL){
            LOG_ERROR("Can't allocate memory zone %s, error:%s.", mz_name, rte_strerror(rte_errno));
            rte_exit(EXIT_FAILURE,
                "[%s][%d] Can't allocate memory zone %s, error:%s.\n", __FILE__, __LINE__,
                mz_name, rte_strerror(rte_errno));
        }
    }
    memset(memzone->addr, 0, size);
    LOG_INFO("New zone allocated: %s.", mz_name);
    return memzone;
}


// Convert IP string (e.g., "192.168.1.1") to uint32_t
static bool ip_to_uint64(const std::string& ip_str, uint64_t& ip) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str.c_str(), &addr) == 1) {
        ip = addr.s_addr; // 这里没有调ntohl，因为客户端accept时ip(sockaddr_in.sin_addr)本身也是网络字节序，如果需要改为主机字节序，这里要注意适配
        // LOG_DEBUG("trans ip:%s to int:%d htonl:%ld", ip_str.c_str(), addr.s_addr, ntohl(ip));
        return true;
    }
    return false;
}

// Load IPs from file into a vector
static bool load_ip_file(const std::string& filename, std::vector<uint64_t>& ips) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        LOG_ERROR("open file[%s] failed", filename.c_str());
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines or comments
        if (line.empty() || line[0] == '#') continue;
        
        uint64_t ip;
        if (ip_to_uint64(line, ip)) {
            ips.push_back(ip);
            LOG_DEBUG("add ip:%s ip_int:%ld", line.c_str(), ip);
        } else {
            LOG_WARNING("invalid ip:%s", line.c_str());
        }
    }
    return true;
}

// Create or lookup a hash table in shared memory
static struct rte_hash* create_or_lookup_hash(const char* name, bool is_primary) {
    struct rte_hash_parameters params = {
        .name = name,
        .entries = HASH_ENTRIES,
        .key_len = RTE_ALIGN(sizeof(uint64_t), 8),
        .hash_func = rte_hash_crc, // Fast CRC-based hash function
        .hash_func_init_val = 0,
        .socket_id = int(rte_socket_id()),
        .extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY // Enable concurrent reads
    };

    if (is_primary) {
        return rte_hash_create(&params);
    } else {
        return rte_hash_find_existing(name);
    }
}

static bool init_ip_filter_inner(struct rte_hash*& result, 
                                 struct rte_hash*& origin, const char* origin_hash_name,
                                 struct rte_hash*& bkup, const char* bkup_hash_name,
                                 const char* filepath, bool is_primary) {
    origin = create_or_lookup_hash(origin_hash_name, is_primary);
    bkup = create_or_lookup_hash(bkup_hash_name, is_primary);
    if(!origin || !bkup) {
        LOG_ERROR("init ip filter failed, get ip filter hash[%s] failed, origin:%p "
            " , bkup:%p.",
         origin_hash_name, origin , bkup);
        if(is_primary) {
            rte_hash_free(origin);
            rte_hash_free(bkup);
        }
        return false;
    }
    if(is_primary) {
        std::vector<uint64_t> filterlist_ips;
        if (!load_ip_file(filepath, filterlist_ips)) {
            // 这里不用rte_hash_free，后面reload的时候就不用再create了
            return false;
        }
        for (uint64_t ip : filterlist_ips) {
            if(rte_hash_add_key_with_hash(origin, &ip, rte_hash_crc(&ip, sizeof(ip), 0)) < 0) {
                LOG_WARNING("add ip[%llu] for hash[%s] failed.", ip, origin_hash_name);
            }
        }
    }
    result = origin;
    return true;
}

static bool init_whitelist(const std::string& filepath, bool is_primary) {
    return init_ip_filter_inner(g_ip_whitelist, g_ip_whitelist_origin, WHITELIST_SHM_NAME, 
        g_ip_whitelist_bkup, WHITELIST_SHM_NAME_BACKUP, filepath.c_str(), is_primary);
}

static bool init_blacklist(const std::string& filepath, bool is_primary) {
    return init_ip_filter_inner(g_ip_blacklist, g_ip_blacklist_origin, BLACKLIST_SHM_NAME, 
        g_ip_blacklist_bkup, BLACKLIST_SHM_NAME_BACKUP, filepath.c_str(), is_primary);
}

static bool init_excludelist(const std::string& filepath, bool is_primary) {
    return init_ip_filter_inner(g_ip_excludelist, g_ip_excludelist_origin, EXCLUDELIST_SHM_NAME, 
        g_ip_excludelist_bkup, EXCLUDELIST_SHM_NAME_BACKUP, filepath.c_str(), is_primary);
}

// Initialize IP filter (called by primary and secondary processes)
bool init_ip_filter(bool is_primary, const char* ip_filter_path) {
    if (is_primary) {
        s_zone_share_ctrl = make_memzone(IP_FILTER_SYNC, sizeof(struct SharedControl));
        g_shared_ctrl = (struct SharedControl*)s_zone_share_ctrl->addr;
        g_shared_ctrl->version = 0;
    } else {
        s_zone_share_ctrl = find_memzone(IP_FILTER_SYNC);
        g_shared_ctrl = (struct SharedControl*)s_zone_share_ctrl->addr;
    }
    std::string filter_path;
    filter_path.reserve(256);
    filter_path.append(ip_filter_path);
    filter_path.append("/");
    std::string whitelist_path = filter_path + WHITELIST_FILE;
    std::string blacklist_path = filter_path + BLACKLIST_FILE;
    std::string excludelist_path = filter_path + EXCLUDELIST_FILE;
    init_whitelist(whitelist_path, is_primary);
    init_blacklist(blacklist_path, is_primary);
    init_excludelist(excludelist_path, is_primary);
    if (is_primary) {
        g_shared_ctrl->whitelist_ptr = g_ip_whitelist;
        g_shared_ctrl->blacklist_ptr = g_ip_blacklist;
        g_shared_ctrl->excludelist_ptr = g_ip_excludelist;
    } else {
        // 从进程从共享内存获取最新指针
        g_ip_whitelist = __atomic_load_n(&g_shared_ctrl->whitelist_ptr, __ATOMIC_ACQUIRE);
        g_ip_blacklist = __atomic_load_n(&g_shared_ctrl->blacklist_ptr, __ATOMIC_ACQUIRE);
        g_ip_excludelist = __atomic_load_n(&g_shared_ctrl->excludelist_ptr, __ATOMIC_ACQUIRE);
    }
    return true;
}

static bool iter_clean_hash(struct rte_hash* hash)
{
    std::list<int64_t> keys_to_delete; // 预存待删键

    // 阶段1：遍历并标记待删键
    uint32_t iter = 0;
    int64_t *key, *value;
    while (rte_hash_iterate(hash, (const void**)&key, (void**)&value, &iter) >= 0) {
        keys_to_delete.push_back(*key);
    }

    // 阶段2：批量删除并同步RCU
    for (auto del_key : keys_to_delete) {
        LOG_DEBUG("abnormal delete int ip:%ld", ntohl(del_key));
        int ret = rte_hash_del_key_with_hash(hash, &del_key, rte_hash_crc(&del_key, sizeof(int64_t), 0));
        if (ret >= 0) {
            // 在并发情况下删除key之后，位置还在，需要删除位置信息，详情参考函数说明
            if (rte_hash_free_key_with_position(hash, ret) < 0) {
                LOG_ERROR("Del ip[%ld] pos:%d failed.", del_key, ret);
                return false;
            }
        } else {
            LOG_ERROR("Del ip[%ld] data failed:%d.", del_key, ret);
            return false;
        }
    }
    return true;
}

static bool reload_ip_filter_inner(struct rte_hash*& result, struct rte_hash*& origin, struct rte_hash*& bkup, const char* filepath ){
    struct rte_hash* new_filter_list;
    if(result != origin) {
        iter_clean_hash(origin);
        new_filter_list = origin;
    } else {
        iter_clean_hash(bkup);
        new_filter_list = bkup;
    }
    std::vector<uint64_t> filterlist_ips;
    if (!load_ip_file(filepath, filterlist_ips)) {
        return false;
    }
    for (uint64_t ip : filterlist_ips) {
        rte_hash_add_key_with_hash(new_filter_list, &ip, rte_hash_crc(&ip, sizeof(ip), 0));
    }
    result = new_filter_list;
    return true;
}

static bool reload_whitelist(const std::string& filepath) {
    if(!g_ip_whitelist) {// 如果之前初始化失败，就不是重新加载，而是执行初始化
        return init_whitelist(filepath, true);
    }
    return reload_ip_filter_inner(g_ip_whitelist, g_ip_whitelist_origin, g_ip_whitelist_bkup, filepath.c_str());
}

static bool reload_blacklist(const std::string& filepath) {
    if(!g_ip_blacklist) {// 如果之前初始化失败，就不是重新加载，而是执行初始化
        return init_blacklist(filepath, true);
    }
    return reload_ip_filter_inner(g_ip_blacklist, g_ip_blacklist_origin, g_ip_blacklist_bkup, filepath.c_str());
}

static bool reload_excludelist(const std::string& filepath) {
    if(!g_ip_excludelist) {// 如果之前初始化失败，就不是重新加载，而是执行初始化
        return init_excludelist(filepath, true);
    }
    return reload_ip_filter_inner(g_ip_excludelist, g_ip_excludelist_origin, g_ip_excludelist_bkup, filepath.c_str());
}


// Reload whitelist and blacklist from files (called by primary process only)
bool reload_ip_filter(const char* ip_filter_path) {
    LOG_DEBUG("try to reload ip filter file:%s.", ip_filter_path);
    // Create temporary hash tables
    std::string filter_path;
    filter_path.reserve(256);
    filter_path.append(ip_filter_path);
    filter_path.append("/");
    std::string whitelist_path = filter_path + WHITELIST_FILE;
    std::string blacklist_path = filter_path + BLACKLIST_FILE;
    std::string excludelist_path = filter_path + EXCLUDELIST_FILE;
    if(!reload_whitelist(whitelist_path)) {
        LOG_ERROR("reload whitelist file[%s] failed.", whitelist_path.c_str());
    } else {
        g_shared_ctrl->whitelist_ptr = g_ip_whitelist;
        // 新增：版本号递增（内存屏障保证可见性）
        __atomic_fetch_add(&g_shared_ctrl->version, 1, __ATOMIC_RELEASE);
    }
    
    if(!reload_blacklist(blacklist_path)) {
        LOG_ERROR("reload blacklist file[%s] failed.", blacklist_path.c_str());
    } else {
        g_shared_ctrl->blacklist_ptr = g_ip_blacklist;
        // 新增：版本号递增（内存屏障保证可见性）
        __atomic_fetch_add(&g_shared_ctrl->version, 1, __ATOMIC_RELEASE);
    }
    
    if(!reload_excludelist(excludelist_path)) {
        LOG_ERROR("reload excludelist file[%s] failed.", excludelist_path.c_str());
    } else {
        g_shared_ctrl->excludelist_ptr = g_ip_excludelist;
        // 新增：版本号递增（内存屏障保证可见性）
        __atomic_fetch_add(&g_shared_ctrl->version, 1, __ATOMIC_RELEASE);
    }

    return true;
}

void sync_ip_filter() {
    static uint32_t last_version = 0;
    uint32_t cur_version = __atomic_load_n(&g_shared_ctrl->version, __ATOMIC_ACQUIRE);
    
    if (cur_version != last_version) {
        // 原子加载最新指针
        g_ip_whitelist = __atomic_load_n(&g_shared_ctrl->whitelist_ptr, __ATOMIC_ACQUIRE);
        g_ip_blacklist = __atomic_load_n(&g_shared_ctrl->blacklist_ptr, __ATOMIC_ACQUIRE);
        g_ip_excludelist = __atomic_load_n(&g_shared_ctrl->excludelist_ptr, __ATOMIC_ACQUIRE);
        last_version = cur_version;
    }
}
// Cleanup IP filter (called by primary process only)
void cleanup_ip_filter() {
    rte_hash_free(g_ip_excludelist_origin);
    rte_hash_free(g_ip_blacklist_origin);
    rte_hash_free(g_ip_excludelist_origin);
    rte_hash_free(g_ip_whitelist_bkup);
    rte_hash_free(g_ip_blacklist_bkup);
    rte_hash_free(g_ip_excludelist_bkup);
    rte_memzone_free(s_zone_share_ctrl);
    s_zone_share_ctrl = NULL;
}

// Check if an IP is allowed (true if in whitelist or not in blacklist)
bool is_ip_allowed(uint64_t ip) {
    // Check whitelist first (higher priority)
    if (g_ip_whitelist && rte_hash_lookup_with_hash(g_ip_whitelist, &ip, rte_hash_crc(&ip, sizeof(ip), 0)) >= 0) {
        return true;
    }
    // If not in whitelist, check blacklist
    return !g_ip_blacklist || rte_hash_lookup_with_hash(g_ip_blacklist, &ip, rte_hash_crc(&ip, sizeof(ip), 0)) < 0;
}

// Check if an IP is excluded (true if in excludelist)
bool is_ip_exclude(uint64_t ip) {
    // Check whitelist first (higher priority)
    if (g_ip_excludelist && rte_hash_lookup_with_hash(g_ip_excludelist, &ip, rte_hash_crc(&ip, sizeof(ip), 0)) >= 0) {
        return true;
    }
    return false;
}

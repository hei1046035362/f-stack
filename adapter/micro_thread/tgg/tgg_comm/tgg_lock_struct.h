#ifndef _TGG_LOCK_STRUCT_H_
#define _TGG_LOCK_STRUCT_H_

#include <rte_rwlock.h>
#include <rte_spinlock.h>
#include <rte_memzone.h>
extern struct rte_memzone* g_lock_zone;

// 网关用到的所有进程锁
typedef struct  st_lock_cache {
    rte_rwlock_t gidfd_lock;    // hash<gid, fd>的操作锁
    rte_rwlock_t uidfd_lock;    // hash<uid, fd>的操作锁
    rte_rwlock_t cidfd_lock;    // hash<cid, fd>的操作锁
    rte_rwlock_t uidgid_lock;   // hash<uid, gid>的操作锁
    rte_spinlock_t cli_lock;    // array[fd,{cid,uid,status,reserved[128]}]的操作锁
    rte_atomic32_t idx_lock;    // idx累加的操作锁
} tgg_lock;

static rte_rwlock_t* get_gidfd_lock() {return &(((tgg_lock*)(g_lock_zone->addr))->gidfd_lock);}
static rte_rwlock_t* get_uidfd_lock() {return &(((tgg_lock*)(g_lock_zone->addr))->uidfd_lock);}
static rte_rwlock_t* get_cidfd_lock() {return &(((tgg_lock*)(g_lock_zone->addr))->cidfd_lock);}
static rte_rwlock_t* get_uidgid_lock() {return &(((tgg_lock*)(g_lock_zone->addr))->uidgid_lock);}
static rte_spinlock_t* get_cli_lock() {return &(((tgg_lock*)(g_lock_zone->addr))->cli_lock);}
static rte_atomic32_t* get_idx_lock() {return &(((tgg_lock*)(g_lock_zone->addr))->idx_lock);}

#endif // _TGG_LOCK_STRUCT_H_
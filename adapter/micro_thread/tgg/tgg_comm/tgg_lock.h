#ifndef _TGG_LOCK_STRUCT_H_
#define _TGG_LOCK_STRUCT_H_

#include <rte_rwlock.h>
#include <rte_spinlock.h>
#include <rte_memzone.h>

// 网关用到的所有进程锁
typedef struct  st_lock_cache {
    rte_rwlock_t gidfd_lock;    // hash<gid, fd>的操作锁
    rte_rwlock_t uidfd_lock;    // hash<uid, fd>的操作锁
    rte_rwlock_t cidfd_lock;    // hash<cid, fd>的操作锁
    rte_rwlock_t uidgid_lock;   // hash<uid, gid>的操作锁
    rte_spinlock_t cli_lock;    // array[fd,{cid,uid,status,reserved[128]}]的操作锁
    rte_atomic32_t idx_lock;    // idx累加的操作锁
    rte_atomic32_t redis_init_lock;    // idx累加的操作锁    
} tgg_lock;

rte_rwlock_t* get_gidfd_lock();
rte_rwlock_t* get_uidfd_lock();
rte_rwlock_t* get_cidfd_lock();
rte_rwlock_t* get_uidgid_lock();
rte_spinlock_t* get_cli_lock();
rte_atomic32_t* get_idx_lock();
rte_atomic32_t* get_redis_init_lock();

#endif // _TGG_LOCK_STRUCT_H_
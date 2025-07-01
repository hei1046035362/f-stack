#ifndef _TGG_LOCK_STRUCT_H_
#define _TGG_LOCK_STRUCT_H_

#include <rte_rwlock.h>
#include <rte_spinlock.h>
#include <rte_memzone.h>

// 网关用到的所有进程锁
typedef struct  st_lock_cache {
    rte_rwlock_t bwfdxhsh_lock;    // hash<bwfdx, NULL> 的操作锁  存放正在使用的bwfdx
    rte_rwlock_t bwwkkeyhsh_lock;    // hash<bwwkkey, NULL> 的操作锁  存放正在使用的bwfdx
    rte_rwlock_t idxhsh_lock;    // hash<idx, NULL> 的操作锁   存放正在使用的idx
    rte_rwlock_t gidfd_lock;    // hash<gid, fd>的操作锁
    rte_rwlock_t uidfd_lock;    // hash<uid, fd>的操作锁
    rte_rwlock_t cidfd_lock;    // hash<cid, fd>的操作锁
    rte_rwlock_t cidgid_lock;   // hash<uid, gid>的操作锁
    rte_spinlock_t cli_lock;    // array[fd,{cid,uid,status,reserved[128]}]的操作锁
                                    // TODO 多个进程共用一把锁，对性能会有一定影响，需要考虑优化
    rte_spinlock_t bwfdx_lock;    // bwfdx 操作锁
    rte_atomic32_t idx_lock;    // idx累加的操作锁
    rte_spinlock_t bwprc_lock;    // bw进程序号锁 防止不同的进程使用同一个序号

} tgg_lock;

rte_rwlock_t* get_bwfdxhsh_lock();// 暂未使用，后续要根据联调、压测结果决定是否会用到  getallkey和add、del会否冲突，待定
rte_rwlock_t* get_bwwkkeyhsh_lock();// 未使用
rte_rwlock_t* get_idxhsh_lock();// 未使用
rte_rwlock_t* get_gidfd_lock();// 未使用
rte_rwlock_t* get_uidfd_lock();// 未使用
rte_rwlock_t* get_cidfd_lock();// 未使用
rte_rwlock_t* get_cidgid_lock();// 未使用
rte_spinlock_t* get_cli_lock();// 暂未使用，后续要根据联调、压测结果决定是否会用到
rte_spinlock_t* get_bwfdx_lock();// 未使用
rte_atomic32_t* get_idx_lock();// 未使用
rte_spinlock_t* get_bwprc_lock();// 有用

#endif // _TGG_LOCK_STRUCT_H_
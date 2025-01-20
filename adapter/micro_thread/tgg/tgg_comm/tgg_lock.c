#include "tgg_lock.h"

extern struct rte_memzone* g_lock_zone;


rte_rwlock_t* get_bwfdxhsh_lock() {return &(((tgg_lock*)(g_lock_zone->addr))->bwfdxhsh_lock);}
rte_rwlock_t* get_bwwkkeyhsh_lock() {return &(((tgg_lock*)(g_lock_zone->addr))->bwwkkeyhsh_lock);}
rte_rwlock_t* get_idxhsh_lock() {return &(((tgg_lock*)(g_lock_zone->addr))->idxhsh_lock);}
rte_rwlock_t* get_gidfd_lock() {return &(((tgg_lock*)(g_lock_zone->addr))->gidfd_lock);}
rte_rwlock_t* get_uidfd_lock() {return &(((tgg_lock*)(g_lock_zone->addr))->uidfd_lock);}
rte_rwlock_t* get_cidfd_lock() {return &(((tgg_lock*)(g_lock_zone->addr))->cidfd_lock);}
rte_rwlock_t* get_uidgid_lock() {return &(((tgg_lock*)(g_lock_zone->addr))->uidgid_lock);}
rte_spinlock_t* get_cli_lock() {return &(((tgg_lock*)(g_lock_zone->addr))->cli_lock);}
rte_spinlock_t* get_bwfdx_lock() {return &(((tgg_lock*)(g_lock_zone->addr))->bwfdx_lock);}
rte_atomic32_t* get_idx_lock() {return &(((tgg_lock*)(g_lock_zone->addr))->idx_lock);}
rte_atomic32_t* get_redis_init_lock() {return &(((tgg_lock*)(g_lock_zone->addr))->redis_init_lock);}


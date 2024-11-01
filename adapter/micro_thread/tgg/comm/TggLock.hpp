#pragma once
#include <rte_rwlock.h>
#include <rte_spinlock.h>

// hash表操作
class ReadLock
{
public:
    ReadLock(rte_rwlock_t* lock):_rwlock(lock) {rte_rwlock_read_lock(_rwlock);}
    ~ReadLock(){rte_rwlock_read_unlock(_rwlock);}
private:
    rte_rwlock_t* _rwlock;
};

class WriteLock
{
public:
    WriteLock(rte_rwlock_t* lock):_rwlock(lock) {rte_rwlock_write_lock(_rwlock);}
    ~WriteLock(){rte_rwlock_write_unlock(_rwlock);}
private:
    rte_rwlock_t* _rwlock;
};


// 轻量操作
class SpinLock
{
public:
    SpinLock(rte_spinlock_t* lock):_lock(lock) {rte_spinlock_lock(_lock);}
    ~SpinLock(){rte_spinlock_unlock(_lock);}
private:
    rte_spinlock_t* _lock;
};

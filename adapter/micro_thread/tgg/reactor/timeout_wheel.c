#include "reactor.h"
#include "comm/log.hpp"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/timerfd.h>

// 获取当前时间（毫秒）
static inline uint64_t get_current_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 初始化时间轮
int timer_wheel_init(timer_wheel_t* wheel) {
    if (!wheel) return -1;
    
    // 初始化所有槽
    for (int i = 0; i < TIME_WHEEL_SIZE; i++) {
        wheel->slots[i].head = NULL;
        wheel->slots[i].tail = NULL;
    }
    
    // 初始化节点映射表
    memset(wheel->nodes, 0, sizeof(wheel->nodes));
    
    wheel->current_slot = 0;
    wheel->current_time = get_current_ms();
    wheel->last_check_time = wheel->current_time;
    wheel->count = 0;
    
    // LOG_INFO("时间轮初始化完成，大小: %d，时间单位: %dms", 
    //          TIME_WHEEL_SIZE, TIME_UNIT_MS);
    return 0;
}

// 释放时间轮
void timer_wheel_free(timer_wheel_t* wheel) {
    if (!wheel) return;
    
    // 释放所有节点
    for (int fd = 0; fd < MAX_CLIENTS; fd++) {
        if (wheel->nodes[fd]) {
            free(wheel->nodes[fd]);
            wheel->nodes[fd] = NULL;
        }
    }
    
    wheel->count = 0;
    // LOG_INFO("时间轮释放完成");
}

// 计算超时时间对应的槽位
static int calculate_slot(timer_wheel_t* wheel, uint64_t expire_ms) {
    uint64_t now = wheel->current_time;
    uint64_t diff_ms = expire_ms - now;
    
    if (diff_ms <= 0) {
        return wheel->current_slot;  // 立即超时
    }
    
    // 计算距离当前时间的槽位数
    int slots = (int)((diff_ms + TIME_UNIT_MS - 1) / TIME_UNIT_MS);
    
    if (slots >= TIME_WHEEL_SIZE) {
        slots = TIME_WHEEL_SIZE - 1;  // 放到最后一个槽
    }
    
    int target_slot = (wheel->current_slot + slots) % TIME_WHEEL_SIZE;
    
    // LOG_DEBUG("计算槽位: diff=%lums, slots=%d, 当前槽=%d, 目标槽=%d", 
    //           diff_ms, slots, wheel->current_slot, target_slot);
    
    return target_slot;
}

// 添加定时器（O(1)操作）
int timer_wheel_add(timer_wheel_t* wheel, int fd, uint64_t expire_ms) {
    if (fd < 0 || fd >= MAX_CLIENTS) {
        LOG_ERROR("invalid fd: fd=%d", fd);
        return -1;
    }
    
    // 如果节点已存在，先移除
    if (wheel->nodes[fd]) {
        timer_wheel_remove(wheel, fd);
    }
    
    // 计算槽位
    int slot = calculate_slot(wheel, expire_ms);
    
    // 创建节点
    timer_node_t* node = (timer_node_t*)calloc(1, sizeof(timer_node_t));
    if (!node) {
        LOG_ERROR("calloc failed.");
        return -1;
    }
    
    node->fd = fd;
    node->expire_time = expire_ms;
    node->slot = slot;
    node->next = NULL;
    node->prev = NULL;
    
    // 插入到槽的链表尾部
    timer_slot_t* timer_slot = &wheel->slots[slot];
    
    if (timer_slot->tail) {
        // 链表非空，插入到尾部
        timer_slot->tail->next = node;
        node->prev = timer_slot->tail;
        timer_slot->tail = node;
    } else {
        // 链表为空
        timer_slot->head = node;
        timer_slot->tail = node;
    }
    
    // 保存映射
    wheel->nodes[fd] = node;
    wheel->count++;
    
    // LOG_DEBUG("添加定时器: fd=%d, 超时时间=%lu, 槽位=%d, 总定时器数=%d", 
    //           fd, expire_ms, slot, wheel->count);
    
    return 0;
}

// 移除定时器（O(1)操作）
int timer_wheel_remove(timer_wheel_t* wheel, int fd) {
    if (fd < 0 || fd >= MAX_CLIENTS || !wheel->nodes[fd]) {
        return 0;  // 节点不存在，直接返回成功
    }
    
    timer_node_t* node = wheel->nodes[fd];
    
    // 从链表中移除节点（使用记录的 slot，O(1)）
    int slot = node->slot;
    if (slot < 0 || slot >= TIME_WHEEL_SIZE) {
        // 防御性处理：回退到全表扫描（兼容旧数据）
        for (int i = 0; i < TIME_WHEEL_SIZE; i++) {
            if (wheel->slots[i].head == node) {
                slot = i;
                break;
            }
        }
    }

    timer_slot_t* timer_slot = &wheel->slots[slot];
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        // 头节点
        timer_slot->head = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    } else {
        // 尾节点
        timer_slot->tail = node->prev;
    }
    
    // 释放节点
    free(node);
    wheel->nodes[fd] = NULL;
    wheel->count--;
    
    // LOG_DEBUG("移除定时器: fd=%d, 剩余定时器数=%d", fd, wheel->count);
    
    return 0;
}

// 更新定时器（O(1)操作）
int timer_wheel_update(timer_wheel_t* wheel, int fd, uint64_t expire_ms) {
    if (fd < 0 || fd >= MAX_CLIENTS) {
        return -1;
    }
    
    // 先移除旧的
    timer_wheel_remove(wheel, fd);
    
    // 再添加新的
    return timer_wheel_add(wheel, fd, expire_ms);
}

// 处理当前槽的超时定时器
int timer_wheel_process_slot(timer_wheel_t* wheel, int slot,
                            void (*callback)(int fd, void* arg), 
                            void* arg) {
    timer_slot_t* timer_slot = &wheel->slots[slot];
    uint64_t now = wheel->current_time;
    int processed = 0;
    
    // LOG_DEBUG("处理槽位 %d 的定时器", slot);
    
    // 处理当前槽的所有定时器（不能假设链表按时间排序，遍历所有节点）
    timer_node_t* node = timer_slot->head;
    while (node) {
        timer_node_t* next = node->next;

        if (node->expire_time < now) {
            if (callback) {
                callback(node->fd, arg);
            }
            // 从时间轮中移除（timer_wheel_remove 会处理链表指针并 free 节点）
            timer_wheel_remove(wheel, node->fd);
            processed++;
        }
        node = next;
    }
    
    return processed;
}

// 移动时间轮指针并处理超时
int timer_wheel_process(timer_wheel_t* wheel,
                       void (*callback)(int fd, void* arg), 
                       void* arg, uint64_t now) {
    uint64_t elapsed = now - wheel->last_check_time;
    
    if (elapsed < TIME_UNIT_MS) {
        return 0;  // 时间未到，不处理
    }
    
    wheel->current_time = now;
    
    // 计算需要移动多少槽（以 TIME_UNIT_MS 为单位）
    // 根据实际 elapsed 时间计算，最多不超过轮大小（防止无限积压）
    // 例如：elapsed=100ms → steps=1，elapsed=300ms → steps=3，elapsed=60s → steps=600
    int steps = (int)(elapsed / TIME_UNIT_MS);
    int steps_to_process = steps > TIME_WHEEL_SIZE ? TIME_WHEEL_SIZE : steps;
    
    // 如果需要处理的槽数已经达到轮大小，说明有严重延迟，记录告警
    if (steps_to_process >= TIME_WHEEL_SIZE) {
        LOG_WARNING("Timer wheel severe backlog: elapsed=%lums (>60s), processing full wheel. Check system load or callback latency",
                    elapsed);
    }
    
    int total_processed = 0;
    
    // LOG_DEBUG("时间轮处理: 经过时间=%lums, 需要移动%d个槽, 本次处理%d个", elapsed, steps, steps_to_process);
    
    for (int i = 0; i < steps_to_process; i++) {
        int slot = wheel->current_slot;
        
        // 处理当前槽
        int processed = timer_wheel_process_slot(wheel, slot, callback, arg);
        total_processed += processed;
        
        // 移动到下一个槽
        wheel->current_slot = (wheel->current_slot + 1) % TIME_WHEEL_SIZE;
        
        // if (processed > 0) {
        //     LOG_INFO("槽位 %d 处理了 %d 个定时器", slot, processed);
        // }
    }
    
    // 只推进已处理步数对应的时间，不人为加速时间轮
    // 这样确保超时判定基于真实时间，避免因加速轮导致的假超时
    // 如果 steps=3，则只推进 300ms；剩余 elapsed 的部分在下次调用时重新计算
    wheel->last_check_time += (uint64_t)steps_to_process * TIME_UNIT_MS;
    
    return total_processed;
}
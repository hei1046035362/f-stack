#include "reactor.h"
#include "comm/log.hpp"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// 获取当前时间（毫秒），从nginx借鉴的高性能时间获取
static inline uint64_t get_current_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 初始化多级时间轮
int timer_wheel_init(timer_wheel_t* wheel) {
    if (!wheel) return -1;
    
    // 初始化各级时间轮大小
    wheel->level_sizes[0] = 256;   // 256ms * 256 = 65.5秒
    wheel->level_sizes[1] = 64;    // 16.4秒 * 64 = 17.5分钟
    wheel->level_sizes[2] = 64;    // 4.3分钟 * 64 = 4.6小时
    wheel->level_sizes[3] = 64;    // 1.1小时 * 64 = 3天
    
    // 分配各级时间轮内存
    for (int i = 0; i < MAX_TIMER_LEVELS; i++) {
        wheel->levels[i] = (timer_slot_t*)calloc(wheel->level_sizes[i], sizeof(timer_slot_t));
        if (!wheel->levels[i]) {
            // 清理已分配的内存
            for (int j = 0; j < i; j++) {
                free(wheel->levels[j]);
            }
            return -1;
        }
    }
    
    // 初始化映射表
    memset(wheel->nodes, 0, sizeof(wheel->nodes));
    wheel->current_time = get_current_ms();
    wheel->last_check = wheel->current_time;
    wheel->count = 0;
    
    return 0;
}

// 释放时间轮
void timer_wheel_free(timer_wheel_t* wheel) {
    for (int i = 0; i < MAX_TIMER_LEVELS; i++) {
        if (wheel->levels[i]) {
            // 释放每个槽中的节点
            for (int j = 0; j < wheel->level_sizes[i]; j++) {
                timer_node_t* node = wheel->levels[i][j].head;
                while (node) {
                    timer_node_t* next = node->next;
                    free(node);
                    node = next;
                }
            }
            free(wheel->levels[i]);
            wheel->levels[i] = NULL;
        }
    }
    memset(wheel->nodes, 0, sizeof(wheel->nodes));
    wheel->count = 0;
}

// 计算定时器应该放在哪一级
static int calculate_level(timer_wheel_t* wheel, uint64_t expire_ms) {
    uint64_t diff = expire_ms - wheel->current_time;
    
    if (diff < (1 << 8)) return 0;        // 0-255ms
    if (diff < (1 << 14)) return 1;       // 256ms-16.3秒
    if (diff < (1 << 20)) return 2;       // 16.4秒-4.6小时
    return 3;                             // 4.6小时以上
}

// 计算在指定层级的槽位
static int calculate_slot(timer_wheel_t* wheel, int level, uint64_t expire_ms) {
    uint64_t mask = (1ULL << (8 + 6 * level)) - 1;
    uint64_t diff = expire_ms - wheel->current_time;
    return (wheel->current_time + diff) & mask;
}

// 添加定时器（O(1)操作）
int timer_wheel_add(timer_wheel_t* wheel, int fd, uint64_t expire_ms) {
    if (fd < 0 || fd >= MAX_CLIENTS || wheel->nodes[fd]) {
        return -1;
    }
    
    // 创建节点
    timer_node_t* node = (timer_node_t*)calloc(1, sizeof(timer_node_t));
    if (!node) return -1;
    
    node->fd = fd;
    node->expire = expire_ms;
    
    // 计算应该放在哪一级
    int level = calculate_level(wheel, expire_ms);
    int slot = calculate_slot(wheel, level, expire_ms) % wheel->level_sizes[level];
    
    node->level = level;
    node->slot = slot;
    
    // 插入到对应槽的链表头部
    timer_slot_t* timer_slot = &wheel->levels[level][slot];
    if (timer_slot->head) {
        timer_slot->head->prev = node;
    }
    node->next = timer_slot->head;
    timer_slot->head = node;
    if (!timer_slot->tail) {
        timer_slot->tail = node;
    }
    
    // 保存映射
    wheel->nodes[fd] = node;
    wheel->count++;
    
    return 0;
}

// 移除定时器（O(1)操作）
int timer_wheel_remove(timer_wheel_t* wheel, int fd) {
    if (fd < 0 || fd >= MAX_CLIENTS || !wheel->nodes[fd]) {
        return 0;
    }
    
    timer_node_t* node = wheel->nodes[fd];
    
    // 从链表中移除
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        // 是头节点
        timer_slot_t* timer_slot = &wheel->levels[node->level][node->slot];
        timer_slot->head = node->next;
    }
    
    if (node->next) {
        node->next->prev = node->prev;
    } else {
        // 是尾节点
        timer_slot_t* timer_slot = &wheel->levels[node->level][node->slot];
        timer_slot->tail = node->prev;
    }
    
    // 清理
    free(node);
    wheel->nodes[fd] = NULL;
    wheel->count--;
    
    return 0;
}

// 更新定时器（先删后加）
int timer_wheel_update(timer_wheel_t* wheel, int fd, uint64_t expire_ms) {
    if (fd < 0 || fd >= MAX_CLIENTS) {
        return -1;
    }
    
    // 如果节点不存在，直接添加
    if (!wheel->nodes[fd]) {
        return timer_wheel_add(wheel, fd, expire_ms);
    }
    
    // 先移除旧的
    timer_wheel_remove(wheel, fd);
    
    // 再添加新的
    return timer_wheel_add(wheel, fd, expire_ms);
}

// 降级定时器（从高级时间轮移动到低级时间轮）
static void cascade_timers(timer_wheel_t* wheel, int level, int slot) {
    timer_slot_t* timer_slot = &wheel->levels[level][slot];
    timer_node_t* node = timer_slot->head;
    
    while (node) {
        timer_node_t* next = node->next;
        
        // 重新计算应该在哪一级
        int new_level = calculate_level(wheel, node->expire);
        int new_slot = calculate_slot(wheel, new_level, node->expire) % wheel->level_sizes[new_level];
        
        if (new_level != level || new_slot != slot) {
            // 从当前链表移除
            if (node->prev) {
                node->prev->next = node->next;
            } else {
                timer_slot->head = node->next;
            }
            if (node->next) {
                node->next->prev = node->prev;
            } else {
                timer_slot->tail = node->prev;
            }
            
            // 更新层级和槽位
            node->level = new_level;
            node->slot = new_slot;
            node->prev = NULL;
            node->next = NULL;
            
            // 插入到新的槽
            timer_slot_t* new_slot_ptr = &wheel->levels[new_level][new_slot];
            if (new_slot_ptr->head) {
                new_slot_ptr->head->prev = node;
            }
            node->next = new_slot_ptr->head;
            new_slot_ptr->head = node;
            if (!new_slot_ptr->tail) {
                new_slot_ptr->tail = node;
            }
        }
        
        node = next;
    }
}

// 处理过期定时器（nginx风格）
int timer_wheel_process(timer_wheel_t* wheel, 
                       void (*callback)(int fd, void* arg), 
                       void* arg) {
    uint64_t now = get_current_ms();
    uint64_t elapsed = now - wheel->last_check;
    
    if (elapsed < TIMER_GRANULARITY) {
        return 0;
    }
    
    wheel->current_time = now;
    int processed = 0;
    
    // 处理每一级时间轮
    for (int level = 0; level < MAX_TIMER_LEVELS; level++) {
        int steps = elapsed >> (8 + 6 * level);
        if (steps == 0) break;
        
        steps = RTE_MIN(steps, wheel->level_sizes[level]);
        
        for (int i = 0; i < steps; i++) {
            int slot = (wheel->current_time >> (8 + 6 * level)) % wheel->level_sizes[level];
            timer_slot_t* timer_slot = &wheel->levels[level][slot];
            
            // 处理当前槽的所有定时器
            timer_node_t* node = timer_slot->head;
            while (node) {
                timer_node_t* next = node->next;
                
                if (node->expire <= now) {
                    // 触发回调
                    if (callback) {
                        callback(node->fd, arg);
                    }
                    
                    // 从时间轮中移除
                    timer_wheel_remove(wheel, node->fd);
                    processed++;
                } else {
                    // 未超时，跳出循环（链表按时间排序）
                    break;
                }
                
                node = next;
            }
            
            // 降级当前槽中的定时器
            cascade_timers(wheel, level, slot);
            
            wheel->current_time += (1ULL << (8 + 6 * level));
        }
    }
    
    wheel->last_check = now;
    return processed;
}
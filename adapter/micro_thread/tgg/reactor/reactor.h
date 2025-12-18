#ifndef REACTOR_H
#define REACTOR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include "ff_api.h"
// #include "ff_uthread.h"
#include <time.h>
#include <sys/queue.h>  // 使用系统队列
#include <rte_common.h>
#include <rte_cycles.h>

#define TIME_WHEEL_SIZE 512
#define TIMER_GRANULARITY 1000   // 1秒粒度
#define MAX_TIMER_LEVELS 4       // 4级时间轮

#define MAX_EVENTS 4096
#define BUFFER_SIZE 4096
#define MAX_CLIENTS 1000000

#define DEFAULT_THREADS 1024
#define MAX_THREADS 1024*16

// 从nginx借鉴的定时器结构
typedef struct timer_node_s {
    int fd;
    uint64_t expire;            // 绝对超时时间（毫秒）
    struct timer_node_s* next;
    struct timer_node_s* prev;
    int slot;                   // 在时间轮中的槽位
    int level;                  // 在多级时间轮中的层级
} timer_node_t;

// 时间轮槽
typedef struct timer_slot_s {
    timer_node_t* head;
    timer_node_t* tail;
} timer_slot_t;

// 多级时间轮（借鉴nginx）
typedef struct timer_wheel_s {
    timer_slot_t* levels[MAX_TIMER_LEVELS];  // 多级时间轮
    int level_sizes[MAX_TIMER_LEVELS];       // 每级大小
    uint64_t current_time;                   // 当前时间（毫秒）
    uint64_t last_check;                     // 上次检查时间
    timer_node_t* nodes[MAX_CLIENTS];    // FD到节点的映射
    uint32_t count;                          // 定时器总数
} timer_wheel_t;


typedef enum {
    EVENT_UNKNOWN = 0,
    EVENT_READ = 0x01,
    EVENT_WRITE = 0x02,
    EVENT_ERROR = 0x04
} event_type_t;

typedef struct client_context_s {
    int fd;
    // char buffer[BUFFER_SIZE];
    // int buffer_len;
    // int write_len;
    // int total_read;
    // int total_write;
    unsigned int ip;
    unsigned short port;
    int idx;
    // ff_uthread_t *uthread;
} client_context_t;

typedef void (*event_callback_t)(int fd, event_type_t events, void *arg);

typedef struct reactor_event_s {
    int fd;
    event_type_t events;
    event_callback_t rcallback;
    event_callback_t wcallback;
    void *arg;
    int active;
    // 新增：超时管理
    uint64_t last_active;      // 最后活动时间
} reactor_event_t;

typedef struct reactor_s {
    reactor_event_t *events;
    int max_events;
    int epoll_fd;
    void* pthread;
    void* data;
    int running;

    // 定时器管理
    timer_wheel_t timer_wheel;
    int timeout_ms;            // 超时时间（毫秒）
    
    // 性能统计
    struct {
        uint64_t timer_adds;
        uint64_t timer_updates;
        uint64_t timer_removes;
        uint64_t timer_expires;
        uint64_t timer_ticks;
    } stats;
} reactor_t;

// extern reactor_t[] g_reactor;

int reactor_create(int max_events, int timeout);
int reactor_destroy();
int reactor_add_event(int fd, event_type_t events, 
                     event_callback_t callback, event_callback_t wcallback,
                      void *arg, int timeout_seconds);
int reactor_modify_event(int fd, event_type_t events);
int reactor_remove_event(int fd);
void reactor_run(void* data);
void reactor_stop();

// 超时管理函数
int timer_wheel_init(timer_wheel_t* wheel);
int timer_wheel_add(timer_wheel_t* wheel, int fd, uint64_t expire_ms);
int timer_wheel_update(timer_wheel_t* wheel, int fd, uint64_t expire_time);
int timer_wheel_remove(timer_wheel_t* wheel, int fd);
int timer_wheel_process(timer_wheel_t* wheel, 
                       void (*callback)(int fd, void* arg), 
                       void* arg);
void timer_wheel_free(timer_wheel_t* wheel);

int reactor_set_timeout(int fd, int timeout_seconds);
int reactor_update_activity(int fd);
void reactor_check_timers();

#endif
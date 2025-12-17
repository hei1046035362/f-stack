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

#define MAX_EVENTS 1024
#define BUFFER_SIZE 4096
#define MAX_CLIENTS 1000000

#define DEFAULT_THREADS 1024
#define MAX_THREADS 1024*16

// 超时节点结构
typedef struct timeout_node {
    int fd;            // 文件描述符
    time_t expire_time; // 超时时间戳
    int heap_idx;      // 在堆中的位置
} timeout_node_t;

// 超时最小堆
typedef struct timeout_heap {
    timeout_node_t* nodes;  // 堆节点数组
    int capacity;           // 堆容量
    int size;              // 当前大小
} timeout_heap_t;

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
    time_t last_active;      // 最后活动时间
} reactor_event_t;

typedef struct reactor_s {
    reactor_event_t *events;
    int max_events;
    int epoll_fd;
    void* pthread;
    void* data;
    int running;
    // 超时管理
    timeout_heap_t timeout_heap;  // 超时最小堆
    int timeout_seconds;          // 超时时间（秒），可配置
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
int reactor_set_timeout(int fd, int timeout_seconds);
int reactor_update_activity(int fd);
void reactor_check_timeouts();

#endif
#include "reactor.h"
#include <rte_common.h>
#include "ff_epoll.h"
#include "mt_api.h"
#include "comm/log.hpp"
#include <errno.h>
#include <sys/fcntl.h>

// typedef struct reactors_s {
//     reactor_event_t *events;
//     reactor_t *reactors;
//     int thread_count;
//     int mask;
//     int max_events;
//     int running;
// } reactors_t;

reactor_t g_reactor;

// 内部函数声明
static int timeout_heap_init(timeout_heap_t* heap, int capacity);
static void timeout_heap_free(timeout_heap_t* heap);
static int timeout_heap_push(timeout_heap_t* heap, int fd, time_t expire_time);
static int timeout_heap_pop(timeout_heap_t* heap);
static int timeout_heap_remove(timeout_heap_t* heap, int fd);
static int timeout_heap_update(timeout_heap_t* heap, int fd, time_t expire_time);
static void timeout_heap_shift_up(timeout_heap_t* heap, int idx);
static void timeout_heap_shift_down(timeout_heap_t* heap, int idx);

// 重载位操作运算符
inline event_type_t operator|(event_type_t lhs, event_type_t rhs) {
    return static_cast<event_type_t>(static_cast<int>(lhs) | static_cast<int>(rhs));
}

inline event_type_t& operator|=(event_type_t& lhs, event_type_t rhs) {
    lhs = lhs | rhs;
    return lhs;
}

// 超时堆初始化
static int timeout_heap_init(timeout_heap_t* heap, int capacity) {
    heap->nodes = (timeout_node_t*)calloc(capacity, sizeof(timeout_node_t));
    if (!heap->nodes) {
        LOG_ERROR("Failed to allocate timeout heap");
        return -1;
    }
    
    heap->capacity = capacity;
    heap->size = 0;
    
    // 初始化所有节点的 heap_idx 为 -1
    for (int i = 0; i < capacity; i++) {
        heap->nodes[i].heap_idx = -1;
    }
    
    return 0;
}

// 释放超时堆
static void timeout_heap_free(timeout_heap_t* heap) {
    if (heap->nodes) {
        free(heap->nodes);
        heap->nodes = NULL;
    }
    heap->capacity = 0;
    heap->size = 0;
}

// 堆上浮调整
static void timeout_heap_shift_up(timeout_heap_t* heap, int idx) {
    while (idx > 0) {
        int parent = (idx - 1) / 2;
        if (heap->nodes[parent].expire_time <= heap->nodes[idx].expire_time) {
            break;
        }
        
        // 交换节点
        timeout_node_t temp = heap->nodes[parent];
        heap->nodes[parent] = heap->nodes[idx];
        heap->nodes[idx] = temp;
        
        // 更新索引
        heap->nodes[parent].heap_idx = parent;
        heap->nodes[idx].heap_idx = idx;
        
        idx = parent;
    }
}

// 堆下沉调整
static void timeout_heap_shift_down(timeout_heap_t* heap, int idx) {
    int size = heap->size;
    timeout_node_t* nodes = heap->nodes;
    
    while (idx * 2 + 1 < size) {
        int left = idx * 2 + 1;
        int right = left + 1;
        int smallest = idx;
        
        if (left < size && nodes[left].expire_time < nodes[smallest].expire_time) {
            smallest = left;
        }
        
        if (right < size && nodes[right].expire_time < nodes[smallest].expire_time) {
            smallest = right;
        }
        
        if (smallest == idx) {
            break;
        }
        
        // 交换节点
        timeout_node_t temp = nodes[smallest];
        nodes[smallest] = nodes[idx];
        nodes[idx] = temp;
        
        // 更新索引
        nodes[smallest].heap_idx = smallest;
        nodes[idx].heap_idx = idx;
        
        idx = smallest;
    }
}

// 添加节点到堆
static int timeout_heap_push(timeout_heap_t* heap, int fd, time_t expire_time) {
    if (heap->size >= heap->capacity) {
        LOG_ERROR("Timeout heap is full, capacity: %d", heap->capacity);
        return -1;
    }
    
    int idx = heap->size;
    heap->nodes[idx].fd = fd;
    heap->nodes[idx].expire_time = expire_time;
    heap->nodes[idx].heap_idx = idx;
    heap->size++;
    
    timeout_heap_shift_up(heap, idx);
    return 0;
}

// 删除堆顶节点
static int timeout_heap_pop(timeout_heap_t* heap) {
    if (heap->size <= 0) {
        return -1;
    }
    
    int fd = heap->nodes[0].fd;
    heap->nodes[0].heap_idx = -1;
    
    heap->size--;
    if (heap->size > 0) {
        heap->nodes[0] = heap->nodes[heap->size];
        heap->nodes[0].heap_idx = 0;
        timeout_heap_shift_down(heap, 0);
    }
    
    return fd;
}

// 删除指定fd的节点
static int timeout_heap_remove(timeout_heap_t* heap, int fd) {
    if (fd < 0 || fd >= heap->capacity) {
        return -1;
    }
    
    // 查找fd在堆中的位置
    for (int i = 0; i < heap->size; i++) {
        if (heap->nodes[i].fd == fd) {
            int idx = i;
            heap->nodes[idx].heap_idx = -1;
            
            heap->size--;
            if (heap->size > 0 && idx < heap->size) {
                heap->nodes[idx] = heap->nodes[heap->size];
                heap->nodes[idx].heap_idx = idx;
                
                // 需要上浮或下沉调整
                if (idx > 0 && heap->nodes[idx].expire_time < heap->nodes[(idx-1)/2].expire_time) {
                    timeout_heap_shift_up(heap, idx);
                } else {
                    timeout_heap_shift_down(heap, idx);
                }
            }
            return 0;
        }
    }
    
    return -1;  // 未找到
}

// 更新节点的超时时间
static int timeout_heap_update(timeout_heap_t* heap, int fd, time_t expire_time) {
    if (fd < 0 || fd >= heap->capacity) {
        return -1;
    }
    
    // 查找fd在堆中的位置
    for (int i = 0; i < heap->size; i++) {
        if (heap->nodes[i].fd == fd) {
            time_t old_expire = heap->nodes[i].expire_time;
            heap->nodes[i].expire_time = expire_time;
            
            if (expire_time < old_expire) {
                timeout_heap_shift_up(heap, i);
            } else {
                timeout_heap_shift_down(heap, i);
            }
            return 0;
        }
    }
    
    // 如果未找到，则添加新节点
    return timeout_heap_push(heap, fd, expire_time);
}

int reactor_create(int max_events, int timeout) {
    // 调整线程数
    // thread_count = rte_align32pow2(thread_count + 1);
    // if (thread_count < DEFAULT_THREADS) {
    //     thread_count = DEFAULT_THREADS;
    // } else if (thread_count > MAX_THREADS) {
    //     thread_count = MAX_THREADS;
    // }

    // g_reactor.reactors = (reactor_t *)malloc(sizeof(reactor_t) * thread_count);
    // if (!g_reactor.reactors) {
    //     return 0;
    // }
    g_reactor.events = (reactor_event_t *)calloc(max_events, sizeof(reactor_event_t));
    if (!g_reactor.events) {
        LOG_ERROR("Failed to allocate events array");
        goto create_reactor_failed;
    }

    // for(int i = 0; i < thread_count; i++) {

        g_reactor.epoll_fd = ff_epoll_create(0);
        if (g_reactor.epoll_fd < 0) {
            LOG_ERROR("Failed to create epoll instance");
            goto create_reactor_failed;
        }
    // 初始化超时堆
    if (timeout_heap_init(&g_reactor.timeout_heap, max_events) < 0) {
        LOG_ERROR("Failed to initialize timeout heap");
        goto create_reactor_failed;
    }
    
    g_reactor.timeout_seconds = timeout;  // 默认60秒超时
        g_reactor.running = 0;
    // }
    g_reactor.max_events = max_events;
    // for(int i = 0; i < thread_count; i++) {
        // g_reactor.data = (int*) malloc(sizeof(int));
        // if(!g_reactor.data) {
        //     goto create_reactor_failed;
        // }
        // *((int*)g_reactor.reactors[i].data) = i;
        g_reactor.pthread = NS_MICRO_THREAD::mt_start_thread((void *)reactor_run, NULL);
        if (!g_reactor.pthread ) {
            LOG_ERROR("Failed to start reactor thread");
            goto create_reactor_failed;
        }
    // }

    return 1;

// 初始化失败
create_reactor_failed:
    LOG_ERROR("create reactors failed.");
    // if(g_reactor.reactors) {
        // for(int i = 0; i < thread_count; i++) {
            if (g_reactor.epoll_fd >= 0) {
                ff_close(g_reactor.epoll_fd);
            }
            // if(g_reactor.data) {
            //     free(g_reactor.data);
            //     g_reactor.data = NULL;
            // }
            // if (!g_reactor.reactors[i].pthread ) {
            //     free(g_reactor.reactors[i].pthread);
            //     g_reactor.reactors[i].pthread = NULL;
            // }
        // }
        if (g_reactor.events) {
            free(g_reactor.events);
            g_reactor.events = NULL;
        }
        timeout_heap_free(&g_reactor.timeout_heap);
        // free(g_reactor.reactors);
        // g_reactor.reactors = NULL;
        g_reactor.running = 0;
    // }
    return 0;
}

int reactor_destroy() {
    // if (!g_reactor.reactors) return -1;

    // for(int i = 0; i < g_reactor.thread_count; i++) {
        if (g_reactor.epoll_fd >= 0) {
            ff_close(g_reactor.epoll_fd);
        }

    // }
    if (g_reactor.events) {
        free(g_reactor.events);
        g_reactor.events = NULL;
    }
    // free(g_reactor.reactors);
    return 0;
}

int reactor_add_event(int fd, event_type_t events, 
                     event_callback_t rcallback, event_callback_t wcallback,
                     void *arg, int timeout_seconds) {
    if (fd < 0 || fd >= g_reactor.max_events) {
        LOG_ERROR("check param failed: fd:%d max_events:%d", fd, g_reactor.max_events);
        return -1;
    }
    
    // int idx = fd & g_reactor.mask;
        
    // 检查epoll_fd是否有效
    if (g_reactor.epoll_fd < 0) {
        LOG_ERROR("invalid epoll_fd: %d", g_reactor.epoll_fd);
        return -1;
    }
        
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    
    if (events & EVENT_READ) {
        ev.events |= EPOLLIN;
    }
    if (events & EVENT_WRITE) {
        ev.events |= EPOLLOUT;
    }
    
    ev.data.fd = fd;
    
    int ret = ff_epoll_ctl(g_reactor.epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    if (ret < 0) {
        LOG_ERROR("ff_epoll_ctl add event failed: epoll_fd=%d, target_fd=%d, error: %s\n", 
               g_reactor.epoll_fd, fd, strerror(errno));
        return -1;
    }
    
    time_t now = time(NULL);

    g_reactor.events[fd].fd = fd;
    g_reactor.events[fd].events = events;
    g_reactor.events[fd].rcallback = rcallback;
    g_reactor.events[fd].wcallback = wcallback;
    g_reactor.events[fd].arg = arg;
    g_reactor.events[fd].active = 1;
    g_reactor.events[fd].last_active = now;
    
    int timeout = (timeout_seconds > 0) ? timeout_seconds : g_reactor.timeout_seconds;
    if (timeout > 0) {
        timeout_heap_update(&g_reactor.timeout_heap, fd, now + timeout);
    }
    return 0;
}

int reactor_modify_event(int fd, event_type_t events) {
    // int idx = fd & g_reactor.thread_count;
    if (fd < 0 || fd >= g_reactor.max_events || !g_reactor.events[fd].active) {
        LOG_ERROR("check param failed: fd:%d max_events:%d", fd, g_reactor.max_events);
        return -1;
    }
    
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    
    if (events & EVENT_READ) {
        ev.events |= EPOLLIN;
    }
    if (events & EVENT_WRITE) {
        ev.events |= EPOLLOUT;
    }
    
    ev.data.fd = fd;
    
    if (ff_epoll_ctl(g_reactor.epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        return -1;
    }
    
    g_reactor.events[fd].events = events;
    return 0;
}

int reactor_remove_event(int fd) {
    // int idx = fd & g_reactor.thread_count;
    if (fd < 0 || fd >= g_reactor.max_events || !g_reactor.events[fd].active) {
        LOG_ERROR("check param failed: fd:%d max_events:%d", fd, g_reactor.max_events);
        return -1;
    }
    
    if (ff_epoll_ctl(g_reactor.epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        return -1;
    }
    
    // 从超时堆中移除
    timeout_heap_remove(&g_reactor.timeout_heap, fd);

    g_reactor.events[fd].active = 0;
    return 0;
}

void reactor_run(void* data) {
    // int idx = *((int*)data);
    // if (!g_reactor.reactors) return;

    g_reactor.running = 1;

    struct epoll_event events[MAX_EVENTS];

    // 上一次检查超时的时间
    static time_t last_timeout_check = 0;
    
    while (g_reactor.running) {
        int nfds = ff_epoll_wait(g_reactor.epoll_fd, events, MAX_EVENTS, 1000);

        // 每秒检查一次超时
        time_t now = time(NULL);
        if (now - last_timeout_check >= 1) {
            reactor_check_timeouts();
            last_timeout_check = now;
        }

        if(!nfds) {
            NS_MICRO_THREAD::mt_sleep(1);
            continue;
        }
        
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            
            if (fd < 0 || fd >= g_reactor.max_events || !g_reactor.events[fd].active) {
                continue;
            }
            
            event_type_t revents = static_cast<event_type_t>(0);
            if (events[i].events & EPOLLIN) {
                revents |= EVENT_READ;
                // 更新活动时间
                reactor_update_activity(fd);
                g_reactor.events[fd].rcallback(fd, revents, g_reactor.events[fd].arg);
            }
            if (events[i].events & EPOLLOUT) {
                revents |= EVENT_WRITE;
                reactor_update_activity(fd);
                g_reactor.events[fd].wcallback(fd, revents, g_reactor.events[fd].arg);
            }
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                LOG_WARNING("connection error, fd=%d", fd);
                revents |= EVENT_ERROR;
                g_reactor.events[fd].rcallback(fd, revents, g_reactor.events[fd].arg);
            }
        }
    }

    if (g_reactor.epoll_fd >= 0) {
        ff_close(g_reactor.epoll_fd);
    }

    if (g_reactor.events) {
        free(g_reactor.events);
        g_reactor.events = NULL;
    }
    // 释放超时堆
    timeout_heap_free(&g_reactor.timeout_heap);

}

void reactor_stop() {
    g_reactor.running = 0;
}

// 更新连接的活动时间
int reactor_update_activity(int fd) {
    if (fd < 0 || fd >= g_reactor.max_events || !g_reactor.events[fd].active) {
        return -1;
    }
    
    time_t now = time(NULL);
    g_reactor.events[fd].last_active = now;
    
    // 更新超时堆中的超时时间
    if (g_reactor.timeout_seconds > 0) {
        timeout_heap_update(&g_reactor.timeout_heap, fd, now + g_reactor.timeout_seconds);
    }
    
    return 0;
}

// 设置连接超时时间
int reactor_set_timeout(int fd, int timeout_seconds) {
    if (fd < 0 || fd >= g_reactor.max_events || !g_reactor.events[fd].active) {
        return -1;
    }
    
    if (timeout_seconds <= 0) {
        // 清除超时
        timeout_heap_remove(&g_reactor.timeout_heap, fd);
        return 0;
    }
    
    time_t now = time(NULL);
    g_reactor.events[fd].last_active = now;
    
    return timeout_heap_update(&g_reactor.timeout_heap, fd, now + timeout_seconds);
}

// 关闭超时连接
static void close_timeout_connection(int fd) {
    LOG_WARNING("connect timeout:%d fd=%d.", g_reactor.timeout_seconds, fd);
    
    // 调用用户回调函数通知连接关闭
    if (g_reactor.events[fd].active && g_reactor.events[fd].rcallback) {
        g_reactor.events[fd].rcallback(fd, EVENT_ERROR, g_reactor.events[fd].arg);
    }
    
    // 从reactor中移除事件 rcallback中会执行移除操作
    // reactor_remove_event(fd);
    
    // 关闭文件描述符
    // close(fd);
}

// 检查并处理超时连接
void reactor_check_timeouts() {
    time_t now = time(NULL);
    
    // 检查堆顶元素是否超时
    while (g_reactor.timeout_heap.size > 0) {
        timeout_node_t* top = &g_reactor.timeout_heap.nodes[0];
        
        if (top->expire_time > now) {
            break;  // 堆顶未超时，后面的更不会超时
        }
        
        int fd = top->fd;
        
        // 验证连接是否仍然活跃
        if (fd >= 0 && fd < g_reactor.max_events && g_reactor.events[fd].active) {
            // 再次确认是否真的超时
            if (now - g_reactor.events[fd].last_active >= g_reactor.timeout_seconds) {
                close_timeout_connection(fd);
            } else {
                // 更新超时时间
                timeout_heap_update(&g_reactor.timeout_heap, fd, 
                                   g_reactor.events[fd].last_active + g_reactor.timeout_seconds);
            }
        } else {
            // 连接已不存在，从堆中移除
            timeout_heap_pop(&g_reactor.timeout_heap);
        }
    }
}
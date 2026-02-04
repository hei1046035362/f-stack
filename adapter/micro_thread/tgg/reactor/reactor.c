#include "reactor.h"
#include <rte_common.h>
#include "ff_epoll.h"
#include "mt_api.h"
#include "comm/log.hpp"
#include <errno.h>
#include <sys/fcntl.h>
#include <sys/timerfd.h>

// typedef struct reactors_s {
//     reactor_event_t *events;
//     reactor_t *reactors;
//     int thread_count;
//     int mask;
//     int max_events;
//     int running;
// } reactors_t;

reactor_t g_reactor;

// 全局缓存的时间戳,减少系统调用
static __thread uint64_t g_cached_time_ms = 0;

// 更新缓存时间(每帧调用一次)
static inline void update_cached_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_cached_time_ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 获取缓存的时间(性能优化)
static inline uint64_t get_current_ms() {
    // 如果未初始化,先更新
    if (unlikely(g_cached_time_ms == 0)) {
        update_cached_time();
    }
    return g_cached_time_ms;
}

// 当需要精确时间时使用此函数
static inline uint64_t get_current_ms_precise() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
// 定时器回调函数
static void on_timer_expired(int fd, void* arg) {
    reactor_t* reactor = (reactor_t*)arg;
    
    if (fd < 0 || fd >= reactor->max_events) {
        return;
    }
    
    reactor_event_t* event = &reactor->events[fd];
    if (!event->active) {
        // LOG_WARNING("定时器触发但连接已关闭: fd=%d", fd);
        return;
    }
    
    LOG_WARNING("time expired: fd=%d, timeout=%dms", fd, reactor->timeout_ms);
    
    // 调用用户回调
    if (event->rcallback) {
        event->rcallback(fd, EVENT_ERROR, event->arg);
    }
    
    reactor->stats.timer_expires++;
}

// 重载位操作运算符
inline event_type_t operator|(event_type_t lhs, event_type_t rhs) {
    return static_cast<event_type_t>(static_cast<int>(lhs) | static_cast<int>(rhs));
}

inline event_type_t& operator|=(event_type_t& lhs, event_type_t rhs) {
    lhs = lhs | rhs;
    return lhs;
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
    // int timer_fd = -1;
    // struct itimerspec timer_spec = {0};
    // struct epoll_event ev;
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
    // 初始化时间轮
    if (timer_wheel_init(&g_reactor.timer_wheel) < 0) {
        LOG_ERROR("init timer wheel failed.");
        goto create_reactor_failed;
    }

    g_reactor.timeout_ms = timeout * 1000;  // 默认60秒超时
        g_reactor.running = 0;
    // }
    g_reactor.max_events = max_events;

    // 初始化统计
    memset(&g_reactor.stats, 0, sizeof(g_reactor.stats));

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
        timer_wheel_free(&g_reactor.timer_wheel);
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
    // 释放时间轮中的节点
    timer_wheel_free(&g_reactor.timer_wheel);
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

    uint64_t now = get_current_ms();

    g_reactor.events[fd].fd = fd;
    g_reactor.events[fd].events = events;
    g_reactor.events[fd].rcallback = rcallback;
    g_reactor.events[fd].wcallback = wcallback;
    g_reactor.events[fd].arg = arg;
    g_reactor.events[fd].active = 1;
    g_reactor.events[fd].last_active = now;
    
    uint64_t expire_ms = now + (timeout_seconds > 0 ? timeout_seconds * 1000 : g_reactor.timeout_ms);
    // 添加定时器
    if (expire_ms > now) {
        timer_wheel_add(&g_reactor.timer_wheel, fd, expire_ms);
        g_reactor.stats.timer_adds++;
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
    
    int ret = ff_epoll_ctl(g_reactor.epoll_fd, EPOLL_CTL_DEL, fd, NULL);

    // 从时间轮移除（无论 epoll_ctl 成功与否，都应从时间轮移除并将事件标记为不激活）
    timer_wheel_remove(&g_reactor.timer_wheel, fd);
    g_reactor.stats.timer_removes++;

    g_reactor.events[fd].active = 0;

    if (ret < 0) {
        return -1;
    }
    return 0;
}

void reactor_run(void* data) {
    g_reactor.running = 1;

    struct epoll_event events[MAX_EVENTS];

    // 上一次检查超时的时间
    static uint64_t last_timer_check = get_current_ms();
    
    while (g_reactor.running) {
        // 每次循环开始更新缓存时间,减少系统调用
        update_cached_time();
        uint64_t now = g_cached_time_ms;
        
        // 动态计算超时时间
        int next_timeout;
        if (g_reactor.timer_wheel.count > 0 && g_reactor.timer_wheel.next_expire_time > 0) {
            // 有定时器,计算到下一个定时器的时间
            int64_t time_to_expire = (int64_t)(g_reactor.timer_wheel.next_expire_time - now);
            if (time_to_expire <= 0) {
                next_timeout = 0;  // 立即检查
            } else if (time_to_expire < 100) {
                next_timeout = 1;  // 小于100ms,最小1ms
            } else {
                next_timeout = (int)time_to_expire;  // 精确等待
            }
        } else {
            next_timeout = 1000;  // 无定时器,默认1秒
        }
        
        // 检查定时器
        if (now - last_timer_check >= 100) {  // 100ms检查一次
            reactor_check_timers(now);
            last_timer_check = now;
        }

        int nfds = ff_epoll_wait(g_reactor.epoll_fd, events, MAX_EVENTS, next_timeout);
        if (nfds <= 0) {
            if (nfds < 0 && errno != EINTR) {
                LOG_ERROR("epoll_wait ERROR(%d): %s", errno, strerror(errno));
            }
            NS_MICRO_THREAD::mt_sleep(1);
            continue;
        }
        
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            
            if (fd < 0 || fd >= g_reactor.max_events || !g_reactor.events[fd].active) {
                continue;
            }
            event_type_t revents = static_cast<event_type_t>(0);

            // 区分连接关闭（EPOLLHUP）和真正的错误（EPOLLERR）
            if (events[i].events & EPOLLHUP) {
                // 对方关闭连接或本端关闭（正常关闭），用 EVENT_HUP 标记
                LOG_INFO("connection hangup (remote closed), fd=%d", fd);
                revents |= EVENT_HUP;
                g_reactor.events[fd].rcallback(fd, revents, g_reactor.events[fd].arg);
                continue;
            }
            
            if (events[i].events & EPOLLERR) {
                // 真正的连接错误
                LOG_ERROR("connection error, fd=%d", fd);
                revents |= EVENT_ERROR;
                g_reactor.events[fd].rcallback(fd, revents, g_reactor.events[fd].arg);
                continue;
            }            

            if (events[i].events & EPOLLIN) {
                revents |= EVENT_READ;
                g_reactor.events[fd].rcallback(fd, revents, g_reactor.events[fd].arg);
            }
            if (events[i].events & EPOLLOUT) {
                revents |= EVENT_WRITE;
                // reactor_update_activity(fd);
                g_reactor.events[fd].wcallback(fd, revents, g_reactor.events[fd].arg);
            }
        }
        // 调试打印reactor的相关统计信息
        // static uint64_t last_stat_time = 0;
        // if (now - last_stat_time >= 10000) {
        //     LOG_DEBUG("定时器统计: 总数=%u, 添加=%lu, 更新=%lu, 移除=%lu, 超时=%lu, 检查次数=%lu",
        //             g_reactor.timer_wheel.count,
        //             g_reactor.stats.timer_adds,
        //             g_reactor.stats.timer_updates,
        //             g_reactor.stats.timer_removes,
        //             g_reactor.stats.timer_expires,
        //             g_reactor.stats.timer_ticks);
        //     last_stat_time = now;
        // }
    }

    if (g_reactor.epoll_fd >= 0) {
        ff_close(g_reactor.epoll_fd);
    }

    if (g_reactor.events) {
        free(g_reactor.events);
        g_reactor.events = NULL;
    }
    // 释放超时堆
    // timeout_heap_free(&g_reactor.timeout_heap);

}

void reactor_stop() {
    g_reactor.running = 0;
}

// 更新连接的活动时间
int reactor_update_activity(int fd) {
    if (fd < 0 || fd >= g_reactor.max_events || !g_reactor.events[fd].active) {
        return -1;
    }
    
    uint64_t now = get_current_ms();
    g_reactor.events[fd].last_active = now;
    
    // 更新定时器
    if (g_reactor.timeout_ms > 0) {
        uint64_t expire_ms = now + g_reactor.timeout_ms;
        timer_wheel_update(&g_reactor.timer_wheel, fd, expire_ms);
        g_reactor.stats.timer_updates++;
    }
    
    return 0;
}

// 设置连接超时时间
int reactor_set_timeout(int fd, int timeout_seconds) {
    if (fd < 0 || fd >= g_reactor.max_events || !g_reactor.events[fd].active) {
        return -1;
    }
    
    if (timeout_seconds <= 0) {
        timer_wheel_remove(&g_reactor.timer_wheel, fd);
        g_reactor.stats.timer_removes++;
        return 0;
    }
    
    uint64_t now = get_current_ms();
    g_reactor.events[fd].last_active = now;
    
    uint64_t expire_ms = now + timeout_seconds * 1000;
    int ret = timer_wheel_update(&g_reactor.timer_wheel, fd, expire_ms);
    if (ret < 0) {
        return -1;
    }
    
    g_reactor.stats.timer_updates++;
    return 0;
}

void reactor_check_timers(uint64_t now) {
    int processed = timer_wheel_process(&g_reactor.timer_wheel, on_timer_expired, &g_reactor, now);
    g_reactor.stats.timer_ticks++;
    
    if (processed > 0) {
        LOG_DEBUG("processed %d timerout", processed);
    }
}
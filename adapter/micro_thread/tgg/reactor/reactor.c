#include "reactor.h"
#include <rte_common.h>
#include "ff_epoll.h"
#include "mt_api.h"
#include "comm/log.hpp"
#include <errno.h>
#include <sys/fcntl.h>
// int g_thread_count = DEFAULT_THREADS;
// int g_mask = 

typedef struct reactors_s {
    reactor_event_t *events;
    reactor_t *reactors;
    int thread_count;
    int mask;
    int max_events;
    int running;
} reactors_t;

reactors_t g_reactor;

// 重载位操作运算符
inline event_type_t operator|(event_type_t lhs, event_type_t rhs) {
    return static_cast<event_type_t>(static_cast<int>(lhs) | static_cast<int>(rhs));
}

inline event_type_t& operator|=(event_type_t& lhs, event_type_t rhs) {
    lhs = lhs | rhs;
    return lhs;
}

int reactor_create(int max_events, int thread_count) {
    // 调整线程数
    thread_count = rte_align32pow2(thread_count + 1);
    if (thread_count < DEFAULT_THREADS) {
        thread_count = DEFAULT_THREADS;
    } else if (thread_count > MAX_THREADS) {
        thread_count = MAX_THREADS;
    }

    g_reactor.reactors = (reactor_t *)malloc(sizeof(reactor_t) * thread_count);
    if (!g_reactor.reactors) {
        return 0;
    }
    g_reactor.events = (reactor_event_t *)calloc(max_events, sizeof(reactor_event_t));
    if (!g_reactor.events) {
        goto create_reactor_failed;
    }

    for(int i = 0; i < thread_count; i++) {

        g_reactor.reactors[i].epoll_fd = ff_epoll_create(0);
        if (g_reactor.reactors[i].epoll_fd < 0) {
            goto create_reactor_failed;
        }

        g_reactor.running = 0;
    }
    g_reactor.max_events = max_events * thread_count;
    g_reactor.thread_count = thread_count;
    g_reactor.mask = thread_count - 1;
    for(int i = 0; i < thread_count; i++) {
        g_reactor.reactors[i].data = (int*) malloc(sizeof(int));
        if(!g_reactor.reactors[i].data) {
            goto create_reactor_failed;
        }
        *((int*)g_reactor.reactors[i].data) = i;
        g_reactor.reactors[i].pthread = NS_MICRO_THREAD::mt_start_thread((void *)reactor_run, g_reactor.reactors[i].data);
        if (!g_reactor.reactors[i].pthread ) {
            goto create_reactor_failed;
        }
    }

    return 1;

// 初始化失败
create_reactor_failed:
    LOG_ERROR("create reactors failed.");
    if(g_reactor.reactors) {
        for(int i = 0; i < thread_count; i++) {
            if (g_reactor.reactors[i].epoll_fd >= 0) {
                ff_close(g_reactor.reactors[i].epoll_fd);
            }
            if(g_reactor.reactors[i].data) {
                free(g_reactor.reactors[i].data);
                g_reactor.reactors[i].data = NULL;
            }
            // if (!g_reactor.reactors[i].pthread ) {
            //     free(g_reactor.reactors[i].pthread);
            //     g_reactor.reactors[i].pthread = NULL;
            // }
        }
        if (g_reactor.events) {
            free(g_reactor.events);
            g_reactor.events = NULL;
        }
        free(g_reactor.reactors);
        g_reactor.reactors = NULL;
        g_reactor.running = 0;
    }
    return 0;
}

int reactor_destroy() {
    if (!g_reactor.reactors) return -1;

    for(int i = 0; i < g_reactor.thread_count; i++) {
        if (g_reactor.reactors[i].epoll_fd >= 0) {
            ff_close(g_reactor.reactors[i].epoll_fd);
        }

        if(g_reactor.reactors[i].data) {
            free(g_reactor.reactors[i].data);
            g_reactor.reactors[i].data = NULL;
        }
    }
    if (g_reactor.events) {
        free(g_reactor.events);
        g_reactor.events = NULL;
    }
    free(g_reactor.reactors);
    return 0;
}

int reactor_add_event(int fd, event_type_t events, 
                     event_callback_t rcallback, event_callback_t wcallback, void *arg) {
    if (!g_reactor.reactors || fd < 0 || fd >= g_reactor.max_events) {
        LOG_ERROR("check param failed: reactors:%p fd:%d max_events:%d", 
               g_reactor.reactors, fd, g_reactor.max_events);
        return -1;
    }
    
    int idx = fd & g_reactor.mask;
        
    // 检查epoll_fd是否有效
    if (g_reactor.reactors[idx].epoll_fd < 0) {
        LOG_ERROR("invalid epoll_fd: %d", g_reactor.reactors[idx].epoll_fd);
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
    
    int ret = ff_epoll_ctl(g_reactor.reactors[idx].epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    if (ret < 0) {
        LOG_ERROR("ff_epoll_ctl add event failed: epoll_fd=%d, target_fd=%d, error: %s\n", 
               g_reactor.reactors[idx].epoll_fd, fd, strerror(errno));
        return -1;
    }
    
    g_reactor.events[fd].fd = fd;
    g_reactor.events[fd].events = events;
    g_reactor.events[fd].rcallback = rcallback;
    g_reactor.events[fd].wcallback = wcallback;
    g_reactor.events[fd].arg = arg;
    g_reactor.events[fd].active = 1;
    
    return 0;
}

int reactor_modify_event(int fd, event_type_t events) {
    int idx = fd & g_reactor.thread_count;
    if (!g_reactor.reactors || fd < 0 || fd >= g_reactor.max_events || !g_reactor.events[fd].active) {
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
    
    if (ff_epoll_ctl(g_reactor.reactors[idx].epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        return -1;
    }
    
    g_reactor.events[fd].events = events;
    return 0;
}

int reactor_remove_event(int fd) {
    int idx = fd & g_reactor.thread_count;
    if (!g_reactor.reactors || fd < 0 || fd >= g_reactor.max_events || !g_reactor.events[fd].active) {
        return -1;
    }
    
    if (ff_epoll_ctl(g_reactor.reactors[idx].epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        return -1;
    }
    
    g_reactor.events[fd].active = 0;
    return 0;
}

void reactor_run(void* data) {
    int idx = *((int*)data);
    if (!g_reactor.reactors || idx < 0 || idx > g_reactor.thread_count) return;

    g_reactor.running = 1;

    struct epoll_event events[MAX_EVENTS];
    
    while (g_reactor.running) {
        int nfds = ff_epoll_wait(g_reactor.reactors[idx].epoll_fd, events, MAX_EVENTS, 1000);

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
                g_reactor.events[fd].rcallback(fd, revents, g_reactor.events[fd].arg);
            }
            if (events[i].events & EPOLLOUT) {
                revents |= EVENT_WRITE;
                g_reactor.events[fd].wcallback(fd, revents, g_reactor.events[fd].arg);
            }
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                revents |= EVENT_ERROR;
                g_reactor.events[fd].rcallback(fd, revents, g_reactor.events[fd].arg);
            }
        }
    }
}

void reactor_stop() {
        g_reactor.running = 0;
}
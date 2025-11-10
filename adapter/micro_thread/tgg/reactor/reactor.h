#ifndef REACTOR_H
#define REACTOR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include "ff_api.h"
// #include "ff_uthread.h"

#define MAX_EVENTS 1024
#define BUFFER_SIZE 4096
#define MAX_CLIENTS 1000

#define DEFAULT_THREADS 1024
#define MAX_THREADS 1024*16


typedef enum {
    EVENT_UNKNOWN = 0,
    EVENT_READ = 0x01,
    EVENT_WRITE = 0x02,
    EVENT_ERROR = 0x04
} event_type_t;

typedef struct client_context_s {
    int fd;
    char buffer[BUFFER_SIZE];
    int buffer_len;
    int write_len;
    int total_read;
    int total_write;
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
} reactor_event_t;

typedef struct reactor_s {
    // int max_events;
    int epoll_fd;
    void* pthread;
    void* data;
    // int running;
} reactor_t;

// extern reactor_t[] g_reactor;

int reactor_create(int max_events, int thread_count = DEFAULT_THREADS);
int reactor_destroy();
int reactor_add_event(int fd, event_type_t events, 
                     event_callback_t callback, event_callback_t wcallback, void *arg);
int reactor_modify_event(int fd, event_type_t events);
int reactor_remove_event(int fd);
void reactor_run(void* data);
void reactor_stop();

#endif
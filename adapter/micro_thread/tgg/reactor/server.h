#ifndef SERVER_H
#define SERVER_H

#include "reactor.h"

#define SERVER_PORT 8080
#define BACKLOG 1024

typedef struct server_s {
    // reactor_t *reactor;
    int listen_fd;
    int port;
    int running;
} server_t;

server_t *server_create(int port);
void server_destroy(server_t *server);
int server_start(server_t *server);
void server_stop(server_t *server);
void accept_callback(int fd, event_type_t events, void *arg);
void client_read_callback(int fd, event_type_t events, void *arg);
void client_write_callback(int fd, event_type_t events, void *arg);

#endif
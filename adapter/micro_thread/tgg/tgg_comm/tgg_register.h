#pragma once
#include "co_routine.h"

typedef struct register_write_st {
    int fd;
    const char* ip;
    unsigned short port;
    const char* seckey;
    uint64_t ping_interval;
} register_routine_data;


int set_non_block(int iSock);

// 作为客户端的fd
int connect_tcp_socket(const unsigned short shPort, const char *pszIP);

void *register_read_routine( void *arg );
void *register_write_routine( void *arg );


void clean_queue_data();


#pragma once
#include "co_routine.h"

struct task_t
{
    stCoRoutine_t *co;
    int fd;
};

int set_non_block(int iSock);

void *accept_routine( void * );

// void *readwrite_routine( void *arg );
void *read_routine( void *arg );
void *write_routine( void *arg );
// void *real_write_routine( void *arg );

// 作为服务器的fd
int create_tcp_socket(const unsigned short shPort ,const char *pszIP ,bool bReuse);

void clean_queue_data();

void print_queue_counts();

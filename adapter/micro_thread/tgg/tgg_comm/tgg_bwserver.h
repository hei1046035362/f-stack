#pragma once

int set_non_block(int iSock);

void *accept_routine( void * );

void *readwrite_routine( void *arg );

int create_tcp_socket(const unsigned short shPort ,const char *pszIP ,bool bReuse);

void clean_queue_data()

// 透传处理线程
int init_bwtrans();
void uninit_bwtrans();

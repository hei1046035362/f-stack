/*
* Tencent is pleased to support the open source community by making Libco available.

* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License"); 
* you may not use this file except in compliance with the License. 
* You may obtain a copy of the License at
*
*   http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, 
* software distributed under the License is distributed on an "AS IS" BASIS, 
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
* See the License for the specific language governing permissions and 
* limitations under the License.
*/



#include "tgg_register.h"
#include "tgg_struct.h"
#include "tgg_common.h"
#include "tgg_conf.h"
#include "tgg_bw_cache.h"
#include "tgg_bwcomm.h"
#include "comm/common.hpp"
#include <rte_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <stack>
#include <map>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

#ifdef __FreeBSD__
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#endif

using namespace std;
#define MAX_PACKET_SIZE 12*1024  // bw发给gw的允许的数据包最大长度

extern int g_run;
int g_register_fd = -1;

int set_non_block(int iSock)
{
    int iFlags;

    iFlags = fcntl(iSock, F_GETFL, 0);
    iFlags |= O_NONBLOCK;
    iFlags |= O_NDELAY;
    int ret = fcntl(iSock, F_SETFL, iFlags);
    return ret;
}

static void SetAddr(const char *pszIP,const unsigned short shPort,struct sockaddr_in &addr)
{
    bzero(&addr,sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(shPort);
    int nIP = 0;
    if( !pszIP || '\0' == *pszIP   
        || 0 == strcmp(pszIP,"0") || 0 == strcmp(pszIP,"0.0.0.0") 
        || 0 == strcmp(pszIP,"*") 
      )
    {
        nIP = htonl(INADDR_ANY);
    }
    else
    {
        nIP = inet_addr(pszIP);
    }
    addr.sin_addr.s_addr = nIP;
}
int connect_tcp_socket(const unsigned short shPort, const char *pszIP)
{
    co_enable_hook_sys();

    int fd = socket(PF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    SetAddr(pszIP,shPort,addr);
    int error = 0;
    int ret = connect(fd,(struct sockaddr*)&addr,sizeof(addr));
                
    if ( errno == EALREADY || errno == EINPROGRESS )
    {       
        struct pollfd pf = { 0 };
        pf.fd = fd;
        pf.events = (POLLIN|POLLOUT|POLLERR|POLLHUP);
        co_poll( co_get_epoll_ct(),&pf,1,200);
        //check connect
        uint32_t socklen = sizeof(error);
        errno = 0;
        ret = getsockopt(fd, SOL_SOCKET, SO_ERROR,(void *)&error,  &socklen);
        if ( ret == -1 ) 
        {       
            printf("[%s][%d]getsockopt ERROR ret %d %d:%s\n", __FILE__, __LINE__, ret, errno, strerror(errno));
            close(fd);
            return -1;
        }       
        if ( error ) 
        {       
            errno = error;
            printf("[%s][%d]connect ERROR ret %d %d:%s\n", __FILE__, __LINE__, error, errno, strerror(errno));
            close(fd);
            return -1;
        }
    } else if(ret < 0) {
        printf("[%s][%d]connect ERROR ret %d %d:%s\n", __FILE__, __LINE__, ret, errno, strerror(errno));
        return -1;
    }
    return fd;
}

void *register_read_routine( void *arg )
{
    co_enable_hook_sys();
    register_routine_data* rdata = (register_routine_data*)arg;
    while(g_run) {
        struct pollfd pf = {0};
        pf.fd = rdata->fd;
        pf.events = POLLIN;
        co_poll(co_get_epoll_ct(), &pf, 1, 1000);

        char buf_read[ 4096 ];
        int ret = read( rdata->fd,buf_read,sizeof(buf_read) );
        if(ret > 0) {
            printf("[%s][%d] recieve data:%s, len[%d].\n", __FILE__, __LINE__, buf_read, ret);
        }
        if( ret > 0 || ( -1 == ret && EAGAIN == errno ) )
        {
            continue;
        }
        close( rdata->fd );
        RTE_LOG(ERR, USER1, "[%s][%d] bw[ip:%s,port%d] closed.\n",
         __FILE__, __LINE__, rdata->ip, rdata->port);
        break;
    }
    g_register_fd = -1;
    return 0;
}

void *register_write_routine( void *arg )
{
    co_enable_hook_sys();
    register_routine_data* wdata = (register_routine_data*)arg;
    // 拼接本机ip端口信息
    std::string con_str = wdata->ip;
    con_str += ":";
    con_str += std::to_string(wdata->port);
    // 拼接注册消息体
    std::string data = "{\"event\":\"gateway_connect\", \"address\":\"";
    data += con_str;
    data += "\", \"secret_key\":\"";
    data += wdata->seckey;
    data += "\"}";
    // 发送注册消息到注册中心
    write(wdata->fd, data.c_str(), data.length());
    // 心跳包
    std::string ping_data = "{\"event\":\"ping\"}";
    uint64_t last_update_time = get_system_ms();
    while(g_run) {
        struct pollfd pf = {0};
        pf.fd = wdata->fd;
        pf.events = POLLIN;
        co_poll(co_get_epoll_ct(), &pf, 1, 1000);

        uint64_t now = get_system_ms();
        if(now - last_update_time > wdata->ping_interval) {
            // printf("update heart beat for [PID:%d][prc_id:%d]\n", getpid(), g_prc_id);
            last_update_time = now;
            int ret = write(wdata->fd, ping_data.c_str(), ping_data.length());
            if(-1 == ret) {
                RTE_LOG(ERR, USER1, "[%s][%d] send to register failed, ip[%s] port[%d].",
                 __FILE__, __LINE__, wdata->ip, wdata->port);
                break;
            }
        } else {
            poll(NULL, 0, 100);// sleep 10ms
        }
    }
    if(g_register_fd > 0) {
        close(g_register_fd);
    }
    g_register_fd = -1;
    return 0;
}



// int main(int argc,char *argv[])
// {
//     if(argc<5){
//         printf("Usage:\n"
//                "example_echosvr [IP] [PORT] [TASK_COUNT] [PROCESS_COUNT]\n"
//                "example_echosvr [IP] [PORT] [TASK_COUNT] [PROCESS_COUNT] -d   # daemonize mode\n");
//         return -1;
//     }
//     const char *ip = argv[1];
//     int port = atoi( argv[2] );
//     int cnt = atoi( argv[3] );
//     int proccnt = atoi( argv[4] );
//     bool deamonize = argc >= 6 && strcmp(argv[5], "-d") == 0;

//     g_listen_fd = create_tcp_socket( port,ip,true );
//     listen( g_listen_fd,1024 );
//     if(g_listen_fd==-1){
//         printf("Port %d is in use\n", port);
//         return -1;
//     }
//     printf("listen %d %s:%d\n",g_listen_fd,ip,port);

//     set_non_block( g_listen_fd );

//     for(int k=0;k<proccnt;k++)
//     {

//         pid_t pid = fork();
//         if( pid > 0 )
//         {
//             continue;
//         }
//         else if( pid < 0 )
//         {
//             break;
//         }
//         for(int i=0;i<cnt;i++)
//         {
//             task_t * task = (task_t*)calloc( 1,sizeof(task_t) );
//             task->fd = -1;

//             co_create( &(task->co),NULL,readwrite_routine,task );
//             co_resume( task->co );

//         }
//         stCoRoutine_t *accept_co = NULL;
//         co_create( &accept_co,NULL,accept_routine,0 );
//         co_resume( accept_co );

//         co_eventloop( co_get_epoll_ct(),0,0 );

//         exit(0);
//     }
//     if(!deamonize) wait(NULL);
//     return 0;
// }


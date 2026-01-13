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
#include "comm/log.hpp"
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
int g_register_fd = -1;// fd
int g_reconnect = 0;// 是否要重连(子进程退出时会触发)

static int s_read_yield = 1, s_write_yield = 1;// 标识 读写 协程是否已挂起

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
            LOG_ERROR("getsockopt ERROR ret %d %d:%s", ret, errno, strerror(errno));
            close(fd);
            return -1;
        }       
        if ( error ) 
        {       
            errno = error;
            LOG_ERROR("connect ERROR ret %d %d:%s", error, errno, strerror(errno));
            close(fd);
            return -1;
        }
    } else if(ret < 0) {
        LOG_ERROR("connect ERROR ret %d %d:%s", ret, errno, strerror(errno));
        return -1;
    }
    return fd;
}

void *register_reconnect_routine( void *arg )
{
    co_enable_hook_sys();
    register_routine_data* cdata = (register_routine_data*)arg;
    while(g_run) {
        if(s_read_yield || s_write_yield) {
            if(g_register_fd > 0) {
                LOG_INFO("read[%d]/write[%d] routine yield, try to close connection.", s_read_yield, s_write_yield);
                shutdown(g_register_fd, SHUT_WR);
                close(g_register_fd);
                g_register_fd = -1;
            }
        }
        if(s_read_yield && s_write_yield) {
            g_register_fd = connect_tcp_socket(cdata->port, cdata->ip);
            if (g_register_fd > 0){
                set_non_block(g_register_fd);
                LOG_INFO("connected to register %s:%hu.", cdata->ip, cdata->port);
                s_read_yield = 0;
                s_write_yield = 0;
            } else {
                LOG_INFO("connect to register[%s:%hu] failed, check if register server is alive.", cdata->ip, cdata->port);
            }
        }
        int try_times = 500;
        struct pollfd pf = { .fd = g_register_fd, .events = 0 };
        while (try_times-- > 0 && g_run) { 
            co_poll(co_get_epoll_ct(), &pf, 1, 10);
            if(g_reconnect) {
                g_reconnect = 0;
                LOG_INFO("catch an reconnect command.");
                if(g_register_fd > 0) {
                    LOG_INFO("closing connection fd[%d].", g_register_fd);
                    shutdown(g_register_fd, SHUT_WR);
                    close(g_register_fd);
                    g_register_fd = -1;
                    co_poll(co_get_epoll_ct(), &pf, 1, 300);// 关闭后要等待读写协程进入yield状态，否则要等5s才能重连
                }
                break;
            }
        }
    }
    if(g_register_fd > 0) {
        shutdown(g_register_fd, SHUT_WR);
        close(g_register_fd);
        g_register_fd = -1;
    }
    LOG_INFO("connect routine exit.");
    return NULL;
}


void *register_read_routine( void *arg )
{
    co_enable_hook_sys();
    register_routine_data* rdata = (register_routine_data*)arg;
    LOG_INFO("read routine start.");
    while(g_run) {
        struct pollfd pf = {0};
        pf.fd = g_register_fd;
        pf.events = POLLIN;
        co_poll(co_get_epoll_ct(), &pf, 1, 100);
        if(s_read_yield) {
            continue;
        }
        if(g_register_fd < 0) {
            if(!s_read_yield) {
                LOG_INFO("socket is not avaliable any more, read routine yield.");
                s_read_yield = 1;
            }
            continue;
        }
        char buf_read[ 4096 ];
        int ret = read( g_register_fd,buf_read,sizeof(buf_read) );
        if(ret > 0) {
            LOG_DEBUG("recieve data:%s, len[%d].", buf_read, ret);
        }
        if( ret > 0 || ( -1 == ret && EAGAIN == errno ) )
        {
            struct pollfd pf = { .fd = g_register_fd, .events = POLLIN };
            co_poll(co_get_epoll_ct(), &pf, 1, 100);
            continue;
        }
        // close(g_register_fd);
        LOG_INFO("bw[ip:%s,port:%hu] closed, ret:%d errno:%d.", rdata->ip, rdata->port, ret, errno);
        s_read_yield = 1;
        // co_yield_ct();
    }
    LOG_INFO("read routine exit.");
    return 0;
}

int safe_write(int fd, const std::string& data)
{
    const char *data_ptr = data.c_str();
    size_t data_remaining = data.length();
    while (data_remaining > 0) {
        ssize_t ret = write(fd, data_ptr, data_remaining);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pf = { .fd = fd, .events = POLLOUT };
                co_poll(co_get_epoll_ct(), &pf, 1, 100);
                continue;
            }
            LOG_ERROR("send ping failed, error[%d]:%s.", errno, strerror(errno));
            return -1;
        }
        data_ptr += ret;
        data_remaining -= ret;
    }
    return 0;
}

void *register_write_routine( void *arg )
{
    co_enable_hook_sys();
    LOG_INFO("write routine start.");
    register_routine_data* wdata = (register_routine_data*)arg;
    uint64_t last_update_time = get_system_ms();
    // 拼接本机ip端口信息
    std::string con_str = wdata->bw_ip;
    con_str += ":";
    con_str += std::to_string(wdata->bw_port);
    // 拼接注册消息体
    std::string data = "{\"event\":\"gateway_connect\", \"address\":\"";
    data += con_str;
    data += "\", \"secret_key\":\"";
    data += wdata->seckey;
    data += "\", \"timestamp\": ";
    data += std::to_string(last_update_time);
    data += "}\n";
    // 心跳包
    std::string ping_data = "{\"event\":\"ping\"}";
    int send_request = 1;// 是否要向注册中心发送 注册信息(重连的时候要发送，区分发送心跳还是发送注册信息)
    int ret = 0;
    while(g_run) {
        uint64_t now = get_system_ms();
        if(g_register_fd <= 0) {
            if(!s_write_yield) {
                LOG_INFO("socket is not avaliable any more, write routine yield.");
                s_write_yield = 1;
                send_request = 1;
            }
            struct pollfd pf = { .fd = -1, .events = 0 };
            co_poll(co_get_epoll_ct(), &pf, 1, 100);
            continue;
        }

        if(( send_request || now - last_update_time > wdata->ping_interval) && !s_write_yield) {
            // printf("update heart beat for [PID:%d][prc_id:%d]\n", getpid(), g_prc_id);
            last_update_time = now;
            const std::string& send_data = send_request ? data : ping_data;
            ret = safe_write(g_register_fd, send_data);
            if(ret < 0) {
                LOG_ERROR("send to register failed, ip[%s] port[%hu], ret:%d, error[%d]:%s.", 
                    wdata->ip, wdata->port, ret, errno, strerror(errno));
                s_write_yield = 1;
                send_request = 1;
            } else  {
                if (send_request) send_request = 0;
            }
        } else {
            struct pollfd pf = { .fd = g_register_fd, .events = 0 };
            co_poll(co_get_epoll_ct(), &pf, 1, 100);
        }
    }
    LOG_INFO("write routine exit.");
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


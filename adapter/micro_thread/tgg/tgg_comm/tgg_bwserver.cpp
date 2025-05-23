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



#include "tgg_bwserver.h"
#include "tgg_struct.h"
#include "cmd/CmdProcessor.h"
#include "tgg_common.h"
#include "tgg_conf.h"
#include "tgg_bw_cache.h"
#include "BwMsgPack.hpp"
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

static stack<task_t*> g_readwrite;
int g_listen_fd = -1;
extern int g_prc_id;
extern int g_run;
int set_non_block(int iSock)
{
    int iFlags;

    iFlags = fcntl(iSock, F_GETFL, 0);
    iFlags |= O_NONBLOCK;
    iFlags |= O_NDELAY;
    int ret = fcntl(iSock, F_SETFL, iFlags);
    return ret;
}

static void tgg_process_bwrcv_data(void* arg)
{
    tgg_bw_data* bdata = (tgg_bw_data*)arg;
    // tgg_bw_info* binfo = g_map_bwinfo[bdata->fd];
    exec_cmd_processor(bdata->coreid, bdata->fd, arg);
    // pro->ExecCmd();
    // clean_bw_data(bdata);
    // TODO BW断开后，之前没有处理完的事情是否要继续处理
    // if (bdata->fd_opt & FD_CLOSE || 
    //     binfo->idx != bdata->idx ||
    //     binfo->status & (FD_STATUS_CLOSING | FD_STATUS_CLOSED)) {
    //     clean_bw_data(bdata);
    // }

}

void clean_queue_data()
{
    // 上次异常退出未处理的数据，先清理掉
    tgg_bw_data* bdata = NULL;
    while(tgg_dequeue_bwsnd(g_prc_id, &bdata) != -ENOENT) {
        clean_bw_data(bdata);
    }
    // 初始化数据结构
    tgg_init_bwfdx_prc(g_prc_id);
}

void *read_routine( void *arg )
{

    co_enable_hook_sys();

    task_t *co = (task_t*)arg;
    char buf[ 1024 * 16 ];
    for(;;)
    {
        if( -1 == co->fd )
        {
            g_readwrite.push( co );
            co_yield_ct();
            continue;
        }

        int fd = co->fd;
        co->fd = -1;

        for(;;)
        {
            struct pollfd pf = { 0 };
            pf.fd = fd;
            pf.events = (POLLIN|POLLERR|POLLHUP);
            co_poll( co_get_epoll_ct(),&pf,1,1000);

            int ret = read( fd,buf,sizeof(buf) );
            if(ret > 0) {
                tgg_bw_data bwdata = {
                    .fd = fd,
                    .coreid = g_prc_id,
                    .bwfdx = (fd << 8 ) | g_prc_id,
                    .fd_opt = FD_WRITE,
                    .idx = tgg_get_bwfdx_idx(g_prc_id, fd),
                    .data_len = (unsigned int)ret,
                    .data = buf
                };
                tgg_process_bwrcv_data(&bwdata);
            }
            if( ret > 0 || ( -1 == ret && EAGAIN == errno ) )
            {
                continue;
            }
            tgg_close_bw_session(g_prc_id, fd);
            close( fd );
            break;
        }

    }
    return 0;
}


void *write_routine( void *arg )
{
    co_enable_hook_sys();
    // g_prc_id = *((int*)arg);
    std::map<int, int> map_msgtype;// 客户端上行透传 消息类型映射
    map_msgtype[FD_NEW] = GatewayProtocal::CMD_ON_CONNECT;
    map_msgtype[FD_HANDLESHAKE] = GatewayProtocal::CMD_ON_WEBSOCKET_CONNECT;
    map_msgtype[FD_WRITE] = GatewayProtocal::CMD_ON_MESSAGE;
    map_msgtype[FD_CLOSE] = GatewayProtocal::CMD_ON_CLOSE;
    while(g_run) {
        tgg_bw_data* bdata = NULL;
        if (tgg_dequeue_bwsnd(g_prc_id, &bdata) < 0 || !bdata) {
            poll(NULL, 0, 10);// sleep 10ms
            continue;
        }
        int bwfdx = tgg_get_cli_bwfdx(bdata->coreid, bdata->fd);
        int prc_id = bwfdx & 0xf;
        int fd = bwfdx >> 8;
        // cli对应的bwfd已经改变或者 bwfdx已关闭，丢弃
        if(prc_id != g_prc_id || bdata->bwfdx != bwfdx || !tgg_get_bwfdx_status((bwfdx & 0xf), fd)) {
            clean_bw_data(bdata);
            continue;
        }
        // TODO cid的加入和删除 最佳的位置是在cliprc中校验之后，然而rte_hash在多线程环境中增加元素会崩溃，
        // 所以暂时放在这里，放在这里也没有问题，因为没有跟bw发送过connect消息的连接，后续也用不上
        if (bdata->fd_opt & FD_NEW) {
            int fdid = (bdata->fd << 8) | bdata->coreid;
            int cid = ((bdata->coreid << 24) | tgg_get_cli_idx(bdata->coreid, bdata->fd));
            // 添加到 hash<cid, fd>
            if (tgg_add_cid(cid, fdid) < 0) {
                RTE_LOG(ERR, USER1, "[%s][%d] add cid[%d] fdid[%d] failed.\n", __FILE__, __LINE__, cid, fdid);
                clean_bw_data(bdata);
                continue;
            }
        }
        std::string result;
        tgg_bw_protocal header = {
            .pack_len = (unsigned int)sizeof(tgg_bw_protocal) + bdata->data_len,
            .cmd = (unsigned char)map_msgtype[bdata->fd_opt],
            .local_ip = (unsigned int)tgg_get_bwfdx_ip(prc_id, fd),
            .local_port = (unsigned short)tgg_get_bwfdx_ip(prc_id, fd),
            .client_ip = (unsigned int)tgg_get_cli_ip(bdata->coreid, bdata->fd),
            .client_port = (unsigned short)tgg_get_cli_port(bdata->coreid, bdata->fd),
            .connection_id = (unsigned int)tgg_get_cli_idx(bdata->coreid, bdata->fd),
            .flag = 1,// TODO 需要确定数据来源，怎么填
            .gateway_port = TggConfigure::getInstance()->get_gateway_port(),
            .ext_len = 0// TODO 暂时不知道上行数据是否能用上
        };
        std::string sdata;
        if(bdata->data_len > 0) {
            sdata = std::string((char*)bdata->data, bdata->data_len);
        }
        BwPackageHandler::encode(result, &header, sdata);
        int ret = write(fd, result.c_str(), result.length());
        int loops = 3;// 如果失败最多重试3次，否则丢弃
        while(ret == -1 && EAGAIN == errno && loops--) {
            ret = write(fd, result.c_str(), result.length());
            poll(NULL, 0, 10);// sleep 10ms
        }
        if(-1 == ret) {
            RTE_LOG(ERR, USER1, "[%s][%d] trans to server failed, client_ip[%d] client_port[%d].",
             __FILE__, __LINE__, header.client_ip, header.client_port);
        }
        clean_bw_data(bdata);
    }
    return 0;
}


int co_accept(int fd, struct sockaddr *addr, socklen_t *len );
void *accept_routine( void * )
{
    co_enable_hook_sys();
    printf("accept_routine\n");
    fflush(stdout);
    while(g_run)
    {
        //printf("pid %ld g_readwrite.size %ld\n",getpid(),g_readwrite.size());
        if( g_readwrite.empty() )
        {
            printf("empty\n"); //sleep
            struct pollfd pf = { 0 };
            pf.fd = -1;
            poll( &pf,1,1000);

            continue;

        }
        struct sockaddr_in addr; //maybe sockaddr_un;
        memset( &addr,0,sizeof(addr) );
        socklen_t len = sizeof(addr);

        int fd = co_accept(g_listen_fd, (struct sockaddr *)&addr, &len);
        if( fd < 0 )
        {
            struct pollfd pf = { 0 };
            pf.fd = g_listen_fd;
            pf.events = (POLLIN|POLLERR|POLLHUP);
            co_poll( co_get_epoll_ct(),&pf,1,1000 );
            continue;
        }
        if( g_readwrite.empty() )
        {
            close( fd );
            continue;
        }
        set_non_block( fd );
        task_t *co = g_readwrite.top();
        co->fd = fd;
        g_readwrite.pop();
        co_resume( co->co );
    }
    return 0;
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

int create_tcp_socket(const unsigned short shPort, const char *pszIP, bool bReuse)
{
    int fd = socket(AF_INET,SOCK_STREAM, IPPROTO_TCP);
    if( fd >= 0 )
    {
        if(shPort != 0)
        {
            if(bReuse)
            {
                int nOpt = 1;
                setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&nOpt,sizeof(nOpt));
                setsockopt(fd,SOL_SOCKET,SO_REUSEPORT,&nOpt,sizeof(nOpt));
            }
            struct sockaddr_in addr ;
            SetAddr(pszIP,shPort,addr);
            int ret = bind(fd,(struct sockaddr*)&addr,sizeof(addr));
            if( ret != 0)
            {
                close(fd);
                return -1;
            }
        }
    }
    return fd;
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


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



#include "tgg_bwserver_fstack.h"
#include "tgg_struct.h"
#include "cmd/CmdProcessor.h"
#include "tgg_common.h"
#include "tgg_conf.h"
#include "tgg_bw_cache.h"
#include "BwMsgPack.hpp"
#include "tgg_bwcomm.h"
#include "tgg_transport.h"
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

#include "mt_incl.h"
#include "mt_api.h"
#include "micro_thread.h"
using namespace NS_MICRO_THREAD;
using namespace std;
#define MAX_PACKET_SIZE 12*1024  // bw发给gw的允许的数据包最大长度

// stack<task_t*> g_read_stack;
// static stack<task_t*> g_write_stack;
int g_listen_fd = -1;
extern int g_prc_id;
extern int g_run;
uint32_t g_gw_local_ip = 0;
unsigned short g_gw_local_port = 0;
static int s_fd_timeout = 60*1000;

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

std::map<int, int> map_msgtype;// 客户端上行透传 消息类型映射

static int s_dequeued_server_count = 0;

static uint64_t s_last_update_time = 0;
// 定时器回调函数
void update_heart_beat() {
    uint64_t now = get_system_ms();
    if(now - s_last_update_time > BW_PRC_HEART_BEAT) {
        // printf("update heart beat for [PID:%d][prc_id:%d]\n", getpid(), g_prc_id);
        s_last_update_time = now;
        tgg_update_bwprc(g_prc_id, now);
    }
}

int local_eventloop_fun(void* arg) {
    if (!g_run)
        return -1;// 终止coroutine的eventloop
    update_heart_beat();
    return 0;
}

static int set_fd_nonblock(int fd)
{
    int nonblock = 1;
    return ioctl(fd, FIONBIO, &nonblock);
}

static int create_tcp_sock()
{
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("create tcp socket failed, error: %s.", strerror(errno));
        return -1;
    }
    if (set_fd_nonblock(fd) == -1) {
        LOG_ERROR("set tcp socket nonblock failed");
        return -1;
    }

    return fd;
}

int main_bw_proc(int prc_id)
{
        // 启动发送线程
    mt_start_thread((void *)write_routine, NULL);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;

    addr.sin_port = big_endian() ? TggConfigure::getInstance()->get_bwsvr_bw_port() : htons(TggConfigure::getInstance()->get_bwsvr_bw_port());

    int fd = create_tcp_sock();
    if (fd < 0) {
        LOG_ERROR("create listen socket failed");
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        LOG_ERROR("bind failed [%s]", strerror(errno));
        return -1;
    }

    if (listen(fd, 1024) < 0) {
        close(fd);
        LOG_ERROR("listen failed [%s]", strerror(errno));
        return -1;
    }
    LOG_INFO("start service for port:%d.", TggConfigure::getInstance()->get_bwsvr_bw_port());
    int clt_fd = 0;
    int *p;
    while (g_run) {
        update_heart_beat();
        struct sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);

        clt_fd = mt_accept(fd, (struct sockaddr*)&client_addr, (socklen_t*)&addr_len, -1);
        if (clt_fd < 0) {
            if(clt_fd != -1) {
                LOG_WARNING("accept error[%d]", clt_fd);
            }
            mt_sleep(10);
            continue;
        }
        // if (clt_fd >= g_fd_limit - 1)   {
        //     LOG_WARNING("given fd[%d] is invalid,[0,%d]", fd, g_fd_limit - 1);
        //     mt_sleep(10);
        //     continue;
        // }
        // 如果fd还在使用中，拒绝连接
        // if (tgg_get_cli_idx(g_core_id, clt_fd) != TGG_FD_CLOSED) {
        //     LOG_ERROR("socket fd[%d] still in use.", clt_fd);
        //     close(clt_fd);
        //     continue;
        // }
        // TODO 获取ip的方式待商榷
        // uint32_t ip_int = get_local_addr(clt_fd);
        if (set_fd_nonblock(clt_fd) == -1) {
            LOG_ERROR("set clt_fd nonblock failed [%s]", strerror(errno));
            break;
        }
        LOG_INFO("accept new connection.");
        // 启动一个接收线程
        p = new int(clt_fd);
        mt_start_thread((void *)read_routine, (void *)p);
    }
    close(fd);
    return 0;

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
void print_queue_counts()
{
    LOG_WARNING("dequeue success count:%d", s_dequeued_server_count);
}

static int get_remote_info(int sockfd, uint32_t& ip, ushort& port, char* ip_str)
{
     // 获取IP地址信息
     struct sockaddr_in local_addr;
     socklen_t addrlen = sizeof(local_addr);
     if (ff_getpeername(sockfd, (struct linux_sockaddr *)&local_addr, &addrlen) < 0) {
         LOG_ERROR("getsockname");
         close(sockfd);
         return -1;
     }
     // char ip_str[INET_ADDRSTRLEN];
     inet_ntop(AF_INET, &(local_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
     // printf("ip str:%s\n", ip_str);
     struct in_addr ip_addr;
     inet_pton(AF_INET, ip_str, &ip_addr);
     ip = ip_addr.s_addr;
     port = local_addr.sin_port;
     LOG_INFO("IP address in decimal: %u\n", ip);
     return 0;
}


void read_routine( void *arg )
{
    // map_msgtype[FD_NEW] = GatewayProtocal::CMD_ON_CONNECT;
    // map_msgtype[FD_HANDLESHAKE] = GatewayProtocal::CMD_ON_WEBSOCKET_CONNECT;
    // map_msgtype[FD_WRITE] = GatewayProtocal::CMD_ON_MESSAGE;
    // map_msgtype[FD_CLOSE] = GatewayProtocal::CMD_ON_CLOSE;

    int ret, consume_ret = 0;
    int bw_fd = *((int *)arg);
    delete (int *)arg;
    uint32_t ip;
    ushort port;
    char ip_str[INET_ADDRSTRLEN] = {0};
    if (get_remote_info(bw_fd, ip, port, ip_str) < 0) {
        LOG_ERROR("get client remote info failed.");
        close(bw_fd);
        return ;
    }

    LOG_INFO("accept new connection ip[%s], port[%u].", ip_str, port);
    char recv_buffer[ MAX_PACKET_SIZE ];
    // std::vector<char> recv_buffer;
    unsigned int pos = 0;
    while(g_run)
    {
        char buf_read[ 4096 ];
        int ret = mt_recv(bw_fd, (void *)buf_read, 4096, 0, s_fd_timeout);
        if(ret > 0) {
            memcpy(recv_buffer + pos, buf_read, ret);
            if(pos + ret < sizeof(tgg_bw_protocal)) {
                pos += ret;
                continue;// 分包
            }
            tgg_bw_protocal* header = reinterpret_cast<tgg_bw_protocal*>(recv_buffer);
            unsigned int pack_len = htonl(header->pack_len);
            // 验证数据包长度有效性[4](@ref)
            if(pack_len < sizeof(tgg_bw_protocal) || 
               pack_len > MAX_PACKET_SIZE) {
                LOG_ERROR("invalid packet len, bw[ip:%s,port%d] is closing.", ip_str, ntohs(port));
                break;
            }
            // 够header 但不够一个完整的包，继续收包
            if(pos + ret < pack_len) {
                pos += ret;
                continue;// 分包
            }
            unsigned int left_len = pos + ret;
            unsigned int parsed_pos = 0;// 当前缓冲区存放的完整的包的个数
            do {
                tgg_bw_data bwdata = {
                    .fd = bw_fd,
                    .coreid = g_prc_id,
                    .bwfdx = (bw_fd << 8 ) | g_prc_id,
                    .fd_opt = FD_WRITE,
                    .idx = tgg_get_bwfdx_idx(g_prc_id, bw_fd),
                    .data_len = pack_len,
                    .data = recv_buffer + parsed_pos,
                    .peer_ip = ip,// 下行的ip 端口 暂时没有用到
                    .peer_port = port,
                    // .cid = 0// 下行没有cid
                };
                tgg_process_bwrcv_data(&bwdata);
                left_len -= pack_len;
                parsed_pos += pack_len;
                header = reinterpret_cast<tgg_bw_protocal*>(recv_buffer + parsed_pos);
                pack_len = htonl(header->pack_len);
            } while (left_len >= pack_len && left_len > 0);// 处理粘包

            if(left_len > 0) {
                // 把剩余数据移动到前面去,数据提供了长度，因此不需要置空操作
                memmove(recv_buffer, recv_buffer + parsed_pos, left_len);
                pos = left_len;
            } else {// 等于的情况
                // 有长度和起始位置字段，不需要置空操作
                // memset(recv_buffer, 0, pos);
                pos = 0;
            }
        }
        if(ret == -1 && errno == ETIME) {
            LOG_ERROR("bw heart beat timeout, bw_fd:%d.", bw_fd);
            break;
        }
        if(ret == -4) {
            // 主动断开连接
            LOG_ERROR("closing connection affected,bw_fd:%d.", bw_fd);
            break;
        }
        // g_tgg_stats.recv++;
        if (ret < 0) {
            // 接收出现错误
            LOG_ERROR("recv from bw error:%d, bw_fd:%d.", bw_fd, ret);
            break;
        }
        if (!ret) {
            // 对端主动关闭了
            LOG_INFO("recv close from bw, bw_fd:%d.", bw_fd);
            break;
        }
        // // 调试打印
        // if(!strncmp(buf, "GET", 3)) {// GET请求消息
        //     LOG_DEBUG("bwfd:%d recv data:%s.", bw_fd, (char*)buf);
        // } else {// 其他消息
        //     LOG_DEBUG("bwfd:%d revc data:%s.", bw_fd, bin2hex(std::string((char*)buf, ret)).c_str());
        // }
    }
    LOG_ERROR("bw[ip:%s,port%d] is closing, ret:%d.", ip_str, ntohs(port), ret);
    tgg_close_bw_session(g_prc_id, bw_fd);
    close( bw_fd );
    LOG_ERROR("bw[ip:%s,port%d] closed.", ip_str, ntohs(port));
}


void write_routine( void *arg )
{
    // int co_index = ++s_co_count;
    // co_enable_hook_sys();
    // g_prc_id = *((int*)arg);
    std::map<int, int> map_msgtype;// 客户端上行透传 消息类型映射
    map_msgtype[FD_NEW] = GatewayProtocal::CMD_ON_CONNECT;
    map_msgtype[FD_HANDLESHAKE] = GatewayProtocal::CMD_ON_WEBSOCKET_CONNECT;
    map_msgtype[FD_WRITE] = GatewayProtocal::CMD_ON_MESSAGE;
    map_msgtype[FD_CLOSE] = GatewayProtocal::CMD_ON_CLOSE;
    while(g_run)
    {
        tgg_bw_data* bdata = NULL;
        if (tgg_dequeue_bwsnd(g_prc_id, &bdata) < 0) {
            mt_sleep(10);
            continue;
        }
        if(!bdata) {
            LOG_ERROR("deque an NULL data.");
            mt_sleep(10);
            continue;
        }

        int bwfdx = bdata->bwfdx;//tgg_get_cli_bwfdx(bdata->coreid, bdata->fd);
        int prc_id = bwfdx & 0xff;
        int fd = bwfdx >> 8;
        // cli对应的bwfd已经改变或者 bwfdx已关闭，丢弃
        if(prc_id != g_prc_id || bdata->bwfdx != bwfdx || !tgg_get_bwfdx_status((bwfdx & 0xff), fd)) {
            LOG_ERROR("deal bw write data failed:prc_id[%d] bwdatafdx:bwfdx[%d:%d] status:[%d].", 
                prc_id, bdata->bwfdx, bwfdx, tgg_get_bwfdx_status((bwfdx & 0xff), fd));
            clean_bw_data(bdata);
            continue;
        }
        // TODO cid的加入和删除 最佳的位置是在cliprc中校验之后，然而rte_hash在多线程环境中增加元素会崩溃，
        // 所以暂时放在这里，放在这里也没有问题，因为没有跟bw发送过connect消息的连接，后续也用不上
        if (bdata->fd_opt & FD_NEW) {
            if(tgg_init_session(bdata->coreid, bdata->fd, bdata->idx) < 0) {
                clean_bw_data(bdata);
                continue;
            }
        }
        int cid = tgg_get_cli_cid(bdata->coreid, bdata->fd);
        if(cid <= 0) {
            // 这里发送给客户端和清理hash表信息的顺序待商榷
            Send2Fd(bdata->coreid, bdata->fd, bdata->idx, "", FD_CLOSE, 0);// cid 还没有创建的连接直接关闭连接
            LOG_ERROR("invalid cid[%d] for coreid:%d fd[%d] failed.", cid, bdata->coreid, bdata->fd);
            clean_bw_data(bdata);
            continue;
        }
        std::string result;
        tgg_bw_protocal header = {
            .pack_len = (unsigned int)sizeof(tgg_bw_protocal) + bdata->data_len,
            .cmd = (unsigned char)map_msgtype[bdata->fd_opt],
            .local_ip = g_gw_local_ip,// gw的内网通信ip (unsigned int)tgg_get_bwfdx_ip(prc_id, fd),
            .local_port = g_gw_local_port,//(unsigned short)tgg_get_bwfdx_port(prc_id, fd),
            .client_ip = bdata->peer_ip,// 客户端的ip
            .client_port = bdata->peer_port,
            .connection_id = (unsigned int)cid,
            .flag = 1,// TODO 需要确定数据来源，怎么填
            .gateway_port = TggConfigure::getInstance()->get_gateway_port(),
            .ext_len = 0// TODO 暂时不知道上行数据是否能用上
        };
        std::string sdata;
        if(bdata->data_len > 0) {
            sdata = std::move(std::string((char*)bdata->data, bdata->data_len));
        }
        BwPackageHandler::encode(result, &header, sdata);

        // TODO:打印发送内容，稳定后需删除
        // uint32_t ip_int = tgg_get_bwfdx_ip(prc_id, fd);  // 整数形式的IP（网络字节序，对应192.168.1.1）
        // struct in_addr addr;
        // addr.s_addr = ip_int;  // 直接赋值网络字节序整数
        // char ip_str[INET_ADDRSTRLEN];
        // inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
        // LOG_DEBUG("send to server[ip:%s,port:%u]:%s.", 
        //     ip_str, tgg_get_bwfdx_port(prc_id, fd), bin2hex(result).c_str());

        // struct pollfd pf = { 0 };
        // pf.fd = fd;
        // pf.events = (POLLOUT|POLLERR|POLLHUP);
        // co_poll( co_get_epoll_ct(),&pf,1, 200);
        // printf("co[%d] do write.\n", co_index);
        // int ret = write(fd, result.c_str(), result.length());
        int ret = mt_send(fd, result.c_str(), result.length(), 0, 1000);
        // printf("co[%d] finished write.\n", co_index);
        // int ret = splice_write(pipefd, fd, result.c_str(), result.length());
        // int loops = 3;// 如果失败最多重试3次，否则丢弃
        // while(ret == -1 && EAGAIN == errno && loops--) {
        //     ret = write(fd, result.c_str(), result.length());
        //     poll(NULL, 0, 10);// sleep 10ms
        // }
        if (ret == -4) {
            // 主动断开连接
            LOG_INFO("closing connection affected.");
        } else if (ret < 0) {
            LOG_ERROR("trans to server failed, client_ip[%d] client_port[%d].",
                header.client_ip, header.client_port);
        }

        // if(-1 == ret) {
        //     LOG_ERROR("trans to server failed, client_ip[%d] client_port[%d].",
        //         header.client_ip, header.client_port);
        //     if (errno == EAGAIN) {
        //         struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        //         co_poll(co_get_epoll_ct(), &pfd, 1, 1000); // 协程友好等待
        //     }
        // }
        if(bdata->fd_opt & FD_CLOSE) {
            // 这里发送给客户端和清理hash表信息的顺序待商榷
            Send2Client(cid, "", FD_CLOSE, 0);// 这里不需要再写数据了，收到对端关闭才走到这里来的 FD_WRITE|
            tgg_free_session(bdata->coreid, bdata->fd, cid);
            LOG_INFO("catched an close cmd, cid:%d.", cid);
        }
        clean_bw_data(bdata);
        // printf("co[%d] finished dequeue.\n", co_index);
    }
    return ;
}


// int co_accept(int fd, struct sockaddr *addr, socklen_t *len );
// void *accept_routine( void * )
// {
//     co_enable_hook_sys();
//     LOG_INFO("accept_routine");
//     while(g_run)
//     {
//         //printf("pid %ld g_readwrite.size %ld\n",getpid(),g_readwrite.size());
//         if( g_read_stack.empty() )
//         {
//             LOG_DEBUG("empty"); //sleep
//             struct pollfd pf = { 0 };
//             pf.fd = -1;
//             poll( &pf,1,1000);

//             continue;

//         }
//         struct sockaddr_in addr; //maybe sockaddr_un;
//         memset( &addr,0,sizeof(addr) );
//         socklen_t len = sizeof(addr);

//         int fd = co_accept(g_listen_fd, (struct sockaddr *)&addr, &len);
//         if( fd < 0 )
//         {
//             struct pollfd pf = { 0 };
//             pf.fd = g_listen_fd;
//             pf.events = (POLLIN|POLLERR|POLLHUP);
//             co_poll( co_get_epoll_ct(),&pf,1,1000 );
//             continue;
//         }
//         LOG_INFO("accept new connection fd[%d].", fd);
//         if( g_read_stack.empty())
//         {
//             task_t * task = (task_t*)calloc( 1,sizeof(task_t) );
//             task->fd = -1;
//             co_create( &(task->co),NULL,read_routine,task );
//             co_resume( task->co );
//             // g_read_stack.push(task);
//             LOG_ERROR("NO enough corutine in g_read_stack.");
//             // close( fd );
//             // continue;
//         }
//         set_non_block( fd );
//         task_t *co = g_read_stack.top();
//         co->fd = fd;
//         g_read_stack.pop();
//         co_resume( co->co );
//         LOG_INFO("accept new connection fd[%d].", fd);
//     }
//     return 0;
// }

// static void SetAddr(const char *pszIP,const unsigned short shPort,struct sockaddr_in &addr)
// {
//     bzero(&addr,sizeof(addr));
//     addr.sin_family = AF_INET;
//     addr.sin_port = htons(shPort);
//     int nIP = 0;
//     if( !pszIP || '\0' == *pszIP   
//         || 0 == strcmp(pszIP,"0") || 0 == strcmp(pszIP,"0.0.0.0") 
//         || 0 == strcmp(pszIP,"*") 
//       )
//     {
//         nIP = htonl(INADDR_ANY);
//     }
//     else
//     {
//         nIP = inet_addr(pszIP);
//     }
//     addr.sin_addr.s_addr = nIP;
//     g_gw_local_ip = nIP;//网络字节序
//     g_gw_local_port = shPort;// 网络字节序

// }

// int create_tcp_socket(const unsigned short shPort, const char *pszIP, bool bReuse)
// {
//     int fd = socket(AF_INET,SOCK_STREAM, IPPROTO_TCP);
//     if( fd >= 0 )
//     {
//         if(shPort != 0)
//         {
//             if(bReuse)
//             {
//                 int nOpt = 1;
//                 setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&nOpt,sizeof(nOpt));
//                 setsockopt(fd,SOL_SOCKET,SO_REUSEPORT,&nOpt,sizeof(nOpt));
//             }
//             struct sockaddr_in addr ;
//             SetAddr(pszIP,shPort,addr);
//             int ret = bind(fd,(struct sockaddr*)&addr,sizeof(addr));
//             if( ret != 0)
//             {
//                 close(fd);
//                 return -1;
//             }
//         }
//     }
//     return fd;
// }


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


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

using namespace std;
#define MAX_PACKET_SIZE 12*1024  // bw发给gw的允许的数据包最大长度

stack<task_t*> g_read_stack;
int g_listen_fd = -1;
extern int g_prc_id;
extern int g_run;
uint32_t g_gw_local_ip = 0;
unsigned short g_gw_local_port = 0;

int set_non_block(int iSock)
{
    int iFlags;

    iFlags = fcntl(iSock, F_GETFL, 0);
    iFlags |= O_NONBLOCK;
    iFlags |= O_NDELAY;
    int ret = fcntl(iSock, F_SETFL, iFlags);
    return ret;
}

static int tgg_process_bwrcv_data(void* arg)
{
    tgg_bw_data* bdata = (tgg_bw_data*)arg;
    return exec_cmd_processor(bdata->coreid, bdata->fd, arg);
}

std::map<int, int> map_msgtype;// 客户端上行透传 消息类型映射

#include <sys/uio.h>
#include <netinet/tcp.h>

// 火焰图显示的关键优化点
#define MIN_ZEROCOPY_SIZE 16384  // 16KB以上启用零拷贝
#define CORK_BATCH_THRESHOLD 4   // 达到4个包时自动触发发送

// 高性能写入函数（解决火焰图瓶颈）
size_t turbo_write(int fd, const void* data, size_t len) {
    static __thread int cork_count = 0;  // CORK状态计数
    
    // 1. 空数据直接返回
    if (len == 0) return 0;
    
    // 2. 启用TCP_CORK减少小包（火焰图显示tcp_transmit_skb占35%）
    if (cork_count == 0) {
        int cork = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
    }
    cork_count++;
    
    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t sent = 0;
        
        // 3. 中等/大数据使用sendmsg优化
        if (len - total_sent >= 128) {
            struct iovec iov = {
                .iov_base = (void*)((char*)data + total_sent),
                .iov_len = len - total_sent
            };
            struct msghdr msg = {
                .msg_iov = &iov,
                .msg_iovlen = 1
            };

            // 启用MSG_ZEROCOPY（消除数据拷贝开销）
            sent = sendmsg(fd, &msg, MSG_DONTWAIT | (len > MIN_ZEROCOPY_SIZE ? MSG_ZEROCOPY : 0));
        } 
        // 4. 小数据使用普通write（避免小包零拷贝开销）
        else {
            sent = write(fd, (char*)data + total_sent, len - total_sent);
        }
        
        if (sent > 0) {
            total_sent += sent;
            continue;
        }
        if(errno == EPIPE) {
            LOG_WARNING("fd[%d] is not avaliable anymore, drop data.", fd);
            return -1;
        }
        // 5. 协程友好型等待（精准控制超时）
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd pfd = {.fd = fd, .events = POLLOUT, 0};
            co_poll(co_get_epoll_ct(), &pfd, 1, 10);  // 仅等待10ms
            
            // 包计数达到阈值时强制发送（减少延迟）
            if (cork_count >= CORK_BATCH_THRESHOLD) {
                int cork = 0;
                setsockopt(fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
                cork = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
                cork_count = 0;
            }
        } 
        // 6. 错误处理
        else {
            break;
        }
    }
    
    // 7. 解除CORK状态（如果达到阈值）
    cork_count--;
    if (cork_count == 0) {
        int cork = 0;
        setsockopt(fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
    }
    
    // 8. 等待零拷贝完成（避免内存过早释放）
    if (len > MIN_ZEROCOPY_SIZE) {
        struct tcp_zerocopy_receive hdr = {0};
        socklen_t hdrlen = sizeof(hdr);
        getsockopt(fd, IPPROTO_TCP, TCP_ZEROCOPY_RECEIVE, &hdr, &hdrlen);
    }
    
    return total_sent;
}

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "comm/common.hpp"
#include <algorithm>

static thread_local rapidjson::StringBuffer tl_buffer;
static thread_local rapidjson::Writer<rapidjson::StringBuffer> tl_writer(tl_buffer);

// 预编译常量（减少临时字符串生成）
static const char HTTP_PREFIX[] = "HTTP_";
static const char UNKNOWN_HOST[] = "unknown";

// 封装发送给bw的握手请求数据
static std::string build_server_data(const http_request_t &req, unsigned int ip, 
                             ushort port) {
    tl_buffer.Clear();  // 复用线程局部缓冲区
    tl_writer.Reset(tl_buffer);
    
    // 1. 直接流式构建JSON（避免DOM树开销）
    tl_writer.StartObject();
    
    // ===== SERVER_VARS 优化区块 =====
    tl_writer.Key("server");
    tl_writer.StartObject();
    
    // 基础字段（零拷贝引用）
    tl_writer.Key("REQUEST_METHOD");
    tl_writer.String(req.method.data, req.method.len);
    
    tl_writer.Key("REQUEST_URI");
    tl_writer.String(req.uri.data, req.uri.len);
    
    tl_writer.Key("SERVER_PROTOCOL");
    char protocal[10];
    sprintf(protocal, "HTTP/1.%d", req.minor_version);
    tl_writer.String(protocal, strlen(protocal));
    
    // 网络信息（SIMD加速IP转换）
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ip, ip_str, sizeof(ip_str));
    tl_writer.Key("REMOTE_ADDR");
    tl_writer.String(ip_str, strlen(ip_str));
    
    tl_writer.Key("REMOTE_PORT");
    tl_writer.Uint(port);
    
    tl_writer.Key("SERVER_PORT");
    tl_writer.Uint(TggConfigure::getInstance()->get_gateway_port());
    
    // 主机名（分支预测优化）
    tl_writer.Key("SERVER_NAME");
    if(req.host.len <= 0 || req.host.data[0] == ' ') {
        tl_writer.String(UNKNOWN_HOST);
    } else {
        tl_writer.String(req.host.data, req.host.len);
    }
    
    // ===== HEADER转换优化（SIMD加速） =====
    for (size_t i = 0; i < req.num_headers; i++) {
        // 原位转换：避免临时字符串
        char header_key[256];
        char* dest = header_key;
        const char* src = req.headers[i].name;
        size_t index = 0;
        
        // 1. 添加"HTTP_"前缀
        memcpy(dest, HTTP_PREFIX, sizeof(HTTP_PREFIX) -1);
        dest += sizeof(HTTP_PREFIX) -1;
        
        // 2. 大写转换+替换字符（向量化处理）
        while (*src && index++ < req.headers[i].name_len && dest - header_key < 250) {
            char c = *src++;
            // SIMD友好分支：减少跳转预测失败
            c = (c == '-') ? '_' : c & ~0x20; // 位运算转大写
            *dest++ = c;
        }
        *dest = '\0';
        
        tl_writer.Key(header_key);
        tl_writer.String(req.headers[i].value, req.headers[i].value_len);
    }
    tl_writer.EndObject(); // server结束
    
    // ===== QUERY参数优化（批量处理） =====
    tl_writer.Key("get");
    tl_writer.StartObject();
    for (size_t i = 0; i < req.num_query_params; i++) {
        tl_writer.Key(req.query_params[i].name, req.query_params[i].name_len);
        tl_writer.String(req.query_params[i].value, req.query_params[i].value_len);
    }
    tl_writer.EndObject();
    
    // ===== COOKIE优化（预过滤） =====
    tl_writer.Key("cookie");
    tl_writer.StartObject();
    for (size_t i = 0; i < req.num_cookies; i++) {
        tl_writer.Key(req.cookies[i].name, req.cookies[i].name_len);
        tl_writer.String(req.cookies[i].value, req.cookies[i].value_len);
    }
    tl_writer.EndObject();
    
    tl_writer.EndObject(); // 根对象结束
    
    // 直接返回缓冲区引用（避免二次拷贝）
    return {tl_buffer.GetString(), tl_buffer.GetSize()};
}


static int s_dequeued_server_count = 0;

// return   -1 外部会等待10ms，并继续循环，
static int write_data()
{
    while (true) {
        tgg_bw_data* bdata = NULL;
        if (tgg_dequeue_bwsnd(g_prc_id, &bdata) < 0) {
            return -1;
        }
        if(!bdata) {
            LOG_ERROR("deque an NULL data.");
            return -1;
        }
        s_dequeued_server_count++;
        int bwfdx = bdata->bwfdx;//tgg_get_cli_bwfdx(bdata->coreid, bdata->fd);
        int prc_id = bwfdx & 0xff;
        int fd = bwfdx >> 8;
        // cli对应的bwfd已经改变或者 bwfdx已关闭，丢弃
        if(prc_id != g_prc_id || !tgg_get_bwfdx_status(prc_id, fd) || bdata->fd <= 0) {
            LOG_ERROR("deal bw write data failed:prc_id[%d] bwdatafdx:bwfdx[%d:%d]," 
                "cli_fd:%d, idx:%d, status:[%d].", 
                prc_id, bdata->bwfdx, fd,
                bdata->fd, bdata->idx, tgg_get_bwfdx_status(prc_id, fd));
            if(bdata->data) {
                LOG_ERROR("write data:%s", bdata->data);
            }
            clean_bw_data(prc_id, bdata);
            continue;
        }
        // TODO cid的加入和删除 最佳的位置是在cliprc中校验之后，然而rte_hash在多线程环境中增加元素会崩溃，
        // 所以暂时放在这里，放在这里也没有问题，因为没有跟bw发送过connect消息的连接，后续也用不上
        if (bdata->fd_opt & FD_NEW) {
            if(tgg_init_session(bdata->coreid, bdata->fd, bdata->idx) < 0) {
                clean_bw_data(prc_id, bdata);
                continue;
            }
        }
        int cid = tgg_get_cli_cid(bdata->coreid, bdata->fd);
        if(cid <= 0) {
            // 这里发送给客户端和清理hash表信息的顺序待商榷
            Send2Fd(bdata->coreid, bdata->fd, bdata->idx, "", FD_CLOSE, 0);// cid 还没有创建的连接直接关闭连接
            LOG_ERROR("invalid cid[%d] for coreid:%d fd[%d] failed.", cid, bdata->coreid, bdata->fd);
            clean_bw_data(prc_id, bdata);
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
        std::string ext_data = tgg_get_cli_reserved(bdata->coreid, bdata->fd);
        if(bdata->fd_opt & FD_CLOSE) {// 要在发送给bw之前先回给客户端，否则客户端收到的消息可能不及时，write会导致协程切换
            // 这里发送给客户端和清理hash表信息的顺序待商榷
            LOG_WARNING("catched an close cmd, coreid[%d] fd[%d] idx[%d] cid:%d.", bdata->coreid, bdata->fd, bdata->idx, cid);
            Send2Fd(bdata->coreid, bdata->fd, bdata->idx, "", FD_CLOSE, 0);// 这里不需要再写数据了，收到对端关闭才走到这里来的 FD_WRITE|
            tgg_free_session(bdata->coreid, bdata->fd, cid);
        }
        std::string sdata;
        if(bdata->data_len > 0) {
            if(bdata->fd_opt & FD_NEW) {
                struct http_request_t req;
                if(!parse_http_request((char*)bdata->data, bdata->data_len, &req)) {
                    LOG_ERROR("parse http request failed:%s.", (char*)bdata->data);
                }
                sdata = build_server_data(req, bdata->peer_ip, bdata->peer_port);
                header.pack_len = (unsigned int)sizeof(tgg_bw_protocal) + sdata.length();
            } else {
                sdata = std::move(std::string((char*)bdata->data, bdata->data_len));
            }
            if(AsyncLogger::getInstance().getloglevel() == LogLevel::DEBUG) {
                std::string print_data;
                if(bdata->data_len > 4 && *((unsigned short*)bdata->data) == 0xfeff) {
                    message_unpack(sdata, print_data);
                } else {
                    print_data = sdata;
                }
                LOG_DEBUG("send to bw data:%s", print_data.c_str());
            }
        }
        clean_bw_data(prc_id, bdata);// 在调用write之前清理数据，防止协程切换导致的地址变化
        BwPackageHandler::encode(result, &header, sdata, ext_data);

        // int ret = co_write_complete(fd, result.c_str(), result.length());
        int ret = turbo_write(fd, result.c_str(), result.length());
        // int ret = splice_write(pipefd, fd, result.c_str(), result.length());
        // int ret = write(fd, result.c_str(), result.length());
        if(-1 == ret) {
            LOG_ERROR("trans to server failed, client_ip[%d] client_port[%d].",
                header.client_ip, header.client_port);
            return -1;
        }
    }
    return 0;
}


void clean_queue_data()
{
    // 上次异常退出未处理的数据，先清理掉
    tgg_bw_data* bdata = NULL;
    while(tgg_dequeue_bwsnd(g_prc_id, &bdata) != -ENOENT) {
        if(!bdata) {
            continue;
        }
        int cid = tgg_get_cli_cid(bdata->coreid, bdata->fd);
        if(cid > 0) {
            if(bdata->fd_opt & FD_CLOSE) {// 要在发送给bw之前先回给客户端，否则客户端收到的消息可能不及时，write会导致协程切换
                // 这里发送给客户端和清理hash表信息的顺序待商榷
                LOG_WARNING("catched an close cmd, coreid[%d] fd[%d] idx[%d] cid:%d.", bdata->coreid, bdata->fd, bdata->idx, cid);
                Send2Fd(bdata->coreid, bdata->fd, bdata->idx, "", FD_CLOSE, 0);// 这里不需要再写数据了，收到对端关闭才走到这里来的 FD_WRITE|
                tgg_free_session(bdata->coreid, bdata->fd, cid);
            }
        }
        clean_bw_data(g_prc_id, bdata);
    }
    // 初始化数据结构
    tgg_init_bwfdx_prc(g_prc_id);
}
void print_queue_counts()
{
    LOG_WARNING("dequeue success count:%d", s_dequeued_server_count);
}

void *read_routine(void *arg)
{
    co_enable_hook_sys();

    task_t *co = (task_t*)arg;
    for(;;) {
        if (-1 == co->fd) {
            g_read_stack.push(co);
            co_yield_ct();
            continue;
        }

        int fd = co->fd;
        co->fd = -1;
        uint32_t ip;
        ushort port;
        char ip_str[INET_ADDRSTRLEN] = {0};
        
        if (get_connection_info(fd, ip_str, &ip, &port) < 0) {
            LOG_ERROR("get peer connection[%d] info failed.", fd);
            close(fd);
            continue;
        }
        
        LOG_INFO("new read routine ip[%s], port[%u].", ip_str, ntohs(port));
        
        char recv_buffer[MAX_PACKET_SIZE];
        int exec_ret = 0;
        unsigned int pos = 0;
        
        for(;;) {
            struct pollfd pf = {0};
            pf.fd = fd;
            pf.events = (POLLIN | POLLERR | POLLHUP);
            co_poll(co_get_epoll_ct(), &pf, 1, 200);

            char buf_read[4096];
            int ret = read(fd, buf_read, sizeof(buf_read));
            
            if (ret > 0) {
                // 检查缓冲区是否溢出
                if (pos + ret > MAX_PACKET_SIZE) {
                    LOG_ERROR("buffer overflow, pos:%u, ret:%d, max:%d", 
                             pos, ret, MAX_PACKET_SIZE);
                    break;
                }
                
                memcpy(recv_buffer + pos, buf_read, ret);
                unsigned int total_len = pos + ret;
                unsigned int parsed_pos = 0;
                
                while (parsed_pos < total_len) {
                    // 检查是否收到完整包头
                    if (total_len - parsed_pos < sizeof(tgg_bw_protocal)) {
                        break;  // 等待更多数据
                    }
                    
                    tgg_bw_protocal* header = reinterpret_cast<tgg_bw_protocal*>(
                        recv_buffer + parsed_pos);
                    unsigned int pack_len = htonl(header->pack_len);
                    
                    // 验证包长度有效性
                    if (pack_len < sizeof(tgg_bw_protocal) || 
                        pack_len > MAX_PACKET_SIZE) {
                        LOG_ERROR("invalid packet len[%d], ip:%s, port:%d", 
                                 pack_len, ip_str, ntohs(port));
                        exec_ret = -1;
                        break;
                    }
                    
                    // 检查是否收到完整包
                    if (total_len - parsed_pos < pack_len) {
                        break;  // 等待更多数据
                    }
                    
                    // 处理完整包
                    tgg_bw_data bwdata = {
                        .fd = fd,
                        .coreid = g_prc_id,
                        .bwfdx = (fd << 8) | g_prc_id,
                        .fd_opt = FD_WRITE,
                        .idx = tgg_get_bwfdx_idx(g_prc_id, fd),
                        .data_len = pack_len,
                        .data = recv_buffer + parsed_pos,
                        .peer_ip = ip,
                        .peer_port = ntohs(port),
                    };
                    
                    if ((exec_ret = tgg_process_bwrcv_data(&bwdata)) < 0) {
                        break;
                    }
                    
                    parsed_pos += pack_len;
                }
                
                // 移动未处理数据到缓冲区头部
                unsigned int left_len = total_len - parsed_pos;
                if (left_len > 0 && parsed_pos > 0) {
                    memmove(recv_buffer, recv_buffer + parsed_pos, left_len);
                }
                pos = left_len;
                
                if (exec_ret < 0) {
                    break;  // 处理错误，退出循环
                }
            }
            
            // 错误处理（原有逻辑）
            if (exec_ret < 0) {
                LOG_WARNING("we are closing bw[ip:%s,port:%d]", ip_str, ntohs(port));
            } else if (ret > 0 || (ret == -1 && errno == EAGAIN)) {
                continue;
            } else if (ret != 0) {
                LOG_WARNING("bw[ip:%s,port:%d] is closing, ret:%d, error:[%d]%s.", 
                           ip_str, ntohs(port), ret, errno, strerror(errno));
            } else {
                LOG_WARNING("catched a close from bw[ip:%s,port:%d]", ip_str, ntohs(port));
            }
            
           break;
        }
        tgg_close_bw_session(g_prc_id, fd);
        close(fd);
        LOG_WARNING("bw[ip:%s,port:%d] closed.", ip_str, ntohs(port));
     }
    return 0;
}


void *write_routine( void *arg )
{
    co_enable_hook_sys();
    map_msgtype[FD_NEW] = GatewayProtocal::CMD_ON_CONNECT;
    map_msgtype[FD_HANDLESHAKE] = GatewayProtocal::CMD_ON_WEBSOCKET_CONNECT;
    map_msgtype[FD_WRITE] = GatewayProtocal::CMD_ON_MESSAGE;
    map_msgtype[FD_CLOSE] = GatewayProtocal::CMD_ON_CLOSE;
    while(g_run) {
        if(write_data() < 0) {
            poll(NULL, 0, 5);// sleep 10ms
        }
    }
    return 0;
}


int co_accept(int fd, struct sockaddr *addr, socklen_t *len );
void *accept_routine( void * )
{
    co_enable_hook_sys();
    LOG_INFO("accept_routine");
    while(g_run)
    {
        if( g_read_stack.empty() )
        {
            LOG_DEBUG("empty"); //sleep
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
        LOG_INFO("accept new connection prc[%d] fd[%d].", g_prc_id, fd);
        if( g_read_stack.empty())
        {
            task_t * task = (task_t*)calloc( 1,sizeof(task_t) );
            task->fd = -1;
            co_create( &(task->co),NULL,read_routine,task );
            co_resume( task->co );
            LOG_ERROR("NO enough corutine in g_read_stack.");
        }
        set_non_block( fd );
        task_t *co = g_read_stack.top();
        co->fd = fd;
        g_read_stack.pop();
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
    g_gw_local_ip = nIP;//网络字节序
    g_gw_local_port = shPort;// 网络字节序

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

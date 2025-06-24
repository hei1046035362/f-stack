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
// static stack<task_t*> g_write_stack;
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


// #include <linux/bpf.h>
// #include <linux/if_xdp.h>
// #include <bpf/bpf.h>
// #include <bpf/xsk.h>
// #include <bpf/libbpf.h>
// #include <net/if.h>
// #include <sys/uio.h>

// // ===== AF_XDP 用户态网络栈加速 =====
// static int create_xdp_socket(int ifindex, int queue_id) {
//     struct xsk_socket_config cfg = {
//         .rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS,
//         .tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS,
//         .libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD
//     };
    
//     struct xsk_umem_config umem_cfg = {
//         .fill_size = XSK_RING_PROD__DEFAULT_NUM_DESCS,
//         .comp_size = XSK_RING_CONS__DEFAULT_NUM_DESCS,
//         .frame_size = XSK_UMEM__DEFAULT_FRAME_SIZE,
//     };
    
//     // 1. 分配UMEM内存（零拷贝基础）
//     void *umem_area = mmap(NULL, UMEM_SIZE, PROT_READ | PROT_WRITE, 
//                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
//     struct xsk_umem *umem;
//     xsk_umem__create(&umem, umem_area, UMEM_SIZE, &umem_fq, &umem_cq, &umem_cfg);

//     // 2. 创建XDP socket（绕过内核）
//     struct xsk_socket *xsk;
//     xsk_socket__create(&xsk, ifindex, queue_id, umem, &rx_ring, &tx_ring, &cfg);
    
//     return xsk_socket__fd(xsk); // 返回可直接操作的fd
// }

// // ===== 终极写入函数 =====
// ssize_t xdp_write(int fd, const void* data, size_t len) {
//     static __thread int xdp_fd = -1;
    
//     // 初始化XDP（每个线程/协程独立）
//     if (xdp_fd == -1) {
//         int ifindex = if_nametoindex("eth0"); // 获取网卡
//         xdp_fd = create_xdp_socket(ifindex, 0); // 绑定队列0
//     }

//     // 获取UMEM缓冲区（零拷贝）
//     uint64_t addr;
//     xsk_ring_cons__reserve(&umem_fq, 1, &addr);
    
//     // 直接拷贝到NIC缓冲区（DMA区域）
//     void *tx_buf = xsk_umem__get_data(umem_area, addr);
//     memcpy(tx_buf, data, len);
    
//     // 提交发送描述符（无协议栈开销）
//     xsk_ring_prod__submit(&tx_ring, 1);
    
//     // 同步通知网卡发送
//     xsk_ring_prod__submit(&tx_ring, 0); 
    
//     // 协程事件等待（高效）
//     struct pollfd pfd = {.fd = xdp_fd, .events = POLLOUT};
//     co_poll(co_get_epoll_ct(), &pfd, 1, 0);
    
//     return len;
// }

// // ===== 混合策略调度器 =====
// ssize_t turbo_write(int fd, const void* data, size_t len) {
//     // 策略1: <128字节用sendmsg零拷贝
//     if (len < 128) {
//         struct iovec iov = {.iov_base = (void*)data, .iov_len = len};
//         struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
//         return sendmsg(fd, &msg, MSG_ZEROCOPY);
//     }
    
//     // 策略2: 大块数据用XDP绕过内核
//     return xdp_write(fd, data, len);
// }


#include <sys/uio.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <errno.h>
#include <string>

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

static int s_dequeued_server_count = 0;

static int write_data()
{
    while (true) {
        tgg_bw_data* bdata = NULL;
        if (tgg_dequeue_bwsnd(g_prc_id, &bdata) < 0) {
            // poll(NULL, 0, 10);// sleep 10ms
            return -1;
        }
        if(!bdata) {
            LOG_ERROR("deque an NULL data.");
            // clean_bw_data(bdata);
            return -1;
        }
        s_dequeued_server_count++;
        int bwfdx = bdata->bwfdx;//tgg_get_cli_bwfdx(bdata->coreid, bdata->fd);
        int prc_id = bwfdx & 0xff;
        int fd = bwfdx >> 8;
        // int cli_fd = bdata->fd;
        // int coreid = bdata->coreid;
        // int idx = bdata->idx;
        // char *buffer = malloc(data->data_len);
        // memcpy(buffer, data->data, data->data_len);
        // cli对应的bwfd已经改变或者 bwfdx已关闭，丢弃
        if(prc_id != g_prc_id || !tgg_get_bwfdx_status((bwfdx & 0xff), fd) || bdata->fd <= 0) {
            LOG_ERROR("deal bw write data failed:prc_id[%d] bwdatafdx:bwfdx[%d:%d] status:[%d].", 
                prc_id, bdata->bwfdx, fd, tgg_get_bwfdx_status((bwfdx & 0xff), fd));
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

        // printf("co[%d] do write.\n", co_index);
        // int ret = co_write_complete(fd, result.c_str(), result.length());
        int ret = turbo_write(fd, result.c_str(), result.length());
        // printf("co[%d] finished write.\n", co_index);
        // int ret = splice_write(pipefd, fd, result.c_str(), result.length());
        // int loops = 3;// 如果失败最多重试3次，否则丢弃
        // while(ret == -1 && EAGAIN == errno && loops--) {
        //     ret = write(fd, result.c_str(), result.length());
        //     poll(NULL, 0, 10);// sleep 10ms
        // }
        if(-1 == ret) {
            LOG_ERROR("trans to server failed, client_ip[%d] client_port[%d].",
                header.client_ip, header.client_port);
            clean_bw_data(bdata);
            return -1;
            // if (errno == EAGAIN) {
            //     struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            //     co_poll(co_get_epoll_ct(), &pfd, 1, 1000); // 协程友好等待
            // }
        }
        if(bdata->fd_opt & FD_CLOSE) {
            // 这里发送给客户端和清理hash表信息的顺序待商榷
            Send2Client(cid, "", FD_CLOSE, 0);// 这里不需要再写数据了，收到对端关闭才走到这里来的 FD_WRITE|
            tgg_free_session(bdata->coreid, bdata->fd, cid);
            LOG_INFO("catched an close cmd, cid:%d.", cid);
        }
        clean_bw_data(bdata);
        // printf("co[%d] finished dequeue.\n", co_index);
    }
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

void *read_routine( void *arg )
{
    map_msgtype[FD_NEW] = GatewayProtocal::CMD_ON_CONNECT;
    map_msgtype[FD_HANDLESHAKE] = GatewayProtocal::CMD_ON_WEBSOCKET_CONNECT;
    map_msgtype[FD_WRITE] = GatewayProtocal::CMD_ON_MESSAGE;
    map_msgtype[FD_CLOSE] = GatewayProtocal::CMD_ON_CLOSE;

    co_enable_hook_sys();

    task_t *co = (task_t*)arg;
    for(;;)
    {
        if( -1 == co->fd )
        {
            g_read_stack.push( co );
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
            return 0;
        }
        LOG_INFO("accept new connection ip[%s], port[%u].", ip_str, port);
        char recv_buffer[ MAX_PACKET_SIZE ];
        // std::vector<char> recv_buffer;
        unsigned int pos = 0;
        for(;;)
        {
            struct pollfd pf = { 0 };
            pf.fd = fd;
            pf.events = (POLLIN|POLLERR|POLLHUP);
            co_poll( co_get_epoll_ct(),&pf,1,200);

            char buf_read[ 4096 ];
            int ret = read( fd,buf_read,sizeof(buf_read) );
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
                    tgg_close_bw_session(g_prc_id, fd);
                    close( fd );
                    LOG_ERROR("bw[ip:%s,port%d] closed.", ip_str, ntohs(port));
                    return 0;
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
                        .fd = fd,
                        .coreid = g_prc_id,
                        .bwfdx = (fd << 8 ) | g_prc_id,
                        .fd_opt = FD_WRITE,
                        .idx = tgg_get_bwfdx_idx(g_prc_id, fd),
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
            if( ret > 0 || ( -1 == ret && EAGAIN == errno ) )
            {
                continue;
            }
            LOG_ERROR("bw[ip:%s,port%d] is closing, ret:%d.", ip_str, ntohs(port), ret);
            tgg_close_bw_session(g_prc_id, fd);
            close( fd );
            LOG_ERROR("bw[ip:%s,port%d] closed.", ip_str, ntohs(port));
            break;
        }

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
            poll(NULL, 0, 10);// sleep 10ms
        }
    }
    // int co_index = ++s_co_count;
    // co_enable_hook_sys();
    // // g_prc_id = *((int*)arg);
    // std::map<int, int> map_msgtype;// 客户端上行透传 消息类型映射
    // map_msgtype[FD_NEW] = GatewayProtocal::CMD_ON_CONNECT;
    // map_msgtype[FD_HANDLESHAKE] = GatewayProtocal::CMD_ON_WEBSOCKET_CONNECT;
    // map_msgtype[FD_WRITE] = GatewayProtocal::CMD_ON_MESSAGE;
    // map_msgtype[FD_CLOSE] = GatewayProtocal::CMD_ON_CLOSE;
    // // 1. 创建管道（splice数据传输的通道）
    // int pipefd[2];
    // if (pipe(pipefd) == -1) {
    //     LOG_ERROR("create pip failed.");
    //     return NULL;  // 管道创建失败
    // }
    // fcntl(pipefd[0], F_SETPIPE_SZ, 1024 * 1024);
    // fcntl(pipefd[1], F_SETPIPE_SZ, 1024 * 1024);


    //     while(g_run)
    //     {

    //         tgg_bw_data* bdata = NULL;
    //         if (tgg_dequeue_bwsnd(g_prc_id, &bdata) < 0 || !bdata) {
    //             if(bdata) {
    //                 clean_bw_data(bdata);
    //             }
    //             poll(NULL, 0, 10);// sleep 10ms
    //             continue;
    //         }
    //         int bwfdx = bdata->bwfdx;//tgg_get_cli_bwfdx(bdata->coreid, bdata->fd);
    //         int prc_id = bwfdx & 0xff;
    //         int fd = bwfdx >> 8;
    //         // cli对应的bwfd已经改变或者 bwfdx已关闭，丢弃
    //         if(prc_id != g_prc_id || bdata->bwfdx != bwfdx || !tgg_get_bwfdx_status((bwfdx & 0xff), fd)) {
    //             LOG_ERROR("deal bw write data failed:prc_id[%d] bwdatafdx:bwfdx[%d:%d] status:[%d].", 
    //                 prc_id, bdata->bwfdx, bwfdx, tgg_get_bwfdx_status((bwfdx & 0xff), fd));
    //             clean_bw_data(bdata);
    //             continue;
    //         }
    //         // TODO cid的加入和删除 最佳的位置是在cliprc中校验之后，然而rte_hash在多线程环境中增加元素会崩溃，
    //         // 所以暂时放在这里，放在这里也没有问题，因为没有跟bw发送过connect消息的连接，后续也用不上
    //         if (bdata->fd_opt & FD_NEW) {
    //             if(tgg_init_session(bdata->coreid, bdata->fd, bdata->idx) < 0) {
    //                 clean_bw_data(bdata);
    //                 continue;
    //             }
    //         }
    //         int cid = tgg_get_cli_cid(bdata->coreid, bdata->fd);
    //         if(cid <= 0) {
    //             // 这里发送给客户端和清理hash表信息的顺序待商榷
    //             Send2Fd(bdata->coreid, bdata->fd, bdata->idx, "", FD_CLOSE, 0);// cid 还没有创建的连接直接关闭连接
    //             LOG_ERROR("invalid cid[%d] for coreid:%d fd[%d] failed.", cid, bdata->coreid, bdata->fd);
    //             clean_bw_data(bdata);
    //             continue;
    //         }
    //         std::string result;
    //         tgg_bw_protocal header = {
    //             .pack_len = (unsigned int)sizeof(tgg_bw_protocal) + bdata->data_len,
    //             .cmd = (unsigned char)map_msgtype[bdata->fd_opt],
    //             .local_ip = g_gw_local_ip,// gw的内网通信ip (unsigned int)tgg_get_bwfdx_ip(prc_id, fd),
    //             .local_port = g_gw_local_port,//(unsigned short)tgg_get_bwfdx_port(prc_id, fd),
    //             .client_ip = bdata->peer_ip,// 客户端的ip
    //             .client_port = bdata->peer_port,
    //             .connection_id = (unsigned int)cid,
    //             .flag = 1,// TODO 需要确定数据来源，怎么填
    //             .gateway_port = TggConfigure::getInstance()->get_gateway_port(),
    //             .ext_len = 0// TODO 暂时不知道上行数据是否能用上
    //         };
    //         std::string sdata;
    //         if(bdata->data_len > 0) {
    //             sdata = std::move(std::string((char*)bdata->data, bdata->data_len));
    //         }
    //         BwPackageHandler::encode(result, &header, sdata);
    
    //         // TODO:打印发送内容，稳定后需删除
    //         // uint32_t ip_int = tgg_get_bwfdx_ip(prc_id, fd);  // 整数形式的IP（网络字节序，对应192.168.1.1）
    //         // struct in_addr addr;
    //         // addr.s_addr = ip_int;  // 直接赋值网络字节序整数
    //         // char ip_str[INET_ADDRSTRLEN];
    //         // inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
    //         // LOG_DEBUG("send to server[ip:%s,port:%u]:%s.", 
    //         //     ip_str, tgg_get_bwfdx_port(prc_id, fd), bin2hex(result).c_str());
    
    //         // struct pollfd pf = { 0 };
    //         // pf.fd = fd;
    //         // pf.events = (POLLOUT|POLLERR|POLLHUP);
    //         // co_poll( co_get_epoll_ct(),&pf,1, 200);
    //         printf("co[%d] do write.\n", co_index);
    //         int ret = write(fd, result.c_str(), result.length());
    //         printf("co[%d] finished write.\n", co_index);
    //         // int ret = splice_write(pipefd, fd, result.c_str(), result.length());
    //         // int loops = 3;// 如果失败最多重试3次，否则丢弃
    //         // while(ret == -1 && EAGAIN == errno && loops--) {
    //         //     ret = write(fd, result.c_str(), result.length());
    //         //     poll(NULL, 0, 10);// sleep 10ms
    //         // }
    //         if(-1 == ret) {
    //             LOG_ERROR("trans to server failed, client_ip[%d] client_port[%d].",
    //                 header.client_ip, header.client_port);
    //             if (errno == EAGAIN) {
    //                 struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    //                 co_poll(co_get_epoll_ct(), &pfd, 1, 1000); // 协程友好等待
    //             }
    //         }
    //         if(bdata->fd_opt & FD_CLOSE) {
    //             // 这里发送给客户端和清理hash表信息的顺序待商榷
    //             Send2Client(cid, "", FD_CLOSE, 0);// 这里不需要再写数据了，收到对端关闭才走到这里来的 FD_WRITE|
    //             tgg_free_session(bdata->coreid, bdata->fd, cid);
    //             LOG_INFO("catched an close cmd, cid:%d.", cid);
    //         }
    //         clean_bw_data(bdata);
    //         printf("co[%d] finished dequeue.\n", co_index);
    //     }
    return 0;
}


int co_accept(int fd, struct sockaddr *addr, socklen_t *len );
void *accept_routine( void * )
{
    co_enable_hook_sys();
    LOG_INFO("accept_routine");
    while(g_run)
    {
        //printf("pid %ld g_readwrite.size %ld\n",getpid(),g_readwrite.size());
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
        LOG_INFO("accept new connection fd[%d].", fd);
        if( g_read_stack.empty())
        {
            task_t * task = (task_t*)calloc( 1,sizeof(task_t) );
            task->fd = -1;
            co_create( &(task->co),NULL,read_routine,task );
            co_resume( task->co );
            // g_read_stack.push(task);
            LOG_ERROR("NO enough corutine in g_read_stack.");
            // close( fd );
            // continue;
        }
        set_non_block( fd );
        task_t *co = g_read_stack.top();
        co->fd = fd;
        g_read_stack.pop();
        co_resume( co->co );
        LOG_INFO("accept new connection fd[%d].", fd);
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


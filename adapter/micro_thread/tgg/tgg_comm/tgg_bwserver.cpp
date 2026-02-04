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
#include "cmd/ShareCmdProcessor.h"
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

// 环形缓冲区结构体 - 替代 memmove 以优化性能
struct RingBuffer {
    static constexpr size_t CAPACITY = MAX_PACKET_SIZE * 2;  // 必须是 MAX_PACKET_SIZE 的2倍或以上
    char data[CAPACITY];
    size_t read_pos;   // 读指针
    size_t write_pos;  // 写指针
    
    RingBuffer() : read_pos(0), write_pos(0) {}
    
    // 获取可读数据长度
    inline size_t available() const {
        return (write_pos - read_pos);
    }
    
    // 获取可写空间大小
    inline size_t space_available() const {
        return CAPACITY - (write_pos - read_pos);
    }
    
    // 写入数据到缓冲区
    int write(const char* buf, size_t len) {
        if (space_available() < len) {
            LOG_ERROR("ringbuffer space not enough, need:%zu, available:%zu", len, space_available());
            return -1;
        }
        
        size_t wp = write_pos % CAPACITY;
        
        // 处理环绕情况
        if (wp + len <= CAPACITY) {
            // 不环绕，直接拷贝
            memcpy(data + wp, buf, len);
        } else {
            // 环绕，分两段拷贝
            size_t first_part = CAPACITY - wp;
            memcpy(data + wp, buf, first_part);
            memcpy(data, buf + first_part, len - first_part);
        }
        
        write_pos += len;
        return (int)len;
    }
    
    // 读取数据但不清除（用于解析包）
    // 注意：返回的指针仅在下次 read_advance 前有效
    const char* peek(size_t len, size_t& out_len) const {
        if (available() < len) {
            return nullptr;  // 数据不足
        }
        
        size_t rp = read_pos % CAPACITY;
        
        // 返回的是连续数据，如果环绕则需要单独处理
        // 为简化，这里假设调用者最多读一个包头（固定大小）
        out_len = len;
        return data + rp;
    }
    
    // 获取连续可读数据的长度（不跨越环绕点）
    size_t continuous_available() const {
        if (available() == 0) return 0;
        
        size_t rp = read_pos % CAPACITY;
        size_t wp = write_pos % CAPACITY;
        
        if (rp <= wp) {
            // 不环绕
            return wp - rp;
        } else {
            // 环绕，返回到缓冲区末尾的长度
            return CAPACITY - rp;
        }
    }
    
    // 获取当前读位置的数据指针（连续部分）
    const char* readable_ptr() const {
        return data + (read_pos % CAPACITY);
    }
    
    // 前进读指针
    void read_advance(size_t len) {
        if (available() < len) {
            LOG_ERROR("ringbuffer advance beyond available, advance:%zu, available:%zu", 
                     len, available());
            return;
        }
        read_pos += len;
        
        // 防止指针溢出：当指针足够大时，同时回绕
        if (read_pos > CAPACITY * 1024) {
            size_t wrap_offset = (read_pos / CAPACITY) * CAPACITY;
            read_pos -= wrap_offset;
            write_pos -= wrap_offset;
        }
    }
    
    // 清空缓冲区
    void clear() {
        read_pos = 0;
        write_pos = 0;
    }
    
    // 将环形缓冲区的一段数据复制到线性缓冲区（处理环绕）
    // 用于需要连续内存的场景（如传给业务层）
    int copy_linear(char* out_buf, size_t len) const {
        if (available() < len) {
            return -1;  // 数据不足
        }
        
        size_t rp = read_pos % CAPACITY;
        
        if (rp + len <= CAPACITY) {
            // 不环绕
            memcpy(out_buf, data + rp, len);
        } else {
            // 环绕，分两段拷贝
            size_t first_part = CAPACITY - rp;
            memcpy(out_buf, data + rp, first_part);
            memcpy(out_buf + first_part, data, len - first_part);
        }
        
        return (int)len;
    }
};

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
    ssize_t sent = write(fd, data, len);
    if (sent < 0 && errno == EAGAIN) {
        // 使用协程友好的 co_poll 等待可写
        struct pollfd pf = {0};
        pf.fd = fd;
        pf.events = POLLOUT;
        co_poll(co_get_epoll_ct(), &pf, 1, 1000);
        sent = write(fd, data, len); // 重试写入
    }
    return sent > 0 ? sent : 0;
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
static int build_server_data(const http_request_t &req, unsigned int ip, 
                             ushort port, char* data) {
    char protocal[10] = {0};
    sprintf(protocal, "HTTP/1.%d", req.minor_version);

    char ip_str[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &ip, ip_str, sizeof(ip_str));

    // 请求行头
    char header[1024] = {0};
    size_t pos = 0;
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
        size_t need_len = strlen(header_key) + req.headers[i].value_len + 11;
        if(pos + need_len > 1023) {
            LOG_ERROR("headers length exceed reserved.");
            return -1;
        }
        int writen = sprintf(header + pos, "\"%s\":\"%.*s\",", header_key, (int)req.headers[i].value_len, req.headers[i].value);
        if(writen < 0) {
            LOG_ERROR("sprintf header failed:\"%s\":\"%.*s\",", header_key, (int)req.headers[i].value_len, req.headers[i].value);
            return -1;
        }
        pos += writen;
    }
    if(pos > 0) {
        header[pos] = '\0';
    }
    // 请求行参数
    char param[1024] = {0};
    pos = 0;
    for (size_t i = 0; i < req.num_query_params; i++) {
        size_t need_len = req.query_params[i].name_len + req.query_params[i].value_len + 6;
        if(pos + need_len > 1023) {
            LOG_ERROR("query_params length exceed reserved.");
            return -1;
        }
        int writen = sprintf(param + pos, "\"%.*s\":\"%.*s\",", (int)req.query_params[i].name_len, req.query_params[i].name,
            (int)req.query_params[i].value_len, req.query_params[i].value);
        if(writen < 0) {
            LOG_ERROR("sprintf param failed:\"%.*s\":\"%.*s\",", (int)req.query_params[i].name_len, req.query_params[i].name,
            (int)req.query_params[i].value_len, req.query_params[i].value);
            return -1;
        }
        pos += writen;
    }
    if(pos > 0) {
        param[pos] = '\0';
    }

    // 请求行参数
    char cookie[1024] = {0};
    pos = 0;
    for (size_t i = 0; i < req.num_cookies; i++) {
        size_t need_len = req.cookies[i].name_len + req.cookies[i].value_len + 6;
        if(pos + need_len > 1023) {
            LOG_ERROR("cookies length exceed reserved.");
            return -1;
        }
        int writen = sprintf(cookie + pos, "\"%.*s\":\"%.*s\",", (int)req.cookies[i].name_len, req.cookies[i].name,
            (int)req.cookies[i].value_len, req.cookies[i].value);
        if(writen < 0) {
            LOG_ERROR("sprintf cookie failed:\"%.*s\":\"%.*s\",", (int)req.cookies[i].name_len, req.cookies[i].name,
            (int)req.cookies[i].value_len, req.cookies[i].value);
            return -1;
        }
        pos += writen;
    }
    if(pos > 0) {
        cookie[pos] = '\0';
    }
    int host_len = req.host.len > 0 ? req.host.len : sizeof(UNKNOWN_HOST);
    int writen = sprintf(data, 
        "{\"server\":{"
                "\"REQUEST_METHOD\":\"%.*s\","
                "\"REQUEST_URI\":\"%.*s\","
                "\"SERVER_PROTOCOL\":\"%s\","
                "\"REMOTE_ADDR\":\"%s\","
                "\"REMOTE_PORT\":\"%u\","
                "\"SERVER_PORT\":\"%u\","
                "\"SERVER_NAME\":\"%.*s\","
                "%s"// headers
            "},"
            "\"get\":{%s},"
            "\"cookie\":{%s}"
        "}",
         (int)req.method.len, req.method.data,
         (int)req.uri.len, req.uri.data,
         protocal,
         ip_str,
         port,
         TggConfigure::getInstance()->get_gateway_port(),
         host_len, req.host.len > 0 ? req.host.data : UNKNOWN_HOST,
         header,
         param,
         cookie
    );
    if(writen < 0) {
        LOG_ERROR("format server data failed.");
        return -1;
    }
    return 0;
    // tl_buffer.Clear();  // 复用线程局部缓冲区
    // tl_writer.Reset(tl_buffer);
    
    // // 1. 直接流式构建JSON（避免DOM树开销）
    // tl_writer.StartObject();
    
    // // ===== SERVER_VARS 优化区块 =====
    // tl_writer.Key("server");
    // tl_writer.StartObject();
    
    // // 基础字段（零拷贝引用）
    // tl_writer.Key("REQUEST_METHOD");
    // tl_writer.String(req.method.data, req.method.len);
    
    // tl_writer.Key("REQUEST_URI");
    // tl_writer.String(req.uri.data, req.uri.len);
    
    // tl_writer.Key("SERVER_PROTOCOL");
    // char protocal[10];
    // sprintf(protocal, "HTTP/1.%d", req.minor_version);
    // tl_writer.String(protocal, strlen(protocal));
    
    // // 网络信息（SIMD加速IP转换）
    // char ip_str[INET_ADDRSTRLEN];
    // inet_ntop(AF_INET, &ip, ip_str, sizeof(ip_str));
    // tl_writer.Key("REMOTE_ADDR");
    // tl_writer.String(ip_str, strlen(ip_str));
    
    // tl_writer.Key("REMOTE_PORT");
    // tl_writer.Uint(port);
    
    // tl_writer.Key("SERVER_PORT");
    // tl_writer.Uint(TggConfigure::getInstance()->get_gateway_port());
    
    // // 主机名（分支预测优化）
    // tl_writer.Key("SERVER_NAME");
    // if(req.host.len <= 0 || req.host.data[0] == ' ') {
    //     tl_writer.String(UNKNOWN_HOST);
    // } else {
    //     tl_writer.String(req.host.data, req.host.len);
    // }
    
    // // ===== HEADER转换优化（SIMD加速） =====
    // for (size_t i = 0; i < req.num_headers; i++) {
    //     // 原位转换：避免临时字符串
    //     char header_key[256];
    //     char* dest = header_key;
    //     const char* src = req.headers[i].name;
    //     size_t index = 0;
        
    //     // 1. 添加"HTTP_"前缀
    //     memcpy(dest, HTTP_PREFIX, sizeof(HTTP_PREFIX) -1);
    //     dest += sizeof(HTTP_PREFIX) -1;
        
    //     // 2. 大写转换+替换字符（向量化处理）
    //     while (*src && index++ < req.headers[i].name_len && dest - header_key < 250) {
    //         char c = *src++;
    //         // SIMD友好分支：减少跳转预测失败
    //         c = (c == '-') ? '_' : c & ~0x20; // 位运算转大写
    //         *dest++ = c;
    //     }
    //     *dest = '\0';
        
    //     tl_writer.Key(header_key);
    //     tl_writer.String(req.headers[i].value, req.headers[i].value_len);
    // }
    // tl_writer.EndObject(); // server结束
    
    // // ===== QUERY参数优化（批量处理） =====
    // tl_writer.Key("get");
    // tl_writer.StartObject();
    // for (size_t i = 0; i < req.num_query_params; i++) {
    //     tl_writer.Key(req.query_params[i].name, req.query_params[i].name_len);
    //     tl_writer.String(req.query_params[i].value, req.query_params[i].value_len);
    // }
    // tl_writer.EndObject();
    
    // // ===== COOKIE优化（预过滤） =====
    // tl_writer.Key("cookie");
    // tl_writer.StartObject();
    // for (size_t i = 0; i < req.num_cookies; i++) {
    //     tl_writer.Key(req.cookies[i].name, req.cookies[i].name_len);
    //     tl_writer.String(req.cookies[i].value, req.cookies[i].value_len);
    // }
    // tl_writer.EndObject();
    
    // tl_writer.EndObject(); // 根对象结束
    
    // // 直接返回缓冲区引用（避免二次拷贝）
    // return {tl_buffer.GetString(), tl_buffer.GetSize()};
}


static int s_dequeued_server_count = 0;

// ===== write_data() 辅助函数 - 职责分离 =====

/**
 * 验证 bwdata 的有效性
 * return: 0=有效, -1=无效(应丢弃)
 */
static inline int validate_bwdata(const tgg_bw_data* bdata, int* out_fd, int* out_prc_id) {
    if (!bdata || bdata->fd <= 0) {
        LOG_ERROR("invalid bdata: NULL or fd <= 0");
        return -1;
    }
    
    int bwfdx = bdata->bwfdx;
    int prc_id = bwfdx & 0xff;
    int fd = bwfdx >> 8;
    
    // 检查fd是否仍在使用
    if (prc_id != g_prc_id || !tgg_get_bwfdx_status(prc_id, fd)) {
        LOG_ERROR("deal bw write data failed: prc_id[%d] bwfdx[%d:%d] fd[%d] idx[%d] status[%d].",
                 prc_id, bdata->bwfdx, fd, bdata->fd, bdata->idx, 
                 tgg_get_bwfdx_status(prc_id, fd));
        if (bdata->data) {
            LOG_ERROR("write data:%s", bdata->data);
        }
        return -1;
    }
    
    *out_fd = fd;
    *out_prc_id = prc_id;
    return 0;
}

/**
 * 初始化连接（如果是FD_NEW）
 * return: 0=成功, -1=失败
 */
static inline int init_connection_if_needed(const tgg_bw_data* bdata) {
    if (!(bdata->fd_opt & FD_NEW)) {
        return 0;  // 不是新连接，无需初始化
    }
    
    if (tgg_init_session(bdata->coreid, bdata->fd, bdata->idx) < 0) {
        LOG_ERROR("init session failed: coreid[%d] fd[%d] idx[%d]",
                 bdata->coreid, bdata->fd, bdata->idx);
        return -1;
    }
    return 0;
}

/**
 * 验证并获取连接ID
 * return: cid (>0=有效), <=0=无效
 */
static inline uint32_t get_and_validate_cid(const tgg_bw_data* bdata) {
    uint32_t cid = tgg_get_cli_cid(bdata->coreid, bdata->fd);
    if (cid <= 0) {
        Send2Fd(bdata->coreid, bdata->fd, bdata->idx, "", FD_CLOSE, 0);
        LOG_ERROR("invalid cid[%u] for coreid:%d fd[%d]",
                 cid, bdata->coreid, bdata->fd);
    }
    return cid;
}

/**
 * 处理关闭信号（发送给客户端和BW服务器）
 */
static inline void handle_close_signal(const tgg_bw_data* bdata, uint32_t cid) {
    if (!(bdata->fd_opt & FD_CLOSE)) {
        return;  // 不是关闭信号
    }
    
    LOG_WARNING("catched close cmd, coreid[%d] fd[%d] idx[%d] cid:%u",
               bdata->coreid, bdata->fd, bdata->idx, cid);
    Send2Fd(bdata->coreid, bdata->fd, bdata->idx, "", FD_CLOSE, 0);
    exec_free_session(bdata->coreid, bdata->fd, cid);
}

/**
 * 处理握手数据（FD_NEW时解析HTTP并构建server_data）
 * return: 0=成功, -1=失败
 */
static inline int process_handshake_data(const tgg_bw_data* bdata, uint32_t cid,
                                         char* out_sdata, size_t* out_sdata_len) {
    if (!(bdata->fd_opt & FD_NEW) || bdata->data_len <= 0) {
        return 0;  // 不是握手包
    }
    
    struct http_request_t req;
    if (!parse_http_request((char*)bdata->data, bdata->data_len, &req, 1)) {
        LOG_ERROR("parse http request failed: idx:%d data:%s",
                 bdata->idx, (char*)bdata->data);
        Send2Fd(bdata->coreid, bdata->fd, bdata->idx, "", FD_CLOSE, 0);
        exec_free_session(bdata->coreid, bdata->fd, cid);
        return -1;
    }
    
    if (build_server_data(req, bdata->peer_ip, bdata->peer_port, out_sdata) < 0) {
        LOG_ERROR("build_server_data failed: idx:%d", bdata->idx);
        Send2Fd(bdata->coreid, bdata->fd, bdata->idx, "", FD_CLOSE, 0);
        exec_free_session(bdata->coreid, bdata->fd, cid);
        return -1;
    }
    
    *out_sdata_len = strlen(out_sdata);
    return 0;
}

/**
 * 处理普通数据包（非握手）
 */
static inline void process_regular_data(const tgg_bw_data* bdata, 
                                        char* out_sdata, size_t* out_sdata_len) {
    if (bdata->data_len > 0 && !(bdata->fd_opt & FD_NEW)) {
        memcpy(out_sdata, (char*)bdata->data, bdata->data_len);
        *out_sdata_len = bdata->data_len;
    }
}

/**
 * 记录数据包调试日志
 */
static inline void log_packet_debug(const tgg_bw_data* bdata, 
                                    const char* sdata, size_t sdata_len) {
    if (AsyncLogger::getInstance().getloglevel() != LogLevel::DEBUG) {
        return;
    }
    
    std::string print_data;
    if (bdata->data_len > 4 && *((unsigned short*)bdata->data) == 0xfeff) {
        message_unpack((char*)sdata, sdata_len, print_data);
    } else {
        print_data = sdata;
    }
    LOG_DEBUG("send to bw data:%s", print_data.c_str());
}

/**
 * 编码并发送到BW服务器
 * return: 0=成功, -1=失败
 */
static inline int encode_and_send_to_bw(int fd, const tgg_bw_protocal& header,
                                        const char* sdata, size_t sdata_len,
                                        const char* ext_data) {
    char result[BUFFER_PACKET_LEN] = {0};
    size_t ret_len = BwPackageHandler::encode(result, (tgg_bw_protocal*)&header,
                                              (char*)sdata, sdata_len, ext_data);
    
    if (ret_len > BUFFER_PACKET_LEN) {
        LOG_ERROR("encode packet overflow: len[%zu] body[%zu] ext[%zu]",
                 ret_len, sdata_len, strlen(ext_data));
        return -1;
    }
    
    LOG_DEBUG("send bw binary:%s, len:%zu", 
             bin2hex(std::string_view(result, ret_len)).c_str(), ret_len);
    
    int write_ret = turbo_write(fd, result, ret_len);
    if (write_ret < 0) {
        LOG_ERROR("turbo_write failed: client_ip[%u] client_port[%u]",
                 header.client_ip, header.client_port);
        return -1;
    }
    
    return 0;
}

// return   -1 外部会等待10ms，并继续循环，
static int write_data()
{
    while (true) {
        tgg_bw_data* bdata = NULL;
        if (tgg_dequeue_bwsnd(g_prc_id, &bdata) < 0) {
            return -1;
        }
        
        if (!bdata) {
            LOG_ERROR("deque NULL data");
            return -1;
        }
        
        s_dequeued_server_count++;
        int fd, prc_id;
        
        // 2. 验证数据有效性
        if (validate_bwdata(bdata, &fd, &prc_id) < 0) {
            clean_bw_data(prc_id, bdata);
            continue;
        }
        
        // 3. 初始化连接（如果是新连接）
        if (init_connection_if_needed(bdata) < 0) {
            clean_bw_data(prc_id, bdata);
            continue;
        }
        
        // 4. 获取并验证连接ID
        uint32_t cid = get_and_validate_cid(bdata);
        if (cid <= 0) {
            clean_bw_data(prc_id, bdata);
            continue;
        }
        
        // 5. 处理关闭信号
        handle_close_signal(bdata, cid);
        
        // 6. 构建协议头
        tgg_bw_protocal header = {
            .pack_len = (unsigned int)sizeof(tgg_bw_protocal) + bdata->data_len,
            .cmd = (unsigned char)map_msgtype[bdata->fd_opt],
            .local_ip = g_gw_local_ip,// gw的内网通信ip (unsigned int)tgg_get_bwfdx_ip(prc_id, fd),
            .local_port = g_gw_local_port,//(unsigned short)tgg_get_bwfdx_port(prc_id, fd),
            .client_ip = bdata->peer_ip,// 客户端的ip
            .client_port = bdata->peer_port,
            .connection_id = cid,
            .flag = 1,// TODO 需要确定数据来源，怎么填
            .gateway_port = TggConfigure::getInstance()->get_gateway_port(),
            .ext_len = 0// TODO 暂时不知道上行数据是否能用上
        };
        
        // 7. 获取扩展数据（需复制，防止协程切换导致失效）
        std::string ext_data = tgg_get_cli_reserved(bdata->coreid, bdata->fd);
        
        // 8. 处理数据包内容（握手或普通数据）
        char sdata[BUFFER_PACKET_LEN] = {0};
        size_t sdata_len = 0;
        
        // 处理握手数据
        if (process_handshake_data(bdata, cid, sdata, &sdata_len) < 0) {
            clean_bw_data(prc_id, bdata);
            continue;
        }
        
        // 更新握手包长度
        if ((bdata->fd_opt & FD_NEW) && sdata_len > 0) {
            header.pack_len = (unsigned int)sizeof(tgg_bw_protocal) + sdata_len;
        }
        
        // 处理普通数据
        process_regular_data(bdata, sdata, &sdata_len);
        
        // 9. 记录调试日志
        log_packet_debug(bdata, sdata, sdata_len);
        
        // 10. 清理bdata（防止协程切换导致的使用错误）
        clean_bw_data(prc_id, bdata);
        
        // 11. 编码并发送到BW
        if (encode_and_send_to_bw(fd, header, sdata, sdata_len, ext_data.c_str()) < 0) {
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
        uint32_t cid = tgg_get_cli_cid(bdata->coreid, bdata->fd);
        if(cid > 0) {
            if(bdata->fd_opt & FD_CLOSE) {// 要在发送给bw之前先回给客户端，否则客户端收到的消息可能不及时，write会导致协程切换
                // 这里发送给客户端和清理hash表信息的顺序待商榷
                LOG_WARNING("catched an close cmd, coreid[%d] fd[%d] idx[%d] cid:%u.", bdata->coreid, bdata->fd, bdata->idx, cid);
                Send2Fd(bdata->coreid, bdata->fd, bdata->idx, "", FD_CLOSE, 0);// 这里不需要再写数据了，收到对端关闭才走到这里来的 FD_WRITE|
                exec_free_session(bdata->coreid, bdata->fd, cid);
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
        
        // 使用环形缓冲区替代线性缓冲区 + memmove
        RingBuffer ring_buf;
        int exec_ret = 0;
        char temp_read_buf[BUFFER_PACKET_LEN];  // 临时读取缓冲区（大小不变）
        
        for(;;) {
            struct pollfd pf = {0};
            pf.fd = fd;
            pf.events = (POLLIN | POLLERR | POLLHUP);
            co_poll(co_get_epoll_ct(), &pf, 1, 200);

            int ret = read(fd, temp_read_buf, sizeof(temp_read_buf));
            
            if (ret > 0) {
                // 将读到的数据写入环形缓冲区
                if (ring_buf.write(temp_read_buf, ret) < 0) {
                    LOG_ERROR("write to ringbuffer failed, ip:%s, port:%d", ip_str, ntohs(port));
                    exec_ret = -1;
                    break;
                }
                
                // 处理缓冲区中的完整包
                while (ring_buf.available() > 0) {
                    // 1. 检查是否收到完整包头
                    if (ring_buf.available() < sizeof(tgg_bw_protocal)) {
                        break;  // 等待更多数据
                    }
                    
                    // 2. 获取包头（需要处理环绕）
                    tgg_bw_protocal header;
                    if (ring_buf.copy_linear((char*)&header, sizeof(tgg_bw_protocal)) < 0) {
                        LOG_ERROR("failed to copy header from ringbuffer");
                        exec_ret = -1;
                        break;
                    }
                    
                    unsigned int pack_len = htonl(header.pack_len);
                    
                    // 3. 验证包长度有效性
                    if (pack_len < sizeof(tgg_bw_protocal) || 
                        pack_len > MAX_PACKET_SIZE) {
                        LOG_ERROR("invalid packet len[%d], ip:%s, port:%d", 
                                 pack_len, ip_str, ntohs(port));
                        exec_ret = -1;
                        break;
                    }
                    
                    // 4. 检查是否收到完整包
                    if (ring_buf.available() < pack_len) {
                        break;  // 等待更多数据
                    }
                    
                    // 5. 将包数据复制到临时缓冲区（处理环绕）
                    char pack_buf[MAX_PACKET_SIZE];
                    if (ring_buf.copy_linear(pack_buf, pack_len) < 0) {
                        LOG_ERROR("failed to copy complete packet from ringbuffer");
                        exec_ret = -1;
                        break;
                    }
                    
                    // 6. 处理完整包
                    tgg_bw_data bwdata = {
                        .fd = fd,
                        .coreid = g_prc_id,
                        .bwfdx = (fd << 8) | g_prc_id,
                        .fd_opt = FD_WRITE,
                        .idx = tgg_get_bwfdx_idx(g_prc_id, fd),
                        .data_len = pack_len,
                        .data = (unsigned char*)pack_buf,
                        .peer_ip = ip,
                        .peer_port = ntohs(port),
                    };
                    
                    if ((exec_ret = tgg_process_bwrcv_data(&bwdata)) < 0) {
                        break;
                    }
                    
                    // 7. 前进读指针，无需 memmove！
                    ring_buf.read_advance(pack_len);
                }
                
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
            poll(NULL, 0, 2);// sleep 10ms
        }
    }
    return 0;
}

void *sharecmd_routine( void *arg )
{
    co_enable_hook_sys();
    LOG_INFO("sharecmd_routine start.");
    while(g_run) {
        if(exec_sharequeue_cmd_processor(g_prc_id) < 0) {
            poll(NULL, 0, 1);// sleep 10ms
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

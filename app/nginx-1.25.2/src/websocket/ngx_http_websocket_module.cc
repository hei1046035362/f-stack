extern "C" {
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <rte_ring.h>
#include "websocket_frame.h"
#include "connection_pool.h"
#include "ring_handler.h"
}
#include "tgg_conf.h"
#include "tgg_common.h"
#include "tgg_struct.h"
#include "WsConsumer.h"
#include <string.h>
#include "comm/log.hpp"
#include "tgg_ip_filter.h"
#include "tgg_bw_cache.h"
#include "ff_api.h"
#include "dpdk_init.h"
#include "ff_config.h"
#include "comm/Encrypt.hpp"
#include "tgg_master_timers.h"
#include <unistd.h>

#define MAX_WS_GET_LEN 4096
// 模块上下文引用
extern ngx_module_t ngx_http_websocket_module;

// 全局队列
extern struct rte_ring *ws_read_ring;
extern struct rte_ring *ws_write_ring;

extern uint32_t g_fd_limit;
extern int g_core_id;
extern int64_t g_max_concurency;
extern uint32_t g_fd_mask;
// 进程是否退出  master进程退出不需要做什么事情，但是secondary退出前必须要释放他持有的内存
int g_run_status = 1;
int g_monitor_count = 0;
int sig_pipe[2];// 信号处理放入主函数异步处理，信号函数中很多系统函数不能调用，会崩溃死锁
static int64_t s_left_fd = 0;// 剩余客户端连接数
int* g_pid_check_times;

static const char* s_dump_file = "/var/corefiles/";//tgg_gw_master_core

static void ngx_http_websocket_exit_module(ngx_cycle_t *cycle);

void signal_handler(int signum)
{
    printf("gwrcv coreid[%d] catched signal:%d\n", g_core_id, signum);
    if(signum == SIGINT || signum == SIGTERM) {
        if(g_run_status) {
            g_run_status = 0;
            // ff_stop_run();
        }
    }
}

void sigchld_handler(int sig) {
    int saved_errno = errno;
    char buf[32];
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) { // 非阻塞回收所有僵尸进程[5,7](@ref)
        if(g_run_status && TggConfigure::getInstance()->get_auto_start()) {
            // 监控到子进程退出，立刻再启动一个
            int len = snprintf(buf, sizeof(buf), "%d_%d\n", pid, WTERMSIG(status));
            ssize_t ret = write(sig_pipe[1], buf, len);
            if(ret < 0) {
                printf("gwrcv child %d exit normal, write pipe failed.\n", pid);
            }
            if (WIFEXITED(status)) {
                printf("gwrcv child %d exit normal, exit code: %d\n", pid, WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                printf("gwrcv child %d exit by signal: %d\n", pid, WTERMSIG(status));
            }
        }
    }
    errno = saved_errno;
}

// WebSocket 连接上下文
typedef struct {
    ngx_http_request_t *request;
    ngx_connection_t *connection;
    // ngx_str_t client_id;
    ws_frame_buffer_t frame_buffer; // 帧缓冲区
} ngx_http_websocket_ctx_t;

// 前置声明
static void ngx_http_websocket_handler(ngx_event_t *rev);
static ngx_int_t ngx_http_websocket_process_input(ngx_http_request_t *r);
static ngx_int_t trans_upstream_data(int core_id, int fd, std::string_view data, int fd_opt);

// 查找特定请求头
static ngx_table_elt_t *find_header(ngx_http_request_t *r, const char *name, size_t len)
{
    ngx_list_part_t *part;
    ngx_table_elt_t *h;
    ngx_uint_t i;
    
    part = &r->headers_in.headers.part;
    h = (ngx_table_elt_t*)part->elts;
    
    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            
            part = part->next;
            h = (ngx_table_elt_t*)part->elts;
            i = 0;
        }
        
        if (h[i].key.len == len && ngx_strncasecmp(h[i].key.data, (u_char*)name, len) == 0) {
            return &h[i];
        }
    }
    
    return NULL;
}

// 查找特定请求头
static ngx_int_t get_header(ngx_http_request_t *r, u_char* data, ngx_int_t left_len)
{
    ngx_list_part_t *part;
    ngx_table_elt_t *h;
    ngx_uint_t i;
    
    part = &r->headers_in.headers.part;
    h = (ngx_table_elt_t*)part->elts;
    ngx_int_t reserved = left_len;
    ngx_int_t pos = 0;
    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            
            part = part->next;
            h = (ngx_table_elt_t*)part->elts;
            i = 0;
        }
        if(int(pos + h[i].key.len + h[i].value.len + 4) > reserved) {
            LOG_ERROR("request content length exceed max buffer_len:%d.", reserved);
            return 0;
        }
        memcpy(data+pos, h[i].key.data, h[i].key.len);
        pos += h[i].key.len;
        data[pos++] = ':';
        data[pos++] = ' ';
        memcpy(data+pos, h[i].value.data, h[i].value.len);
        pos += h[i].value.len;
        data[pos++] = '\r';
        data[pos++] = '\n';
        
        // if (h[i].key.len == len && ngx_strncasecmp(h[i].key.data, (u_char*)name, len) == 0) {
        //     return &h[i];
        // }
    }
    data[pos++] = '\r';
    data[pos++] = '\n';
    data[pos] = '\0';
    return pos;
}


static ngx_int_t extract_ip_port(const struct sockaddr *sa, char* ip_str, unsigned short* port, int* ip) {
    if (sa == NULL) return -1;
    
    
    if (sa->sa_family == AF_INET) {
        // IPv4 处理
        struct sockaddr_in *saddr = (struct sockaddr_in *)sa;
        
        // 获取IP字符串
        if (inet_ntop(AF_INET, &(saddr->sin_addr), ip_str, INET_ADDRSTRLEN) == NULL) {
            return -1;
        }
        
        // 获取IP数值（网络字节序）
        *ip = ntohl(saddr->sin_addr.s_addr);
        
        // 获取端口
        *port = ntohs(saddr->sin_port);
    } else {
        // 不支持的协议族
        strcpy(ip_str, "Unknown AF");
        *port = 0;
        return -1;
    }
    
    return 0;
}

static ngx_int_t init_tgg_cli(ngx_http_request_t *r, ngx_http_websocket_ctx_t *ctx)
{
    int fd = r->connection->fd & g_fd_mask;
    // 如果fd还在使用中，拒绝连接
    if (tgg_get_cli_idx(g_core_id, fd) != TGG_FD_CLOSED) {
        LOG_ERROR("socket fd[%d] still in use.", r->connection->fd);
        return -1;
    }
    char ip_str[INET_ADDRSTRLEN] = {0};
    int ip = 0;
    unsigned short port = 0;
    int ret = extract_ip_port(r->connection->sockaddr, ip_str, &port, &ip);
    // const char* result = inet_ntop(AF_INET, &(cli_info->ip), ip_str,  sizeof(ip_str));
    if(ret < 0) {
        LOG_ERROR("get connection ip string failed, fd:%d, ip:%d, port:%u", r->connection->fd, ip, port);
        // close(cli_info->cli_fd);
        // delete(cli_info);
        // cli_info = NULL;
        return -1;
    }
    // int idx = -1;
    bool exclude = is_ip_exclude(ip);// exclude的连接只recv，不进入业务逻辑
    if(!exclude) {
        if(tgg_init_cli(g_core_id, fd, ip_str, ip, port) < 0) {
            LOG_ERROR("init client info failed.");
            // close(cli_info->cli_fd);
            // tgg_close_cli(g_core_id, cli_info->cli_fd);
            // delete(cli_info);
            return -1;
        }
        // idx = tgg_get_cli_idx(g_core_id, r->connection->fd);
    }
    tgg_set_cli_ctx(g_core_id, fd, ctx);
    return 0;
}

static void clean_client_data(int cli_fd)
{
    tgg_close_cli(g_core_id, cli_fd);
    release_ws_buffer(g_core_id, cli_fd);
}

static void destroy_tgg_cli(int fd)
{
    LOG_INFO("destroy client.");
    ngx_http_websocket_ctx_t *ctx = (ngx_http_websocket_ctx_t*)tgg_get_cli_ctx(g_core_id, fd);
    if(ctx && ctx->connection) {
        ngx_http_close_connection(ctx->connection);
    }
    // ws_connection_pool_remove(fd);
    // int idx = tgg_get_cli_idx(g_core_id, fd);
    clean_client_data(fd);

}

static ngx_int_t
ngx_http_websocket_upgrade(ngx_http_request_t *r)
{
    ngx_table_elt_t *upgrade, *sec_key;
    size_t header_len;
    size_t request_len;
    // size_t raw_data_len;
    ngx_http_websocket_ctx_t *ctx = NULL;
    ngx_int_t rc;
    u_char accept_key[29];
    ngx_table_elt_t *h;
    int fd = r->connection->fd & g_fd_mask;
    u_char* raw_data;
    LOG_INFO("accept http request, fd[%d]", fd);
    // 必须是 GET
    if (!(r->method & NGX_HTTP_GET)) {
        LOG_WARNING("Only GET method allowed.");
        return NGX_HTTP_NOT_ALLOWED;
    }
    // 健康检查
    // if(r->uri.len >= 12 && !ngx_strncasecmp(r->uri.data, (u_char*)"/healthcheck", 12)) {
    //     r->headers_out.status = NGX_HTTP_NO_CONTENT;
    //     r->header_only = 1;
    //     r->keepalive = 0;
    //     r->lingering_close = 1;
    //     rc = ngx_http_send_header(r);
    //     if (rc == NGX_ERROR || rc > NGX_OK) {
    //         LOG_WARNING("healthcheck failed, send header failed.");
    //         return rc;
    //     }
    //     LOG_DEBUG("healthcheck success.");
    //     return NGX_DONE;
    // }

    upgrade = find_header(r, "Upgrade", 7);
    if (upgrade == NULL || (ngx_strlen(upgrade->value.data) != 9) || ngx_strncasecmp(upgrade->value.data, (u_char*)"websocket", 9) != 0) {
        LOG_WARNING("Check Upgrade failed.");
        return NGX_DECLINED;
    }

    sec_key = find_header(r, "Sec-WebSocket-Key", 17);
    if (sec_key == NULL || sec_key->value.len != 24) {  // Base64 encoded 16-byte nonce
        LOG_WARNING("Check Sec-WebSocket-Key failed.");
        return NGX_HTTP_BAD_REQUEST;
    }

    // === 设置响应 ===
    r->headers_out.status = NGX_HTTP_SWITCHING_PROTOCOLS;
    r->header_only = 1;
    r->keepalive = 0;
    r->lingering_close = 0;

    ngx_http_clear_content_length(r);
    ngx_http_clear_accept_ranges(r);

    // --- 添加 Upgrade: websocket ---
    h = (ngx_table_elt_t *)ngx_list_push(&r->headers_out.headers);
    if (h == NULL) goto failed;
    h->hash = 1;
    ngx_str_set(&h->key, "Upgrade");
    ngx_str_set(&h->value, "websocket");

    // --- 添加 Connection: Upgrade ---
    h = (ngx_table_elt_t *)ngx_list_push(&r->headers_out.headers);
    if (h == NULL) goto failed;

    h->hash = 1;
    ngx_str_set(&h->key, "Connection");
    ngx_str_set(&h->value, "Upgrade");

    // --- 添加 Sec-WebSocket-Accept ---
    ngx_http_websocket_calc_accept(sec_key->value.data, sec_key->value.len, accept_key);

    h = (ngx_table_elt_t *)ngx_list_push(&r->headers_out.headers);
    if (h == NULL) goto failed;

    h->hash = 1;
    ngx_str_set(&h->key, "Sec-WebSocket-Accept");
    h->value.data = accept_key;
    h->value.len = 28;

    // === 发送 header ===
    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK) {
        LOG_WARNING("ws send header failed.");
        return rc;
    }

    // === 创建上下文 ===
    ctx = (ngx_http_websocket_ctx_t *)ngx_pcalloc(r->pool, sizeof(ngx_http_websocket_ctx_t));
    if (ctx == NULL) goto failed;

    ctx->request = r;
    ctx->connection = r->connection;
    // ctx->client_id.len = 32;
    // ctx->client_id.data = ngx_pnalloc(r->pool, 32);
    // if (ctx->client_id.data == NULL) goto failed;

    // ngx_snprintf(ctx->client_id.data, 32, "%V-%T", &r->connection->addr_text, ngx_time());

    ws_frame_buffer_init(&ctx->frame_buffer);
    ngx_http_set_ctx(r, ctx, ngx_http_websocket_module);

    // 防止请求被释放
    r->main->count++;

    // 切换读事件处理器
    r->connection->data = r;
    r->connection->read->handler = ngx_http_websocket_handler;

    // 如果已经有数据到来，直接调度
    if (r->connection->read->ready) {
        ngx_post_event(r->connection->read, &ngx_posted_events);
    } else {
        ngx_add_event(r->connection->read, NGX_READ_EVENT, 0);
    }

    // 注册连接池
    // ws_connection_pool_add(ctx->client_id, r->connection);
    
    if(init_tgg_cli(r, ctx) < 0) {
        goto failed;
    }
    // if(tgg_init_cli(g_core_id, r->connection->fd, r->connection->addr_text, 
    //     ((struct sockaddr_in*)r->connection->sockaddr)->sin_addr.s_addr, 
    //     ((struct sockaddr_in*)r->connection->sockaddr)->sin_port) < 0) {
    //     LOG_ERROR("init client info failed.");
    //     goto failed;
    //     // close(r->connection->fd);
    //     // return;
    // }
    
    if (r->header_in == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No header_in buffer");
        goto failed;
    }
    raw_data = (u_char*)ngx_pcalloc(r->pool, MAX_WS_GET_LEN);
    request_len = r->request_end - r->request_start;
    memcpy(raw_data, (const char*)r->request_start, request_len);
    raw_data[request_len++] = '\r';
    raw_data[request_len++] = '\n';
    header_len = get_header(r, raw_data + request_len, MAX_WS_GET_LEN - request_len);
    if(header_len == 0) {
        ngx_pfree(r->pool, raw_data);
        LOG_ERROR("get ws headers failed.");
        goto failed;
    }
    LOG_INFO("new websocket client accept raw_data:%s.", raw_data);

    trans_upstream_data(g_core_id, fd, std::string_view((const char*)raw_data, request_len+header_len), FD_NEW);
    ngx_pfree(r->pool, raw_data);
    // LOG_INFO("new websocket client accept request:%.*s.", (const char*)r->request_end - (const char*)r->request_start, (const char*)r->request_start);
    // LOG_INFO("new websocket client accept rline:%.*s.", r->request_line.len, (const char*)r->request_line.data);
    // LOG_INFO("new websocket client accept uri:%.*s.", r->uri.len, (const char*)r->uri.data);
    // LOG_INFO("new websocket client accept args:%.*s.", r->args.len, (const char*)r->args.data);
    // LOG_INFO("new websocket client accept exten:%s.*.", r->exten.len, (const char*)r->exten.data);
    // LOG_INFO("new websocket client accept header_start:%.*s.", (const char*)r->header_end - (const char*)r->header_start, (const char*)r->header_start);
    // 返回 DONE，表示协议升级完成
    return NGX_DONE;

failed:
    destroy_tgg_cli(fd);
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
}
static void send_frame(int fd, const char* data, int data_len)
{
    ngx_http_websocket_ctx_t* ctx = (ngx_http_websocket_ctx_t*)tgg_get_cli_ctx(g_core_id, fd);
    if(ctx && ctx->connection) {
        ngx_int_t result = NGX_AGAIN, try_times = 10;
        while (result == NGX_AGAIN && try_times-- > 0) {
            result = ngx_send(ctx->connection, (u_char*)data, data_len);
        }
        if (result < NGX_OK) {
            // TODO 设计发送次数限制，防止发不出去一直发，超过次数可以直接关闭
            LOG_ERROR("send frame failed coreid[%d] fd[%d].", g_core_id, fd);
        }
    } else {
        LOG_ERROR("send frame failed ctx[%p] connection[%p] coreid[%d] fd[%d].", ctx, ctx->connection, g_core_id, fd);
    }
}
// WebSocket 事件处理
static void ngx_http_websocket_handler(ngx_event_t *rev)
{
    ngx_connection_t *c = (ngx_connection_t *)rev->data;
    ngx_http_request_t *r = (ngx_http_request_t *)c->data;
    int fd = r->connection->fd & g_fd_mask;

    // ngx_http_websocket_ctx_t *ctx = ngx_http_get_module_ctx(r, ngx_http_websocket_module);
    
    if (rev->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, 0, "WebSocket timeout");
        destroy_tgg_cli(fd);
        return;
    }
    
    // 处理输入数据
    ngx_int_t rc = ngx_http_websocket_process_input(r);
    if (rc == NGX_AGAIN) {
        ngx_add_event(rev, NGX_READ_EVENT, 0);
        // clean_client_data(cli_fd, idx);
        return;
    }
    
    if (rc != NGX_OK) {
        // destroy_tgg_cli(fd);
        // ws_connection_pool_remove(ctx->client_id);
        // ngx_http_close_connection(c);
        // clean_client_data(cli_fd, idx);
        return;
    }
    
    ngx_add_event(rev, NGX_READ_EVENT, 0);
}
static ngx_int_t trans_upstream_data(int core_id, int fd, std::string_view data, int fd_opt)
{
    if (enqueue_data_trans(core_id, fd, data, fd_opt) < 0) {// 函数内部会循环尝试发送10次
        LOG_ERROR("Send data to server Failed,[core:%d][fd:%d].",
         core_id, fd);
        return -1;
    }
    // LOG_DEBUG("send to server:%s.", bin2hex(data).c_str());
    return 0;
}

static ngx_int_t deal_close_fram(int core_id, int fd)
{
    std::string result = Websocket::EncodeCloseFrame("");
    send_frame(fd, result.data(), result.length());
    // tgg_set_cli_status(core_id, fd, FD_STATUS_CLOSING);
    // TODO 后需全局健康检查的话，healthcheck
    LOG_INFO("close connection");
    if (enqueue_data_trans(core_id, fd, "", FD_CLOSE) < 0) {// 函数内部会循环尝试发送10次
        LOG_ERROR("Send data to server Failed,[core:%d][fd:%d].",
         core_id, fd);
        return -1;
    }
    return 0;
}

static ngx_int_t deal_ping_fram(int core_id, int fd, const std::string& response)
{
    std::string result = Websocket::EncodeWebsocketMessage(PONG_FRAME, response);
    send_frame(fd, result.data(), result.length());
    // if (enqueue_data_trans(core_id, fd, result, FD_WRITE) < 0) {// 函数内部会循环尝试发送10次
    //     LOG_ERROR("Send data to server Failed,[core:%d][fd:%d].",
    //      core_id, fd);
    //     return -1;
    // }
    return 0;
}

static ngx_int_t deal_pong_fram(int core_id, int fd, const std::string& response)
{
    // 在这里可以获取主动检测结果
    // LOG_DEBUG("recieve pong:%s", response.c_str());
    return 0;
}
// static int consume_rdata(int clt_fd, const char* buf, int len, int idx, enum FD_OPT opt)
// {
//     // g_tgg_stats.en_read_stats.malloc_st++;
//     tgg_read_data rdata = {};
//     rdata.fd = clt_fd;
//     rdata.coreid = g_core_id;
//     rdata.idx = idx;
//     rdata.fd_opt = opt;
//     rdata.data_len = len;
//     rdata.data = (void*)buf;
//     WsConsumer cons;
//     int ret = cons.ConsumerData(&rdata);
//     return ret;
// }
// 处理输入数据（关键修复：添加实现）
static ngx_int_t ngx_http_websocket_process_input(ngx_http_request_t *r)
{
    ngx_connection_t *c = r->connection;
    ngx_http_websocket_ctx_t *ctx = (ngx_http_websocket_ctx_t *)ngx_http_get_module_ctx(r, ngx_http_websocket_module);
    ws_frame_buffer_t *buffer = &ctx->frame_buffer;
    int fd = r->connection->fd & g_fd_mask;
    
    ssize_t n;
    u_char buf[4096];
    
    // 读取数据
    n = ngx_recv(c, buf, sizeof(buf));
    if(n <= 0) {
        if (n == NGX_AGAIN) {
            return NGX_AGAIN;
        } else {
            if(tgg_get_cli_idx(g_core_id, fd) >= 0) {
                LOG_DEBUG("recv close from client.");
                trans_upstream_data(g_core_id, fd, "", FD_CLOSE);
            } else {
                destroy_tgg_cli(fd);
            }
            if(n == 0 /*&& !(tgg_get_cli_status(g_core_id, fd) & FD_STATUS_CLOSING) && tgg_get_cli_idx(g_core_id, fd) != TGG_FD_CLOSING*/) {// 没发送过close给gwcliprc
                LOG_INFO("WebSocket closed by client");
                return NGX_DECLINED;
            } else {
                LOG_ERROR("WebSocket recv error ret:%d", n);
                return NGX_ERROR;
            }
        }
    }
    
    // consume_ret = consume_rdata(r->connection->fd, buf, n, tgg_get_cli_idx(g_core_id, r->connection->fd), FD_READ);
    // if (consume_ret < 0) {
    //     LOG_ERROR("consume data failed.");
    //     return NGX_ERROR;
    // }
    // 处理数据
    u_char *pos = buf;
    size_t len = n;
    while (len > 0) {
        size_t consumed = ws_frame_parse(r, buffer, pos, len);
        if (consumed == 0) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0, "WebSocket frame parse error");
            ws_frame_buffer_reset(r, buffer);
            return NGX_ERROR;
        }
        
        pos += consumed;
        len -= consumed;
        
        // 完整帧处理
        if (buffer->state == WS_FRAME_COMPLETE) {
            switch (buffer->opcode) {
                case TEXT_FRAME:
                case BINARY_FRAME:
                    trans_upstream_data(g_core_id, fd, std::string((char*)buffer->payload, buffer->payload_len), FD_WRITE);
                    break;
                case CLOSING_FRAME:
                    deal_close_fram(g_core_id, fd);
                    // return 1;
                    break;
                case ERROR_FRAME:
                    LOG_ERROR("error frame, fd:%d.", r->connection->fd);
                    deal_close_fram(g_core_id, fd);
                    break;
                case PING_FRAME:
                    deal_ping_fram(g_core_id, fd, std::string((char*)buffer->payload, buffer->payload_len));
                    break;
                case PONG_FRAME:
                    deal_pong_fram(g_core_id, fd, std::string((char*)buffer->payload, buffer->payload_len));
                    break;
                default:
                    LOG_ERROR("unexpected frame type %d, fd:%d.", buffer->opcode, r->connection->fd);
                    break;
            }

            // 创建帧结构
            // websocket_frame_t *frame = ngx_palloc(r->pool, sizeof(websocket_frame_t));
            // frame->client_id = ctx->client_id;
            // frame->opcode = buffer->opcode;
            // frame->fin = buffer->fin;
            // frame->payload.data = buffer->payload;// 使用完需要手动释放
            // frame->payload.len = buffer->payload_len;
            
            // // 放入读队列
            // if (rte_ring_enqueue(ws_read_ring, frame) != 0) {
            //     ngx_log_error(NGX_LOG_ERR, c->log, 0, "WebSocket read ring full");
            // }
            
            // 重置缓冲区
            ws_frame_buffer_reset(r, buffer);
        }
    }
    
    return NGX_OK;
}

// 内容处理函数
static ngx_int_t ngx_http_websocket_content_handler(ngx_http_request_t *r)
{
    if (r->method == NGX_HTTP_GET) {
        return ngx_http_websocket_upgrade(r);
    }
    return NGX_DECLINED;
}

// 指令处理函数
static char *ngx_http_websocket_handler_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t *clcf = (ngx_http_core_loc_conf_t *)ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_websocket_content_handler;
    return NGX_CONF_OK;
}

static void tgg_connection_write_handler(ngx_event_t *ev)
{
    ngx_connection_t *c = (ngx_connection_t*)ev->data;
    // ngx_http_websocket_ctx_t *ctx = ngx_http_get_module_ctx(r, ngx_http_websocket_module);
    // ws_frame_buffer_t *buffer = &ctx->frame_buffer;
    
    // ssize_t n;
    // u_char buf[4096];
    
    // // 读取数据
    // n = ngx_recv(c, buf, sizeof(buf));
    // if (n == NGX_ERROR) {
    //     return NGX_ERROR;
    // }
    int fd = c->fd & g_fd_mask;
    tgg_write_data* cur = tgg_get_cli_blocked_data(g_core_id, fd);
    if(cur) {
        ngx_int_t result = ngx_send(c, (u_char*)cur->data, cur->data_len);
        if (result >= NGX_OK) {
            ((tgg_write_data*)cur)->ref--;
            if(((tgg_write_data*)cur)->ref <= 0) {
                clean_write_data(g_core_id, (tgg_write_data*)(cur));
            }
        } else if (result == NGX_AGAIN) {
            // TODO 设计发送次数限制，防止发不出去一直发，超过次数可以直接关闭
            LOG_DEBUG("Debuging Try again core[%d] fd[%d].", g_core_id, c->fd);
            return ;
        } else {
            LOG_ERROR("Write error, core[%d] fd[%d] closed.", g_core_id, c->fd);
            destroy_tgg_cli(fd);
        }
        if(cur->fd_opt & FD_CLOSE) {
            destroy_tgg_cli(fd);
        }
    }
    tgg_send_data* data = NULL;
    while((data = tgg_pop_cli_snd_data(g_core_id, fd)) != NULL) {
        if(data && data->data) {
            if(((tgg_write_data*)(data->data))->data) {
                if(AsyncLogger::getInstance().getloglevel() == LogLevel::DEBUG) {
                    if(((tgg_write_data*)(data->data))->data_len > 4 && !strncmp((char*)(((tgg_write_data*)(data->data))->data), "HTTP", 4)) {// GET请求消息
                        LOG_DEBUG("fd:%d idx:%d send to clien:%s.", c->fd, ((tgg_write_data*)(data->data))->idx, (char*)(((tgg_write_data*)(data->data))->data));
                    } else {// 其他消息
                        LOG_DEBUG("fd:%d idx:%d send to clien:%s.", c->fd, ((tgg_write_data*)(data->data))->idx, bin2hex(std::string_view((char*)(((tgg_write_data*)(data->data))->data), ((tgg_write_data*)(data->data))->data_len)).c_str());
                    }
                }
                ngx_int_t result = ngx_send(c, (u_char*)((tgg_write_data*)(data->data))->data, ((tgg_write_data*)(data->data))->data_len);
                if (result >= NGX_OK) {
                    LOG_DEBUG("send data success, ret:%d", result);
                    // ((tgg_write_data*)(data->data))->ref--;
                    // if(((tgg_write_data*)(data->data))->ref <= 0) {
                    //     clean_write_data(g_core_id, (tgg_write_data*)(data->data));
                    // }
                } else if (result == NGX_AGAIN) {
                    // TODO 设计发送次数限制，防止发不出去一直发，超过次数可以直接关闭
                    tgg_set_cli_blocked_data(g_core_id, fd, data->data);
                    tgg_free_cli_snd_data(g_core_id, data);
                    LOG_DEBUG("Debuging Try again core[%d] fd[%d].", g_core_id, c->fd);
                    return ;
                } else {
                    destroy_tgg_cli(fd);
                    LOG_ERROR("Write error, core[%d] fd[%d] closed, ret:%d.", g_core_id, c->fd, result);
                    return ;
                }

                ((tgg_write_data*)(data->data))->ref--;
            }
            if(((tgg_write_data*)(data->data))->fd_opt & FD_CLOSE) {
                LOG_WARNING("Deal Close cmd.");
                destroy_tgg_cli(fd);
            }
            if(((tgg_write_data*)(data->data))->ref <= 0) {
                clean_write_data(g_core_id, (tgg_write_data*)(data->data));
            }
            tgg_free_cli_snd_data(g_core_id, data);
        }
    }
    return ;
}

static void tgg_do_send(tgg_write_data* wdata)
{
    tgg_fd_id_list* fd_id_list = wdata->lst_fd;
    while (fd_id_list) {
        int cli_fd = fd_id_list->fdid;// 数据传递时fdid存的是fd
        int idx = tgg_get_cli_idx(g_core_id, cli_fd);
        // if(AsyncLogger::getInstance().getloglevel() == LogLevel::DEBUG) {
        //     if(wdata->data_len > 4 && !strncmp((char*)wdata->data, "HTTP", 4)) {// GET请求消息
        //         LOG_DEBUG("fd:%d idx:%d send to clien:%s.", cli_fd, idx, (char*)wdata->data);
        //     } else {// 其他消息
        //         LOG_DEBUG("fd:%d idx:%d send to clien:%s.", cli_fd, idx, bin2hex(std::string_view((char*)wdata->data, wdata->data_len)).c_str());
        //     }
        // }
        // 只有未关闭的连接才需要走以下逻辑，已经关闭的连接，不再发送数据
        if(idx > 0) {
            // 新的连接旧的数据就不要发送了，直接清理空间
            if (idx != fd_id_list->idx) {// 后台推送给前端时，可能会出现这种情况
                LOG_ERROR("Idx[%d:%d] Changed, Closing Connection[%d].", idx, fd_id_list->idx, cli_fd);
                goto send_client_end;
            }

            // 是否需要发送数据
            if (wdata->fd_opt & (FD_WRITE | FD_CLOSE)) {
                if(tgg_add_cli_snd_data(g_core_id, cli_fd, wdata) < 0) {
                    // ff_close(cli_fd);
                    LOG_ERROR("add cli[fd:%d, idx:%d] snd data failed, no more available unit in mempool", cli_fd, idx);
                    // 队列满时，发送关闭消息，让bwprc先释放，再发送FD_CLOSE过来走正常结束流程
                    trans_upstream_data(g_core_id, cli_fd, "", FD_CLOSE);
                    destroy_tgg_cli(cli_fd);
                    // ngx_http_websocket_ctx_t *ctx = (ngx_http_websocket_ctx_t *)tgg_get_cli_ctx(g_core_id, cli_fd);
                    // if(!ctx) {
                    //     LOG_ERROR("add cli[fd:%d, idx:%d] snd data failed, ctx is null", cli_fd, idx);
                    // }
                    // ctx->connection->read->handler = ngx_http_websocket_handler;
                    // ngx_add_event(ctx->connection->read, NGX_READ_EVENT, 0);
                } else {
                    ngx_http_websocket_ctx_t *ctx = (ngx_http_websocket_ctx_t *)tgg_get_cli_ctx(g_core_id, cli_fd);
                    if(!ctx) {
                        LOG_ERROR("add cli[fd:%d, idx:%d] snd data failed, ctx is null", cli_fd, idx);
                    }
                    ctx->connection->write->handler = tgg_connection_write_handler;
                    ngx_add_event(ctx->connection->write, NGX_WRITE_EVENT, 0);
                }
            }

            // if ( wdata->fd_opt & FD_CLOSE) {
            //     LOG_INFO("Closing Connection[%d].", cli_fd);
            //     // FD_CLOSE 时说明bwprc已经清理完，咱们这边只要清理并关闭fd即可
            //     destroy_tgg_cli(cli_fd);
            //     // tgg_set_cli_idx(g_core_id, cli_fd, TGG_FD_CLOSED);// 先设置标记，后续数据将不再入写队列
            //     // tgg_del_idx(g_core_id, idx);
            //     // ngx_http_websocket_ctx_t *ctx = (ngx_http_websocket_ctx_t *)tgg_get_cli_ctx(g_core_id, cli_fd);
            //     // if(!ctx) {
            //     //     LOG_ERROR("add cli[fd:%d, idx:%d] snd data failed, ctx is null", cli_fd, idx);
            //     // }
            //     // ctx->connection->read->handler = ngx_http_websocket_handler;
            //     // ngx_add_event(ctx->connection->read, NGX_READ_EVENT, 0);
            //  }
        } else {
            // 连接标记已设置为关闭，队列中的数据直接丢弃
            LOG_DEBUG("write to client data droped, cause connection[fd:%d] not published[idx:%d].", cli_fd, idx);
        }

send_client_end:
        // tgg_fd_id_list* tmp = fd_id_list;
        fd_id_list = fd_id_list->next;
        // clean_fdidnode(tmp);
    }
    // 所有fd都发送完了之后，需要清理并回收内存
    if(wdata->ref <= 0) {
        clean_write_data(g_core_id, wdata);
    }else {
        clean_fdidlist(wdata->lst_fd);
        wdata->lst_fd = NULL;// 清理完必须要置空，否则后续clean_write_data时，会重复释放
    }
}


static void ngx_write_queue_timer(ngx_event_t *ev)
{
    if (!g_run_status) {
        LOG_WARNING("stop running...");
        // ngx_http_websocket_exit_module();
        return;
    }
    
    unsigned int BATCH_SIZE = 128; // 1ms 128个写入，1s就是12.8万个请求
    tgg_write_data *batch_data[BATCH_SIZE];
    int processed = 0;
    ngx_msec_t start_time = ngx_current_msec;
    
    // 批量从全局队列中取出数据
    unsigned int available = 0;
    processed = tgg_batch_dequeue_write(g_core_id, batch_data, BATCH_SIZE, &available);
    if (processed == 0) {
        // 队列为空，延长检查间隔
        ngx_msec_t next_timeout = (available > 0) ? 1 : 2; // 根据队列状态动态调整
        ev->timer.key = ngx_current_msec + next_timeout;
        ngx_add_timer(ev, next_timeout);
        return;
    }/* else {
        LOG_DEBUG("dequeue count : %d, available %d", processed, available);
    }*/
    
    // int success_count = 0;
    // int error_count = 0;
    
    // 分发数据到对应连接的发送队列
    for (int i = 0; i < processed; i++) {
        tgg_write_data *wdata = batch_data[i];
        if (!wdata) continue;
        // 添加到连接的发送队列
        tgg_do_send(wdata);
    }
    
    // 动态调整下次执行时间
    ngx_msec_t processing_time = ngx_current_msec - start_time;
    ngx_msec_t next_timeout;
    
    if (processed >= (int)BATCH_SIZE) {
        // 队列很满，立即再次处理
        next_timeout = 0;
    } else if (processing_time > 5) {
        // 处理时间较长，适当延长间隔
        next_timeout = 2;
    } else {
        next_timeout = 1;
    }
    
    ev->timer.key = ngx_current_msec + next_timeout;
    ngx_add_timer(ev, next_timeout);
    
    // 记录统计信息
    // if (processed > 0) {
    //     ngx_log_debug4(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
    //                   "Write queue timer: processed=%d, success=%d, error=%d, next_timeout=%M",
    //                   processed, success_count, error_count, next_timeout);
    // }
}
// 模块定义
static ngx_command_t ngx_http_websocket_commands[] = {
    {
        ngx_string("websocket_handler"),
        NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS,
        ngx_http_websocket_handler_directive,
        0,
        0,
        NULL
    },
    ngx_null_command
};

static ngx_http_module_t ngx_http_websocket_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */
    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */
    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */
    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};

static void tgg_recv_clean_prev()
{
    for (int i = 0; i < (int)g_fd_limit; ++i)
    {// 防止secondary进程异常重启后，上一次的缓存没有清理
        int idx = tgg_get_cli_idx(g_core_id, i);
        if( idx > 0 && tgg_check_idx_exist(g_core_id, idx)) {
            // 通知gwbwprc 清理这个链接对应的缓存
            LOG_ERROR("clean prev data coreid[%d] fd[%d] idx[%d].", g_core_id, i, idx);
            trans_upstream_data(g_core_id, i, "", FD_CLOSE);
            tgg_del_idx(g_core_id, idx);
            clean_client_data(i);
        }
    }
    tgg_iter_del_idx(g_core_id);
}

extern int ngx_argc;
extern char **ngx_argv;

void tgg_sig_init()
{
    if (signal(SIGINT, signal_handler) == SIG_ERR) {
        LOG_ERROR("Error setting signal handler");
        prc_exit(-1, "Error setting signal handler");
    }
    if (signal(SIGTERM, signal_handler) == SIG_ERR) {
        LOG_ERROR("Error setting signal handler");
        prc_exit(-1, "Error setting signal handler");
    }

    if (signal(SIGCHLD, sigchld_handler) == SIG_ERR) {
        LOG_ERROR("Error setting signal handler");
        exit(-1);
    }
}
static ngx_int_t ngx_http_websocket_init_module()
{
    init_core(s_dump_file);
    if (tgg_init_config(ngx_argc, ngx_argv) < 0) {
        LOG_ERROR("init config error.");
        return -1;
    }
    if (AsyncLogger::getInstance().init(TggConfigure::getInstance()->get_log_path(), 
        TggConfigure::getInstance()->get_gateway_log_level()) < 0) {
        LOG_ERROR("init log error.");
        return -1;
    }

    tgg_sig_init();// 信号处理初始化
    initOpenSSL();// 初始化ssl加解密环境

    // if (!mt_init_frame(ngx_argc, ngx_argv)) {
    //     LOG_ERROR("mt frame init failed.");
    //     return -1;
    // }
    if(rte_eal_process_type() != RTE_PROC_PRIMARY) {
        if(wait_primary_up() < 0) {
            LOG_INFO("-------secondary core[%d] exit, wait primary up overtime-------", g_core_id);
            return -1;
        }
    }
    if (!init_ip_filter(rte_eal_process_type() == RTE_PROC_PRIMARY, TggConfigure::getInstance()->get_ip_filter_path().c_str())) {
        LOG_WARNING("init ip filter failed.");
    }
    g_core_id = ff_get_proc_id();//rte_lcore_to_cpu_id(rte_lcore_id());
    if(rte_eal_process_type() == RTE_PROC_PRIMARY) {
        if(pipe2(sig_pipe, O_NONBLOCK | O_CLOEXEC) < 0){
            LOG_ERROR("-------master[pid:%d] core[%d] pipe failed.-------", getpid(), g_core_id);
        }
        LOG_INFO("-------master[pid:%d] core[%d] start-------", getpid(), g_core_id);
        tgg_master_init();
        if(TggConfigure::getInstance()->get_auto_start()) {
            g_monitor_count = count_ones(TggConfigure::getInstance()->get_lcore_mask()) + 2;// +2 是gwcliprc和register
            g_pid_check_times = new int[g_monitor_count]{0};
            check_gw_monitor(NULL, NULL);
            // 检查子进程是否已全部启动
            int check_times = 1500;// 最多等待15s
            while (g_run_status && check_times > 0) {
                if(check_if_all_child_up()) {
                    break;
                }
                check_times--;
                usleep(10000);
            }
            if(!check_if_all_child_up()) {
                kill_all_child();
                g_run_status = 0;
                LOG_FATAL("not all child process is working on the beginning, exiting...");
            }
            // mt_sleep(2000);// 等待所有进程启动完成
        }
    } else {
        LOG_INFO("-------secondary[pid:%d] core[%d] start-------", getpid(), g_core_id);
        tgg_gwrcv_secondary_init();
        if(TggConfigure::getInstance()->get_auto_start()) {
            if (tgg_check_gw_monitor_up(g_core_id)) {// 上一个进程尚未结束
                LOG_INFO("-------secondary core[%d] exit, prev coreid still running-------", g_core_id);
                // mt_uninit_frame();
                ff_stop_run();
                ff_release();
                rte_eal_cleanup();
                AsyncLogger::getInstance().shutdown();
                return 0;
            }
        }
        tgg_recv_clean_prev();
    }
    // 启动定时器
    init_timer();
    return 0;
}

static ngx_int_t ngx_http_websocket_init_master(ngx_log_t *log)
{

    return ngx_http_websocket_init_module();
}

static ngx_int_t ngx_http_websocket_init_process(ngx_cycle_t *cycle)
{

    if(ngx_http_websocket_init_module() < 0) {
        return NGX_ERROR;
    }
    ngx_event_t *ev = (ngx_event_t *)ngx_pcalloc(cycle->pool, sizeof(ngx_event_t));
    ev->handler = ngx_write_queue_timer;
    ev->log = cycle->log;
    ev->data = cycle;
    
    ngx_add_timer(ev, 100);
    return NGX_OK;
}

static void ngx_http_websocket_exit_module(ngx_cycle_t *cycle)
{
    // 停止定时器
    stop_timer();
    print_mem_statistics();
    if(rte_eal_process_type() == RTE_PROC_PRIMARY) {
        if(TggConfigure::getInstance()->get_auto_start()) {
            kill_all_child();
            wait_all_child_exit();
            delete[] g_pid_check_times;
        }
        tgg_master_uninit();
        cleanup_ip_filter();
        LOG_INFO("-------master core[%d] exit-------", g_core_id);
    } else {
        LOG_INFO("-------secondary core[%d] exit-------", g_core_id);
    }
    // mt_uninit_frame();
    // ff_stop_run();
    ff_release();
    // rte_eal_cleanup();
    LOG_WARNING("gwrcv left fd count:%ld", s_left_fd);
    AsyncLogger::getInstance().shutdown();
    // return NGX_OK;
}

// static void ngx_http_websocket_exit_master(ngx_cycle_t *cycle)
// {
//     ngx_http_websocket_exit_module(cycle);
// }

static void ngx_http_websocket_exit_process(ngx_cycle_t *cycle)
{
    ngx_http_websocket_exit_module(cycle);
}

ngx_module_t ngx_http_websocket_module = {
    NGX_MODULE_V1,
    &ngx_http_websocket_module_ctx,        /* module context */
    ngx_http_websocket_commands,           /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    ngx_http_websocket_init_master,                                  /* init master */
    NULL,                                  /* init module */
    ngx_http_websocket_init_process,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    ngx_http_websocket_exit_process,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


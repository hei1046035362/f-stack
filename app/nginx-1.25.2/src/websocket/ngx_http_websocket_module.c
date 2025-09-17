#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <rte_ring.h>
#include "websocket_frame.h"
#include "connection_pool.h"
#include "ring_handler.h"
// 模块上下文引用
extern ngx_module_t ngx_http_websocket_module;

// 全局队列
extern struct rte_ring *ws_read_ring;
extern struct rte_ring *ws_write_ring;

// WebSocket 连接上下文
typedef struct {
    ngx_http_request_t *request;
    ngx_connection_t *connection;
    ngx_str_t client_id;
    ws_frame_buffer_t frame_buffer; // 帧缓冲区
} ngx_http_websocket_ctx_t;

// 前置声明
static void ngx_http_websocket_handler(ngx_event_t *rev);
static ngx_int_t ngx_http_websocket_process_input(ngx_http_request_t *r);

// 查找特定请求头
static ngx_table_elt_t *find_header(ngx_http_request_t *r, const char *name, size_t len)
{
    ngx_list_part_t *part;
    ngx_table_elt_t *h;
    ngx_uint_t i;
    
    part = &r->headers_in.headers.part;
    h = part->elts;
    
    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            
            part = part->next;
            h = part->elts;
            i = 0;
        }
        
        if (h[i].key.len == len && ngx_strncasecmp(h[i].key.data, (u_char*)name, len) == 0) {
            return &h[i];
        }
    }
    
    return NULL;
}

static ngx_int_t
ngx_http_websocket_upgrade(ngx_http_request_t *r)
{
    ngx_table_elt_t *upgrade, *sec_key;

    // 必须是 GET
    if (!(r->method & NGX_HTTP_GET)) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    upgrade = find_header(r, "Upgrade", 7);
    if (upgrade == NULL || ngx_strncasecmp(upgrade->value.data, (u_char*)"websocket", 9) != 0) {
        return NGX_DECLINED;
    }

    sec_key = find_header(r, "Sec-WebSocket-Key", 17);
    if (sec_key == NULL || sec_key->value.len != 24) {  // Base64 encoded 16-byte nonce
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
    ngx_table_elt_t *h = ngx_list_push(&r->headers_out.headers);
    if (h == NULL) goto failed;
    h->hash = 1;
    ngx_str_set(&h->key, "Upgrade");
    ngx_str_set(&h->value, "websocket");

    // --- 添加 Connection: Upgrade ---
    h = ngx_list_push(&r->headers_out.headers);
    if (h == NULL) goto failed;
    h->hash = 1;
    ngx_str_set(&h->key, "Connection");
    ngx_str_set(&h->value, "Upgrade");

    // --- 添加 Sec-WebSocket-Accept ---
    u_char accept_key[29];
    ngx_http_websocket_calc_accept(sec_key->value.data, sec_key->value.len, accept_key);

    h = ngx_list_push(&r->headers_out.headers);
    if (h == NULL) goto failed;
    h->hash = 1;
    ngx_str_set(&h->key, "Sec-WebSocket-Accept");
    h->value.data = accept_key;
    h->value.len = 28;

    // === 发送 header ===
    ngx_int_t rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK) {
        return rc;
    }

    // === 创建上下文 ===
    ngx_http_websocket_ctx_t *ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_websocket_ctx_t));
    if (ctx == NULL) goto failed;

    ctx->request = r;
    ctx->connection = r->connection;
    ctx->client_id.len = 32;
    ctx->client_id.data = ngx_pnalloc(r->pool, 32);
    if (ctx->client_id.data == NULL) goto failed;

    ngx_snprintf(ctx->client_id.data, 32, "%V-%T", &r->connection->addr_text, ngx_time());

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
    ws_connection_pool_add(ctx->client_id, r->connection);

    // 返回 DONE，表示协议升级完成
    return NGX_DONE;

failed:
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
}
// WebSocket 事件处理
static void ngx_http_websocket_handler(ngx_event_t *rev)
{
    ngx_connection_t *c = rev->data;
    ngx_http_request_t *r = c->data;
    ngx_http_websocket_ctx_t *ctx = ngx_http_get_module_ctx(r, ngx_http_websocket_module);
    
    if (rev->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, 0, "WebSocket timeout");
        ws_connection_pool_remove(ctx->client_id);
        ngx_http_close_connection(c);
        return;
    }
    
    // 处理输入数据
    ngx_int_t rc = ngx_http_websocket_process_input(r);
    if (rc == NGX_AGAIN) {
        ngx_add_event(rev, NGX_READ_EVENT, 0);
        return;
    }
    
    if (rc != NGX_OK) {
        ws_connection_pool_remove(ctx->client_id);
        ngx_http_close_connection(c);
        return;
    }
    
    ngx_add_event(rev, NGX_READ_EVENT, 0);
}

// 处理输入数据（关键修复：添加实现）
static ngx_int_t ngx_http_websocket_process_input(ngx_http_request_t *r)
{
    ngx_connection_t *c = r->connection;
    ngx_http_websocket_ctx_t *ctx = ngx_http_get_module_ctx(r, ngx_http_websocket_module);
    ws_frame_buffer_t *buffer = &ctx->frame_buffer;
    
    ssize_t n;
    u_char buf[4096];
    
    // 读取数据
    n = ngx_recv(c, buf, sizeof(buf));
    if (n == NGX_ERROR) {
        return NGX_ERROR;
    }
    
    if (n == 0) {
        ngx_log_error(NGX_LOG_INFO, c->log, 0, "WebSocket closed by client");
        return NGX_ERROR;
    }
    
    if (n == NGX_AGAIN) {
        return NGX_AGAIN;
    }
    
    // 处理数据
    u_char *pos = buf;
    size_t len = n;
    
    while (len > 0) {
        size_t consumed = ws_frame_parse(r, buffer, pos, len);
        if (consumed == 0) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0, "WebSocket frame parse error");
            return NGX_ERROR;
        }
        
        pos += consumed;
        len -= consumed;
        
        // 完整帧处理
        if (buffer->state == WS_FRAME_COMPLETE) {
            // 创建帧结构
            websocket_frame_t *frame = ngx_palloc(r->pool, sizeof(websocket_frame_t));
            frame->client_id = ctx->client_id;
            frame->opcode = buffer->opcode;
            frame->fin = buffer->fin;
            frame->payload.data = buffer->payload;// 使用完需要手动释放
            frame->payload.len = buffer->payload_len;
            
            // 放入读队列
            if (rte_ring_enqueue(ws_read_ring, frame) != 0) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0, "WebSocket read ring full");
            }
            
            // 重置缓冲区
            ws_frame_buffer_reset(buffer);
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
    ngx_http_core_loc_conf_t *clcf;
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_websocket_content_handler;
    return NGX_CONF_OK;
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
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <rte_ring.h>
#include "websocket_frame.h"
#include "connection_pool.h"
#include "ring_handler.h"
extern ngx_module_t ngx_http_websocket_module;
// 全局队列
struct rte_ring *ws_read_ring = NULL;
struct rte_ring *ws_write_ring = NULL;
static ngx_event_t ring_event;
// 初始化队列
ngx_int_t init_websocket_rings(ngx_log_t *log)
{
    if(rte_eal_process_type() != RTE_PROC_PRIMARY) {
        ws_read_ring = rte_ring_lookup("ws_read_queue");
        ws_write_ring = rte_ring_lookup("ws_write_queue");
        return NGX_OK;
    }

    // 创建读队列
    ws_read_ring = rte_ring_create("ws_read_queue", 1024 * 1024, rte_socket_id(), 
                                  RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (ws_read_ring == NULL) {
        ngx_log_error(NGX_LOG_EMERG, log, 0, "Failed to create WebSocket read ring");
        return NGX_ERROR;
    }
    
    // 创建写队列
    ws_write_ring = rte_ring_create("ws_write_queue", 1024 * 1024, rte_socket_id(), 
                                   RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (ws_write_ring == NULL) {
        ngx_log_error(NGX_LOG_EMERG, log, 0, "Failed to create WebSocket write ring");
        return NGX_ERROR;
    }
    
    return NGX_OK;
}

// 处理写队列中的WebSocket帧
static void process_websocket_write_queue()
{
    websocket_frame_t *frame = NULL;;
    int processed = 0;
    
    // 批量处理最多32个帧
    while (processed < 32 && rte_ring_dequeue(ws_write_ring, (void**)&frame) == 0) {
        // 查找连接
        ngx_connection_t *c = ws_connection_pool_find(frame->client_id);
        if (c == NULL) {
            ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, 
                         "Connection not found for client: %V", frame->client_id);
            continue;
        }
        
        // 发送WebSocket帧
        ngx_int_t rc = ngx_http_websocket_send_frame(c, frame->opcode, frame->fin, frame->payload);
        if (rc != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0, "Failed to send WebSocket frame");
        }
        
        processed++;
    }
}

// 定时事件处理函数
static void websocket_ring_handler(ngx_event_t *ev)
{
    // 处理写队列
    process_websocket_write_queue();
    
    // 重新添加定时器
    ngx_event_add_timer(ev, 10);
}

// 初始化定时事件
ngx_int_t init_websocket_ring_handler(ngx_cycle_t *cycle)
{
    ngx_event_timer_init(cycle->log);

    ngx_memzero(&ring_event, sizeof(ngx_event_t));
    ring_event.handler = websocket_ring_handler;
    ring_event.log = cycle->log;
    ring_event.data = NULL;
    // 添加到定时器
    ngx_event_add_timer(&ring_event, 10);
    
    return NGX_OK;
}
ngx_int_t ngx_http_websocket_init_master(ngx_log_t *log) {

    // 初始化 rte_ring 队列
    if (init_websocket_rings(log) != NGX_OK) {
        return NGX_ERROR;
    }
    
    // 初始化连接池
    if (ws_connection_pool_init(log) != NGX_OK) {
        return NGX_ERROR;
    }
    return NGX_OK;
}

// 模块初始化
ngx_int_t ngx_http_websocket_init_process(ngx_cycle_t *cycle)
{
    // 初始化 rte_ring 队列
    if (init_websocket_rings(cycle->log) != NGX_OK) {
        return NGX_ERROR;
    }
    
    // 初始化连接池
    if (ws_connection_pool_init(cycle->log) != NGX_OK) {
        return NGX_ERROR;
    }
    
    // 初始化队列处理定时器
    if (init_websocket_ring_handler(cycle) != NGX_OK) {
        return NGX_ERROR;
    }
    
    return NGX_OK;
}
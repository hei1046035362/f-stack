#ifndef NGX_HTTP_WEBSOCKET_FRAME_H
#define NGX_HTTP_WEBSOCKET_FRAME_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

// WebSocket 操作码
#define WS_OP_CONTINUE  0x0
#define WS_OP_TEXT      0x1
#define WS_OP_BINARY    0x2
#define WS_OP_CLOSE     0x8
#define WS_OP_PING      0x9
#define WS_OP_PONG      0xA

// 帧解析状态
typedef enum {
    WS_FRAME_HEADER,
    WS_FRAME_LENGTH_16,
    WS_FRAME_LENGTH_64,
    WS_FRAME_MASK,
    WS_FRAME_PAYLOAD,
    WS_FRAME_COMPLETE,
    WS_FRAME_ERROR
} ws_frame_state_t;

// 帧缓冲区
typedef struct {
    ws_frame_state_t state;
    uint8_t fin;
    uint8_t opcode;
    uint8_t mask;
    uint8_t mask_key[4];
    uint64_t payload_len;
    u_char *payload;
    size_t payload_received;
    u_char header[14]; // 最大头部长度
    size_t header_len;
    size_t mask_offset;
} ws_frame_buffer_t;

// WebSocket 帧结构
typedef struct {
    ngx_str_t client_id;
    uint8_t opcode;
    uint8_t fin;
    ngx_str_t payload;
} websocket_frame_t;

// 函数声明
void ngx_http_websocket_calc_accept(u_char *key, size_t key_len, u_char *accept);
size_t ws_frame_parse(ngx_http_request_t* r, ws_frame_buffer_t *buffer, u_char *data, size_t len);
ngx_int_t ngx_http_websocket_send_frame(ngx_connection_t *c, uint8_t opcode, uint8_t fin, ngx_str_t payload);
void ws_frame_buffer_init(ws_frame_buffer_t *buffer);
void ws_frame_buffer_reset(ws_frame_buffer_t *buffer);

#endif // NGX_HTTP_WEBSOCKET_FRAME_H
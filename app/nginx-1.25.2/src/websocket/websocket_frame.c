#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_sha1.h>
#include "websocket_frame.h"

// 帧缓冲区初始化
void ws_frame_buffer_init(ws_frame_buffer_t *buffer)
{
    ngx_memzero(buffer, sizeof(ws_frame_buffer_t));
    buffer->state = WS_FRAME_HEADER;
    buffer->payload = NULL;
}

// 重置帧缓冲区
void ws_frame_buffer_reset(ngx_http_request_t* r, ws_frame_buffer_t *buffer)
{
    buffer->payload_len = 0;
    buffer->header_len = 0;
    buffer->mask_offset = 0;
    buffer->payload_received = 0;
    buffer->state = WS_FRAME_HEADER;
    buffer->opcode = 0;
    buffer->fin = 0;
    if(buffer->payload) {
        ngx_pfree(r->pool, buffer->payload);
        buffer->payload = NULL;
    }
}

// 计算 WebSocket Accept 密钥
void ngx_http_websocket_calc_accept(u_char *key, size_t key_len, u_char *accept)
{
    static u_char magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    u_char combined[60];
    ngx_sha1_t sha;
    u_char digest[20];
    
    // 拼接 key + magic
    ngx_memcpy(combined, key, key_len);
    ngx_memcpy(combined + key_len, magic, sizeof(magic) - 1);
    
    // 计算 SHA1
    ngx_sha1_init(&sha);
    ngx_sha1_update(&sha, combined, key_len + sizeof(magic) - 1);
    ngx_sha1_final(digest, &sha);
    
    // Base64 编码
    ngx_str_t b64;
    b64.data = accept;
    b64.len = 29;
    ngx_encode_base64(&b64, &(ngx_str_t){20, digest});
}

// WebSocket 帧解析
size_t ws_frame_parse(ngx_http_request_t* r, ws_frame_buffer_t *buffer, u_char *data, size_t len)
{
    size_t consumed = 0;
    
    while (len > 0) {
        switch (buffer->state) {
            case WS_FRAME_HEADER:
                if (buffer->header_len < 2) {
                    buffer->header[buffer->header_len++] = data[0];
                    consumed++;
                    data++;
                    len--;

                    if (buffer->header_len == 2) {
                        buffer->fin = (buffer->header[0] >> 7) & 1;
                        buffer->opcode = buffer->header[0] & 0x0F;

                        buffer->mask = (buffer->header[1] >> 7) & 1;
                        buffer->payload_len = buffer->header[1] & 0x7F;

                        // 必须检查扩展长度，并转移到对应状态
                        if (buffer->payload_len == 126) {
                            buffer->state = WS_FRAME_LENGTH_16;
                            buffer->header_len = 0;  // 重用 header 存放 length 字段
                        } else if (buffer->payload_len == 127) {
                            buffer->state = WS_FRAME_LENGTH_64;
                            buffer->header_len = 0;
                        } else {
                            if (!buffer->mask) {
                                buffer->state = WS_FRAME_ERROR;
                                return 0;
                            }
                            buffer->state = WS_FRAME_MASK;
                        }
                    }
                }
                break;
                
            case WS_FRAME_LENGTH_16:
                while (len > 0 && buffer->header_len < 2) {
                    buffer->header[buffer->header_len++] = *data++;
                    len--;
                    consumed++;
                }

                if (buffer->header_len == 2) {
                    buffer->payload_len = (buffer->header[0] << 8) | buffer->header[1];

                    if (!buffer->mask) {
                        buffer->state = WS_FRAME_ERROR;
                        return 0;
                    }
                    buffer->state = WS_FRAME_MASK;
                    buffer->mask_offset = 0;
                }
                break;                
            case WS_FRAME_LENGTH_64:
                while (len > 0 && buffer->header_len < 8) {
                    buffer->header[buffer->header_len++] = *data++;
                    len--;
                    consumed++;
                }

                if (buffer->header_len == 8) {
                    if ((buffer->header[0] | buffer->header[1]) != 0) {
                        buffer->state = WS_FRAME_ERROR;
                        return 0;
                    }           
                    buffer->payload_len =
                    ((uint64_t)buffer->header[0] << 56) |
                    ((uint64_t)buffer->header[1] << 48) |
                    ((uint64_t)buffer->header[2] << 40) |
                    ((uint64_t)buffer->header[3] << 32) |
                    ((uint64_t)buffer->header[4] << 24) |
                    ((uint64_t)buffer->header[5] << 16) |
                    ((uint64_t)buffer->header[6] << 8) |
                    buffer->header[7];
    
                    if (!buffer->mask) {
                        buffer->state = WS_FRAME_ERROR;
                        return 0;
                    }
                    buffer->state = WS_FRAME_MASK;
                    buffer->mask_offset = 0;
                }
                break;
                
            case WS_FRAME_MASK:
                while (len > 0 && buffer->mask_offset < 4) {
                    buffer->mask_key[buffer->mask_offset++] = *data++;
                    len--;
                    consumed++;
                }

                if (buffer->mask_offset == 4) {
                    buffer->state = WS_FRAME_PAYLOAD;
                    buffer->payload_received = 0;

                    // 分配 payload 缓冲区
                    if (buffer->payload_len > 0) {
                        buffer->payload = ngx_palloc(r->pool, buffer->payload_len);  // 注意：要用 request pool
                        if (buffer->payload == NULL) {
                            buffer->state = WS_FRAME_ERROR;
                            return 0;
                        }
                    }
                }
                break;
                
            case WS_FRAME_PAYLOAD:
                if (buffer->payload_received >= buffer->payload_len) {
                    buffer->state = WS_FRAME_COMPLETE;
                    return consumed;
                }

                size_t need = buffer->payload_len - buffer->payload_received;
                if (need == 0) {
                    buffer->state = WS_FRAME_COMPLETE;
                    return consumed;
                }

                size_t take = (len < need) ? len : need;

                if (buffer->payload) {
                    ngx_memcpy(buffer->payload + buffer->payload_received, data, take);
                }

                for (size_t i = 0; i < take; i++) {
                    ((u_char*)(buffer->payload + buffer->payload_received))[i] ^= 
                    buffer->mask_key[(buffer->payload_received + i) % 4];
                }

                buffer->payload_received += take;
                consumed += take;
                data += take;
                len -= take;

                if (buffer->payload_received == buffer->payload_len) {
                    buffer->state = WS_FRAME_COMPLETE;
                }
                break;
                
            case WS_FRAME_COMPLETE:
                return consumed;
                
            case WS_FRAME_ERROR:
                return 0;
        }
    }
    
    return consumed;
}

// 发送 WebSocket 帧
ngx_int_t ngx_http_websocket_send_frame(ngx_connection_t *c, uint8_t opcode, uint8_t fin, ngx_str_t payload)
{
    u_char header[14]; // 最大头部长度
    u_char *p = header;
    size_t header_len;
    
    // 构建帧头
    *p = (fin << 7) | opcode;
    p++;
    
    // 有效载荷长度
    if (payload.len <= 125) {
        *p = payload.len;
        p++;
        header_len = 2;
    } else if (payload.len <= 65535) {
        *p = 126;
        p++;
        *p = (payload.len >> 8) & 0xFF;
        p++;
        *p = payload.len & 0xFF;
        p++;
        header_len = 4;
    } else {
        *p = 127;
        p++;
        *p = (payload.len >> 56) & 0xFF;
        p++;
        *p = (payload.len >> 48) & 0xFF;
        p++;
        *p = (payload.len >> 40) & 0xFF;
        p++;
        *p = (payload.len >> 32) & 0xFF;
        p++;
        *p = (payload.len >> 24) & 0xFF;
        p++;
        *p = (payload.len >> 16) & 0xFF;
        p++;
        *p = (payload.len >> 8) & 0xFF;
        p++;
        *p = payload.len & 0xFF;
        p++;
        header_len = 10;
    }

    
    struct iovec iov[2];
    int iovcnt = 0;
    
    // 帧头
    iov[iovcnt].iov_base = header;
    iov[iovcnt].iov_len = header_len;
    iovcnt++;
    
    // 有效载荷
    if (payload.len > 0) {
        iov[iovcnt].iov_base = payload.data;
        iov[iovcnt].iov_len = payload.len;
        iovcnt++;
    }
    ssize_t n;
    size_t sent = 0;
    int attempts = 0;
    size_t total_len = header_len + payload.len;

    ngx_iovec_t ngx_iovc;
    ngx_iovc.iovs = iov;
    ngx_iovc.count = iovcnt;
    ngx_iovc.size = total_len;
    ngx_iovc.nalloc = 2;

    // 处理部分写
    while (sent < total_len && attempts < 5) {
        n = ngx_writev(c, &ngx_iovc);
        
        if (n == NGX_ERROR) {
            return NGX_ERROR;
        }
        
        if (n == NGX_AGAIN) {
            usleep(10);
            continue;
            // // 等待可写事件
            // ngx_add_event(c->write, NGX_WRITE_EVENT, 0);
            // return NGX_AGAIN;
        }
        
        // 更新已发送字节
        sent += n;        
        attempts++;
    }
    
    return sent == total_len ? NGX_OK : NGX_ERROR;
}
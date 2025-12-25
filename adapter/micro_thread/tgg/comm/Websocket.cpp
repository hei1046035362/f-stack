#include <iostream>
#include <string>
#include <sstream>
#include <map>
#include <algorithm>
#include <unordered_map>
#include <array>
#include <string_view>
#include <openssl/sha.h>
#include <unistd.h>
#include <ctime>
#include "Encrypt.hpp" // 需要使用 Base64 库
#include "common.hpp"
#include "tgg_comm/tgg_common.h"
#include "Websocket.hpp"
#include "log.hpp"
#include <version>
static const size_t WS_MAX_RECV_FRAME_SZ = 10485760;

#include "picohttpparser.h"

// WebSocket 握手解析结果

// 解析 WebSocket 握手请求
int parse_websocket_handshake(const char* data, size_t len,
                             ws_handshake_t* handshake) {
    memset(handshake, 0, sizeof(*handshake));
    
    // 解析 HTTP 请求
    const char *method, *path;
    size_t method_len, path_len;
    int minor_version;
    struct phr_header headers[20];
    size_t num_headers = 20;
    
    int ret = phr_parse_request(data, len,
                               &method, &method_len,
                               &path, &path_len,
                               &minor_version,
                               headers, &num_headers, 0);
    
    if (ret <= 0) {
        LOG_ERROR("parse request failed,data:%s, len:%d, num_headers:%d.", data, len, num_headers);
        return 0;
    }
    
    // 检查是否是 GET 请求
    if (method_len != 3 || strncmp(method, "GET", 3) != 0) {
        LOG_ERROR("parse request failed,invalid method:%s.", method);
        return 0;
    }
    
    int has_upgrade = 0;
    int has_connection = 0;
    
    // 检查头部
    for (size_t i = 0; i < num_headers; i++) {
        struct phr_header* h = &headers[i];
        
        // Upgrade: websocket
        if (strncasecmp("upgrade", h->name, h->name_len) == 0 &&
            strncasecmp("websocket", h->value, h->value_len) == 0) {
            has_upgrade = 1;
        }
        
        // Connection: Upgrade
        else if (strncasecmp("connection", h->name, h->name_len) == 0) {
            const char* val = h->value;
            size_t val_len = h->value_len;
            
            // 检查是否包含 "Upgrade"（不区分大小写）
            for (size_t j = 0; j + 7 <= val_len; j++) {
                if (strncasecmp(val + j, "upgrade", 7) == 0) {
                    has_connection = 1;
                    break;
                }
            }
        }
        
        // Sec-WebSocket-Key
        else if (strncasecmp("sec-websocket-key", h->name, h->name_len) == 0) {
            handshake->ws_key = h->value;
            handshake->ws_key_len = h->value_len;
        }
        
        // Sec-WebSocket-Version
        else if (strncasecmp("sec-websocket-version", h->name, h->name_len) == 0) {
            handshake->ws_version = h->value;
            handshake->ws_version_len = h->value_len;
        }
        
        // Host
        else if (strncasecmp("host", h->name, h->name_len) == 0) {
            handshake->host = h->value;
            handshake->host_len = h->value_len;
        }
        
        // Origin
        else if (strncasecmp("origin", h->name, h->name_len) == 0) {
            handshake->origin = h->value;
            handshake->origin_len = h->value_len;
        }
        
        // Cookie 解析后移到bwprc
        // else if (strncasecmp("cookie", h->name, h->name_len) == 0) {
        //     handshake->num_cookies = parse_cookies(h->value, h->value_len,
        //                                           handshake->cookies, 10);
        // }
    }
    
    // 验证握手条件
    if (has_upgrade && has_connection && 
        handshake->ws_key && handshake->ws_key_len > 0 &&
        handshake->ws_version && handshake->ws_version_len > 0) {
        handshake->is_valid_handshake = 1;
        return 1;
    }
    
    return 0;
}

static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static const size_t WS_GUID_LEN = 36;  // 不包括结尾的\0
__thread unsigned char tl_sha1_buffer[SHA_DIGEST_LENGTH];
#define TL_BASE64_BUFFER_LEN 29
__thread char tl_base64_buffer[TL_BASE64_BUFFER_LEN];  // 20字节SHA1的Base64编码长度是28字节+1结束符

int Websocket::_GenerateAcceptKey(const char* client_key, size_t key_len,
                                            char* accept_key, size_t& accept_key_capacity) {
    if (!client_key || key_len == 0 || !accept_key || accept_key_capacity < TL_BASE64_BUFFER_LEN) {
        return -1;
    }
    
    // 1. 使用EVP接口，避免拼接内存分配
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (!mdctx) return -2;
    
    if (EVP_DigestInit_ex(mdctx, EVP_sha1(), NULL) != 1) {
        EVP_MD_CTX_free(mdctx);
        return -3;
    }
    
    // 更新数据
    if (EVP_DigestUpdate(mdctx, client_key, key_len) != 1 ||
        EVP_DigestUpdate(mdctx, WS_GUID, WS_GUID_LEN) != 1) {
        EVP_MD_CTX_free(mdctx);
        return -4;
    }
    
    unsigned int sha1_len = 0;
    if (EVP_DigestFinal_ex(mdctx, tl_sha1_buffer, &sha1_len) != 1) {
        EVP_MD_CTX_free(mdctx);
        return -5;
    }
    
    EVP_MD_CTX_free(mdctx);
    
    // 2. Base64编码
    int encoded_len = EVP_EncodeBlock((unsigned char*)accept_key, 
                                      tl_sha1_buffer, SHA_DIGEST_LENGTH);
    
    if (encoded_len != TL_BASE64_BUFFER_LEN-1) {  // SHA1(20字节)的Base64编码应该是28字节
        return -6;
    }
    
    // 复制结果到输出缓冲区
    // memcpy(accept_key, tl_base64_buffer, TL_BASE64_BUFFER_LEN-1);
    // accept_key[TL_BASE64_BUFFER_LEN-1] = '\0';
    
    return 0;
}

int Websocket::_HandleHandshake(std::string_view request, ws_handshake_t& req, std::string& response)
{
    int ret = parse_websocket_handshake(request.data(), request.length(), &req);
    if (!ret) {
        if(!_ElbHealthCheck(request, response)) {
            return -1;
        }
        LOG_DEBUG("Invalid WebSocket handshake, fd:%d", this->fd);
        response = "HTTP/1.1 400 Bad Request\r\n\r\nInvalid WebSocket handshake headers";
        return -1;
    }
    char accept_key[TL_BASE64_BUFFER_LEN] = {0};
    size_t accept_key_len = TL_BASE64_BUFFER_LEN;
    ret = _GenerateAcceptKey(req.ws_key, req.ws_key_len, accept_key, accept_key_len);
    if(ret < 0) {
        LOG_ERROR("generate accept_key failed, ret:%d", ret);
        response = "HTTP/1.1 400 Bad Request\r\n\r\nInvalid WebSocket handshake headers";
        return -1;
    }

    // 构建握手响应
    response.clear();
    size_t RESPONSE_SIZE = 285; // 实测响应平均长度 256 + 29
    response.reserve(RESPONSE_SIZE);

    // 使用单个内存块构建响应（避免多次内存分配）
    response.append("HTTP/1.1 101 Switching Protocols\r\n"
                   "Upgrade: websocket\r\n"
                   "Connection: Upgrade\r\n"
                   "Sec-WebSocket-Accept: ");
    response.append(accept_key, accept_key_len-1);
    response.append("\r\nServer: tgg_gateway/1.0.0\r\n\r\n");
    return 0;
}

int Websocket::_ElbHealthCheck(std::string_view request, std::string& response)
{
    if(healthcheck)
        return 0;
    if((request.size() >= 16) && (request.substr(0, 16) == "GET /healthcheck")) {
        time_t now = time(nullptr);
        struct tm tm;
        gmtime_r(&now, &tm); // 线程安全的 GMT 时间
        char date_str[128];
        strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S GMT", &tm);
        response.clear();
        response.resize(256);
        response = "HTTP/1.1 204 No Content\r\nDate: ";
        response += std::string(date_str);
        response += "\r\n"
        "Server: tgg_gateway/1.0.0\r\n"
        "Connection: keep-alive\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
        healthcheck = 1;
        return 0;
    }
    return -1;
}

// 编码关闭帧
std::string Websocket::EncodeCloseFrame(std::string_view reason)
{
    std::vector<uint8_t> frame;
    uint16_t status_code = 1000;
    const uint8_t CLOSE_OPCODE = 0x08;
    // 2. 构造有效载荷
    std::vector<uint8_t> payload;
    // 大端序状态码
    payload.push_back((status_code >> 8) & 0xFF);
    payload.push_back(status_code & 0xFF);
    
    // 截断原因短语至123字节
    std::string_view trimmed_reason = reason.substr(0, 123);
    payload.insert(payload.end(), trimmed_reason.begin(), trimmed_reason.end());
    // 3. 构建帧头
    frame.push_back(0b10000000 | CLOSE_OPCODE); // FIN=1 + Opcode=8
    // 4. 处理掩码和长度
    frame.push_back(payload.size()); // 服务端不掩码
    // 5. 组合完整帧
    frame.insert(frame.end(), payload.begin(), payload.end());
    
    return std::string(frame.begin(), frame.end());
}

std::string Websocket::EncodeWebsocketMessage(int opcode, std::string_view message)
{
    std::vector<uint8_t> frame;
    frame.push_back(0b10000000|opcode); // FIN + opcode (text frame)
    size_t length = message.size();

    if (length <= 125) {
        frame.push_back(static_cast<uint8_t>(length));
    } else if (length <= 65535) {
        frame.push_back(126);
        frame.push_back((length >> 8) & 0xFF);
        frame.push_back(length & 0xFF);
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back((length >> (8 * i)) & 0xFF);
        }
    }
    frame.insert(frame.end(), message.begin(), message.end());
    return std::string(frame.begin(), frame.end());
}

std::string Websocket::DecodeWebsocketMessage(const std::vector<uint8_t>& frame)
{
    if (frame.size() < 2) {
        throw std::runtime_error("Frame too short.");
    }

    uint8_t opcode = frame[0] & 0x0F;
    if (opcode != 0x1) {
        throw std::runtime_error("Not a text frame.");
    }

    size_t payload_length = frame[1] & 0x7F;
    size_t index = 2;

    if (payload_length == 126) {
        payload_length = (frame[index] << 8) | frame[index + 1];
        index += 2;
    } else if (payload_length == 127) {
        payload_length = 0;
        for (int i = 0; i < 8; ++i) {
            payload_length = (payload_length << 8) | frame[index++];
        }
    }

    std::string message(frame.begin() + index, frame.begin() + index + payload_length);
    return message;
}

/* parse base frame according to
 * https://www.rfc-editor.org/rfc/rfc6455#section-5.2
 */
int
Websocket::_GetWsFrame(unsigned char *in_buffer, size_t buf_len,
    unsigned char **payload_ptr, size_t *out_len)
{
    unsigned char opcode;
    unsigned char fin;
    unsigned char masked;
    size_t payload_len;
    size_t pos = 2;  // 前2字节是基本头部
    int length_field;

    if (buf_len < 2) {
        return INCOMPLETE_DATA;
    }

    opcode = in_buffer[0] & 0x0F;
    fin = (in_buffer[0] >> 7) & 0x01;
    masked = (in_buffer[1] >> 7) & 0x01;
    length_field = in_buffer[1] & (~0x80);

    if (length_field <= 125) {
        payload_len = length_field;
    } else if (length_field == 126) {
        uint16_t tmp16;
        if (buf_len < 4)
            return INCOMPLETE_DATA;
        memcpy(&tmp16, in_buffer + pos, 2);
        payload_len = ntohs(tmp16);
        pos += 2;
    } else if (length_field == 127) {
        uint64_t tmp64 = 0;
        if (buf_len < 10)
            return INCOMPLETE_DATA;
        
        // 读取64位长度（大端序）
        for (int i = 0; i < 8; i++) {
            tmp64 = (tmp64 << 8) | (uint64_t)in_buffer[pos + i];
        }
        pos += 8;
        
        if (tmp64 > WS_MAX_RECV_FRAME_SZ) {
            LOG_ERROR("frame length %lu exceeds %lu.\n",
                tmp64, (uint64_t)WS_MAX_RECV_FRAME_SZ);
            *payload_ptr = NULL;
            *out_len = 0;
            return ERROR_FRAME;
        }
        payload_len = (size_t)tmp64;
    } else {
        payload_len = length_field;
    }

    // 计算掩码密钥位置
    if (masked) {
        // 检查是否有足够的空间包含掩码密钥
        if (buf_len < pos + 4) {
            return INCOMPLETE_DATA;
        }
    }

    // 检查是否有足够的空间包含完整载荷
    size_t total_frame_len = pos + (masked ? 4 : 0) + payload_len;
    if (buf_len < total_frame_len) {
        // LOG_INFO("buf_len:%zu, needed:%zu", buf_len, total_frame_len);
        return INCOMPLETE_DATA;
    }

    // 处理掩码
    if (masked) {
        unsigned char *mask = in_buffer + pos;  // 掩码密钥位置
        pos += 4;  // 跳过掩码密钥
        
        // 解掩码载荷
        unsigned char *payload = in_buffer + pos;
        for (size_t i = 0; i < payload_len; i++) {
            payload[i] = payload[i] ^ mask[i % 4];
        }
    }

    *payload_ptr = in_buffer + pos;
    *out_len = payload_len;

    // 检查操作码
    if ((opcode >= 3 && opcode <= 7) || (opcode >= 0xb)) {
        return ERROR_FRAME;
    }

    if (opcode <= 0x3 && !fin) {
        return INCOMPLETE_FRAME;
    }
    
    return opcode;
}

#include <cstring>
static inline int check_if_http_end(const char* data, size_t len) {
    // 快速检查最小长度
    if (__builtin_expect(len < 4, 0)) return 0;  // 小于4字节不可能包含\r\n\r\n

    const char* end = data + len;
    const char* ptr = data;

    // 快速扫描首个\r\n位置
    while ((ptr = static_cast<const char*>(memchr(ptr, '\r', end - ptr)))) {
        // 检查连续\r\n\r\n模式
        if (ptr + 3 < end && 
            ptr[1] == '\n' && 
            ptr[2] == '\r' && 
            ptr[3] == '\n') {
            return ptr - data + 4;  // 返回结束位置
        }
        ptr++;  // 继续搜索下一个\r
    }
    return 0;
}

// websocket的解析逻辑，只有毁掉函数和返回值是自定义的，其余都是ai提供的解析代码，目前(2025/07/01)验证结果是正常的
// return  -1 缓存失败，要关闭连接并删除源数据data 0 缓存数据，本次不处理  1 消息处理完成，需要清理缓存
// 缓存区域换成环形缓冲区了，连接建立时创建，关闭时销毁，不再每次缓冲数据时分配
int Websocket::ReadData(void* data, int len)
{
    int left_len = ringbuf_size(core_id, fd);
    if(left_len < 0) {
        // 缓冲区没有数据
        LOG_ERROR("read data from ringbuf failed, fd:%d.", fd);
        return -1;
    }
    int buffer_len = left_len + len;
    char buffer[4096] = {0};
    int read_len = get_one_frame_buffer(this->core_id, this->fd, data, len, buffer);
    if(read_len != buffer_len) {
        LOG_ERROR("read data from ringbuf failed, read_len:%d not match expect_len:%d, fd:%d.", 
            fd, read_len, buffer_len);
        return -1;
    }
    // LOG_INFO("whole buffer:%s len:%d.\n", bin2hex(std::string_view((char*)buffer, read_len)).data(), read_len);
    int cur_pos = 0;
    do {
        int type;
        unsigned char *payload;
        size_t msg_len, in_len, header_sz;
        // std::string completedata = get_one_frame_buffer(this->core_id, this->fd, data, len);
        unsigned char* input = (unsigned char*)(buffer + cur_pos);
        in_len = buffer_len - cur_pos;
        if (in_len <= 0)
        {// 没有数据了 直接返回
            return 0;
        }
        if (handshake != AUTH_TYPE_HANDLESHAKED) {
            size_t buf_len = check_if_http_end((const char*)input, in_len);
            if(buf_len <= 0) { // 分包
                int write_len = ringbuf_write(core_id, fd, (char*)data, len);
                if(write_len < len) {
                // 缓冲区剩余长度不够了
                    LOG_ERROR("free length is not enough, fd:%d.", fd);
                    return -1;
                }
                return 0;
            }
            if(buf_len < in_len) {// 粘包
                size_t write_len = ringbuf_write(core_id, fd, (char*)input + buf_len, in_len - buf_len);
                if(write_len < in_len) {
                // 缓冲区剩余长度不够了
                    LOG_ERROR("free length is not enough, fd:%d.", fd);
                    return -1;
                }
            }
            ws_handshake_t req;
            std::string_view request((char*)input, in_len);
            std::string response;
            if (_HandleHandshake(request, req, response) < 0) {
                _ElbHealthCheck(request, response);
                OnSend(response, FD_WRITE);
                if(!healthcheck)
                    LOG_ERROR("handle shake check failed, fd:%d.", this->fd);
                else
                    LOG_DEBUG("health check ok.");
                return -1;
            }
            OnHandShake(request, response, req);
            cur_pos += in_len;
            ringbuf_move_read_pos(this->core_id, this->fd, cur_pos < left_len ? cur_pos : left_len);
            continue;
        }
        // LOG_INFO("input:%s len:%d cur_pos:%d.", bin2hex(std::string_view((char*)input, in_len)).data(), in_len, cur_pos);
        type = _GetWsFrame(input, in_len, &payload, &msg_len);
        if (type == INCOMPLETE_DATA) {
            /* incomplete data received, wait for next chunk */
            // 数据不完整，先缓存起来，等待下一个包，一个websocket包分在两个分片中  buflen<packetlen
            // 也就是还没有缓存一个完整的websocket包，不用解析，等待下一个包进来拼接在一起
            int write_len;
            if (cur_pos == 0) {
                // 没消费过，就只缓存
                write_len = ringbuf_write(core_id, fd, (char*)data, len);
                if(write_len < len) {
                // 缓冲区剩余长度不够了
                    LOG_ERROR("free length is not enough.");
                    return -1;
                }
            } else {
                // 消费过就要移动读指针
                ringbuf_move_read_pos(this->core_id, this->fd, cur_pos < left_len ? cur_pos : left_len);
                write_len = ringbuf_write(core_id, fd, (char*)input, in_len);
                if(write_len < (int)in_len) {
                // 缓冲区剩余长度不够了
                    LOG_ERROR("free length is not enough.");
                    return -1;
                }
            }
            return 0;
        }
        header_sz = payload - input;
        cur_pos += header_sz + msg_len;
        // std::string buffer = get_whole_buffer(this->core_id, this->fd);
        switch (type) {
            case TEXT_FRAME:
            case BINARY_FRAME:
                OnMessage(std::string_view((char*)payload, msg_len));
                break;
            case INCOMPLETE_FRAME:
            // 多个帧的数据(没有fin标记)，每一帧的数据都有websocket的头，这些数据需要合到一起才能算一个完整的数据包
            // 我们不处理数据包，只负责透传，所以不需要处理多个ws包的拼接
                LOG_WARNING("incomplete frame type %d, fd:%d.", type, fd);
                OnMessage(std::string_view((char*)payload, msg_len));
                if(in_len > msg_len) {
                    LOG_ERROR("incomplete frame type %d, fd:%d, data:%s.",
                     type, fd, bin2hex(std::string_view((char*)input, in_len)).data());
                    return -1;
                }
                // return 0;
                break;
            case CLOSING_FRAME:
                // OnClose();
                SendONnoAuth("", FD_WRITE|FD_CLOSE);// TODO FD_CLOSE会强制关闭socket,这种方式欠妥，会报错
                ringbuf_move_read_pos(this->core_id, this->fd, cur_pos < left_len ? cur_pos : left_len);
                return 1;
                break;
            case ERROR_FRAME:
                LOG_ERROR("error frame, fd:%d data:%s len:%d.", fd, bin2hex(std::string_view((char*)payload, msg_len)).data(), msg_len);
                return -1;// 返回 -1外部会关闭
                break;
            case PING_FRAME:
                OnPing(std::string_view((char*)payload, msg_len));
                break;
            case PONG_FRAME:
                OnPong(std::string_view((char*)payload, msg_len));
                break;
            default:
                LOG_ERROR("unexpected frame type %d, fd:%d.", type, fd);
                return -1;
                break;
        }
    } while(buffer_len > cur_pos);
    ringbuf_move_read_pos(this->core_id, this->fd, cur_pos < left_len ? cur_pos : left_len);
    return 1;
}

void Websocket::SendONnoAuth(const std::string_view data, int fd_opt)
{
    std::string result;
    if(fd_opt & FD_CLOSE) {
        result = EncodeCloseFrame("");
    } else {
        result = EncodeWebsocketMessage(BINARY_FRAME, data.data());
    }
    OnSend(result, fd_opt);
}

void Websocket::SendData(const std::string_view data, int fd_opt) {
    if(handshake) {
        SendONnoAuth(data, fd_opt);
    } else {
        LOG_ERROR("Session should be authorized before send data, fd:%d.", this->fd);
    }
}

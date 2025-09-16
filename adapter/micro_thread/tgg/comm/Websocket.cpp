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

// 判断字符串是否以子串开头，忽略大小写
#ifdef __cpp_lib_starts_ends_with  // C++20 feature test macro
bool strview_starts_with_insensitive(std::string_view str, std::string_view prefix) {
    if (prefix.size() > str.size()) return false;
    return std::equal(str.begin(), str.begin() + prefix.size(), prefix.begin(), prefix.end(),
        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
}
#define STRVIEW_STARTS_WITH(str, prefix) strview_starts_with_insensitive(str, prefix)
#else
bool strview_starts_with(std::string_view str, std::string_view prefix) {
    if (prefix.size() > str.size()) return false;
    return std::equal(str.begin(), str.begin() + prefix.size(), prefix.begin(), prefix.end(),
        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
}
#define STRVIEW_STARTS_WITH(str, prefix) strview_starts_with(str, prefix)
#endif

ValidationResult parse_websocket_request(std::string_view raw_request) {
    ValidationResult result;
    if (raw_request.empty()) return result;

    // 1. Parse request line
    const size_t line_end = raw_request.find("\r\n");
    if (line_end == std::string_view::npos) return result;
    std::string_view request_line = raw_request.substr(0, line_end);

    // 2. Extract query parameters
    size_t query_start = request_line.find('?');
    if (query_start == std::string_view::npos) return result;
    query_start++; // Move past '?'
    
    constexpr std::array<std::string_view, 2> targets = {"token=", "client_properties="};
    std::array<size_t, 2> targets_len = {targets[0].size(), targets[1].size()};
    bool found_token = false, found_client_properties = false;

    for (size_t pos = query_start; pos < request_line.size(); ) {
        size_t param_end = request_line.find_first_of("& ", pos);
        if (param_end == std::string_view::npos) param_end = request_line.size();
        std::string_view param = request_line.substr(pos, param_end - pos);

        if (STRVIEW_STARTS_WITH(param, targets[0])) {
            result.token = param.substr(targets_len[0]);
            found_token = true;
        } else if (STRVIEW_STARTS_WITH(param, targets[1])) {
            result.client_properties = param.substr(targets_len[1]);
            found_client_properties = true;
        }

        if (found_token && found_client_properties) break;
        pos = param_end + (param_end < request_line.size() ? 1 : 0);
    }

    // 3. Validate headers
    constexpr std::array<std::string_view, 3> required_headers = {
        "upgrade: websocket",
        "connection: upgrade",
        "sec-websocket-version: 13"
    };
    std::array<bool, required_headers.size()> found_headers = {false};

    size_t pos = line_end + 2; // Skip request line and \r\n
    while (pos < raw_request.size()) {
        size_t next_line = raw_request.find("\r\n", pos);
        if (next_line == std::string_view::npos) break;
        std::string_view line = raw_request.substr(pos, next_line - pos);
        pos = next_line + 2;

        if (line.empty()) break; // End of headers

        // Check required headers
        for (size_t i = 0; i < required_headers.size(); ++i) {
            if (!found_headers[i] && STRVIEW_STARTS_WITH(line, required_headers[i])) {
                found_headers[i] = true;
            }
        }

        // Extract Sec-WebSocket-Key and Origin
        if (STRVIEW_STARTS_WITH(line, "sec-websocket-key: ")) {
            result.sec_websocket_key = line.substr(19);
            // LOG_INFO("sec_key:%s", result.sec_websocket_key.data());
        } else if (STRVIEW_STARTS_WITH(line, "origin: ")) {
            result.origin = line.substr(8);
        }
    }

    // 4. Validate result
    result.valid = std::all_of(found_headers.begin(), found_headers.end(), [](bool v) { return v; });
    return result;
}

static constexpr std::string_view WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
// 生成websocket连接的唯一键
std::string Websocket::_GenerateAcceptKey(std::string_view key)
{
    // 预分配拼接内存 (key + GUID)
    thread_local std::string concat_key;
    concat_key.reserve(key.size() + WS_GUID.size());
    concat_key.assign(key);
    concat_key.append(WS_GUID);

    // 计算SHA1 (复用内存)
    thread_local std::string sha1_result;
    sha1_result.resize(SHA_DIGEST_LENGTH);
    SHA1(reinterpret_cast<const unsigned char*>(concat_key.data()), 
         concat_key.size(),
         reinterpret_cast<unsigned char*>(sha1_result.data()));

    // Base64编码 (预计算长度)
    const size_t encoded_len = (4 * ((SHA_DIGEST_LENGTH + 2) / 3));
    thread_local std::string base64_result;
    base64_result.resize(encoded_len);
    
    const int actual_len = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(base64_result.data()),
        reinterpret_cast<const unsigned char*>(sha1_result.data()),
        SHA_DIGEST_LENGTH
    );

    // 移除尾部填充的NUL字符
    if (actual_len > 0 && static_cast<size_t>(actual_len) < base64_result.size()) {
        base64_result.resize(actual_len);
    }
    return base64_result;
}

int Websocket::_HandleHandshake(std::string_view request, ValidationResult& req, std::string& response)
{
    if((request.size() < 5) || (request.substr(0, 5) != "GET /")) {
        LOG_DEBUG("Invalid http request, fd:%d", this->fd);
        response = "HTTP/1.1 400 Bad Request\r\n\r\nInvalid request method or path";
        return -1;
    }
    req = parse_websocket_request(request);
    if (!req.valid) {
        if(!_ElbHealthCheck(request, response)) {
            return -1;
        }
        LOG_DEBUG("Invalid WebSocket handshake, fd:%d", this->fd);
        response = "HTTP/1.1 400 Bad Request\r\n\r\nInvalid WebSocket handshake headers";
        return -1;
    }
    std::string accept_key = _GenerateAcceptKey(req.sec_websocket_key);

    // 构建握手响应
    response.clear();
    size_t RESPONSE_SIZE = 256 + accept_key.size(); // 实测响应平均长度
    response.reserve(RESPONSE_SIZE);

    // 使用单个内存块构建响应（避免多次内存分配）
    response.append("HTTP/1.1 101 Switching Protocols\r\n"
                   "Upgrade: websocket\r\n"
                   "Connection: Upgrade\r\n"
                   "Sec-WebSocket-Accept: ");
    response.append(accept_key);
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
        size_t pos;
        int length_field;

        if (buf_len < 2) {
            return INCOMPLETE_DATA;
        }

        opcode = in_buffer[0] & 0x0F;
        fin = (in_buffer[0] >> 7) & 0x01;
        masked = (in_buffer[1] >> 7) & 0x01;

        payload_len = 0;
        pos = 2;
        length_field = in_buffer[1] & (~0x80);

        if (length_field <= 125) {
            payload_len = length_field;
        } else if (length_field == 126) { /* msglen is 16bit */
            uint16_t tmp16;
            if (buf_len < 4)
                return INCOMPLETE_DATA;
            memcpy(&tmp16, in_buffer + pos, 2);
            payload_len = ntohs(tmp16);
            pos += 2;
        } else if (length_field == 127) { /* msglen is 64bit */
            int i;
            uint64_t tmp64 = 0;
            if (buf_len < 10)
                return INCOMPLETE_DATA;
            /* swap bytes from big endian to host byte order */
            for (i = 56; i >= 0; i -= 8) {
                tmp64 |= (uint64_t)in_buffer[pos++] << i;
            }
            if (tmp64 > WS_MAX_RECV_FRAME_SZ) {
                /* Implementation limitation, we support up to 10 MiB
                 * length, as a DoS prevention measure.
                 */
                LOG_ERROR("frame length %lu exceeds %lu.\n",
                    tmp64, (uint64_t)WS_MAX_RECV_FRAME_SZ);
                /* Calling code needs these values; do the best we can here.
                 * Caller will close the connection anyway.
                 */
                *payload_ptr = in_buffer + pos;
                *out_len = 0;
                return ERROR_FRAME;
            }
            payload_len = (size_t)tmp64;
        }
        if (buf_len < payload_len + pos + (masked ? 4u : 0u)) {
            return INCOMPLETE_DATA;
        }

        /* According to RFC it seems that unmasked data should be prohibited
         * but we support it for nonconformant clients
         */
        if (masked) {
            unsigned char *c, *mask;
            size_t i;

            mask = in_buffer + pos; /* first 4 bytes are mask bytes */
            pos += 4;

            /* unmask data */
            c = in_buffer + pos;
            for (i = 0; i < payload_len; i++) {
                c[i] = c[i] ^ mask[i % 4u];
            }
        }

        *payload_ptr = in_buffer + pos;
        *out_len = payload_len;

        /* are reserved for further frames */
        if ((opcode >= 3 && opcode <= 7) || (opcode >= 0xb))
            return ERROR_FRAME;

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
    int buffer_len = ringbuf_size(core_id, fd);
    if(buffer_len < 0) {
        // 缓冲区没有数据
        LOG_ERROR("read data from ringbuf failed, fd:%d.", fd);
        return -1;
    }
    std::string buffer = get_one_frame_buffer(this->core_id, this->fd, data, len);
    buffer_len += len;
    int cur_pos = 0;
    do {
        int type;
        unsigned char *payload;
        size_t msg_len, in_len, header_sz;
        // std::string completedata = get_one_frame_buffer(this->core_id, this->fd, data, len);
        unsigned char* input = (unsigned char*)(buffer.c_str() + cur_pos);
        in_len = buffer_len - cur_pos;
        if (in_len <= 0)
        {// 没有数据了 直接返回
            return 0;
        }
        if (handshake != AUTH_TYPE_HANDLESHAKED) {
            size_t buf_len = check_if_http_end((const char*)input, in_len);
            if(buf_len <= 0) { // 分包
                size_t write_len = ringbuf_write(core_id, fd, (char*)input, in_len);
                if(write_len < in_len) {
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
            ValidationResult req;
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
            continue;
        }
        type = _GetWsFrame(input, in_len, &payload, &msg_len);
        if (type == INCOMPLETE_DATA) {
            /* incomplete data received, wait for next chunk */
            // 数据不完整，先缓存起来，等待下一个包，一个websocket包分在两个分片中  buflen<packetlen
            // 也就是还没有缓存一个完整的websocket包，不用解析，等待下一个包进来拼接在一起
            size_t write_len = ringbuf_write(core_id, fd, (char*)input, in_len);
            if(write_len < in_len) {
            // 缓冲区剩余长度不够了
                LOG_ERROR("free length is not enough.");
                return -1;
            }
            return 0;
        }
        header_sz = payload - input;
        cur_pos += header_sz + msg_len;
        // std::string buffer = get_whole_buffer(this->core_id, this->fd);
        switch (type) {
            case TEXT_FRAME:
            case BINARY_FRAME:
                OnMessage(std::string((char*)payload, msg_len));
                break;
            case INCOMPLETE_FRAME:
            // 多个帧的数据(没有fin标记)，每一帧的数据都有websocket的头，这些数据需要合到一起才能算一个完整的数据包
            // 我们不处理数据包，只负责透传，所以不需要处理多个ws包的拼接
                LOG_WARNING("incomplete frame type %d, fd:%d.", type, fd);
                OnMessage(std::string((char*)payload, msg_len));
                // return 0;
                break;
            case CLOSING_FRAME:
                OnClose();
                return 1;
                break;
            case ERROR_FRAME:
                LOG_ERROR("error frame, fd:%d.", fd);
                return -1;// 返回 -1外部会关闭
                break;
            case PING_FRAME:
                OnPing(std::string((char*)payload, msg_len));
                break;
            case PONG_FRAME:
                OnPong(std::string((char*)payload, msg_len));
                break;
            default:
                LOG_ERROR("unexpected frame type %d, fd:%d.", type, fd);
                break;
        }
    } while(buffer_len > cur_pos);
    return 1;
}

void Websocket::SendONnoAuth(const std::string& data, int fd_opt)
{
    std::string result;
    if(fd_opt & FD_CLOSE) {
        result = EncodeCloseFrame("");
    } else {
        result = EncodeWebsocketMessage(BINARY_FRAME, data.data());
    }
    OnSend(result, fd_opt);
}

void Websocket::SendData(const std::string& data, int fd_opt) {
    if(handshake) {
        SendONnoAuth(data, fd_opt);
    } else {
        LOG_ERROR("Session should be authorized before send data, fd:%d.", this->fd);
    }
}

#include <iostream>
#include <string>
#include <sstream>
#include <map>
#include <algorithm>
#include <unordered_map>
#include <openssl/sha.h>
#include "Encrypt.hpp" // 需要使用 Base64 库
#include "common.hpp"
#include "tgg_comm/tgg_common.h"
#include "Websocket.hpp"
#include "log.hpp"

static const size_t WS_MAX_RECV_FRAME_SZ = 10485760;

bool is_valid_websocket_handshake(const HttpRequest &req) {
    // 检查必需的头字段
    if (req.headers.find("upgrade") == req.headers.end() ||
        req.headers.find("connection") == req.headers.end() ||
        req.headers.find("sec-websocket-key") == req.headers.end()) {
        LOG_ERROR("lack of nessesary key in request header: upgrade, connection, sec-websocket-key");
        return false;
    }

    // 验证协议升级字段
    std::string upgrade = req.headers.at("upgrade");
    std::string connection = req.headers.at("connection");
    std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(), ::tolower);
    std::transform(connection.begin(), connection.end(), connection.begin(), ::tolower);
    if(upgrade == "websocket" && connection.find("upgrade") != std::string::npos) {
       return true;
    }
    LOG_ERROR("invalid upgrade[%s] or connection[%s] in headers", upgrade.c_str(), connection.c_str());
    return false;

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

#include <immintrin.h>  // AVX2指令集

std::string_view extract_websocket_key_fallback(const char* buffer, size_t len) {
    const char* key_header = "Sec-WebSocket-Key: ";
    size_t key_header_len = strlen(key_header);
    
    const char* field_start = static_cast<const char*>(
        memmem(buffer, len, key_header, key_header_len)
    );
    if (!field_start) return {};
    
    const char* key_start = field_start + key_header_len;
    const char* end_ptr = static_cast<const char*>(
        memmem(key_start, len - (key_start - buffer), "\r\n", 2)
    );
    return (end_ptr) ? std::string_view(key_start, end_ptr - key_start) : std::string_view();
}

std::string_view extract_websocket_key(const char* buffer, size_t len) {
    constexpr char key_header[] = "Sec-WebSocket-Key:";
    constexpr size_t key_header_len = sizeof(key_header) - 1;  // 18字节
    
    if (len < key_header_len) return {};
    
    // 1. AVX2扫描字段名
    const __m256i header = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(key_header)
    );
    
    for (size_t i = 0; i <= len - 32; i += 16) {
        __m256i block = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(buffer + i)
        );
        int mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(block, header));
        if (mask == 0) continue;
        
        size_t pos = i + __builtin_ctz(mask);
        const char* field_start = buffer + pos;
        
        // 2. 验证字段边界（必须满足以下条件）
        if (pos > 0 && field_start[-1] != '\n') continue;  // 前需换行（或开头）
        if (field_start[key_header_len] != ' ') continue; // 后需空格（标准格式）
        
        const char* key_start = field_start + key_header_len + 1;  // 跳过": "
        
        // 3. 严格检测行尾（\r\n）
        const char* end_ptr = static_cast<const char*>(
            memmem(key_start, len - (key_start - buffer), "\r\n", 2)  // 查找完整行尾
        );
        if (!end_ptr) end_ptr = buffer + len;  // 无行尾则取到末尾（防御）
        
        // 4. 验证键值格式（Base64长度应为24字节）
        size_t key_len = end_ptr - key_start;
        if (key_len != 24) continue;  // RFC标准长度
        
        return std::string_view(key_start, key_len);
    }
    
    // 回退到纯C实现（处理剩余数据）
    return extract_websocket_key_fallback(buffer, len);
}
#if 0
std::string_view extract_websocket_key(const char* buffer, size_t len) {
    // 1. 定位字段名（固定19字节）
    constexpr char key_header[] = "Sec-WebSocket-Key:";
    constexpr size_t key_header_len = sizeof(key_header) - 1;  // 去掉末尾\0
    
    // 2. 内存扫描（避免使用strstr）
    const char* pos = buffer;
    const char* end = buffer + len - key_header_len;
    
    for (; pos < end; ++pos) {
        // 快速跳过首字符不匹配的位置
        if (*pos != 'S') continue;  
        
        // 批量比较剩余字符（减少分支预测失败）
        if (memcmp(pos, key_header, key_header_len) == 0) {
            pos += key_header_len;
            break;
        }
    }
    
    // 3. 未找到直接返回
    if (pos >= end) return {};
    
    // 4. 提取值（直到遇到\r\n）
    const char* value_start = pos;
    while (*value_start == ' ') ++value_start;  // 跳过空格
    
    const char* value_end = value_start;
    while (value_end < buffer + len - 1) {
        if ((value_end[0] == '\r' && value_end[1] == '\n') || value_end[0] == '\n') break;
        ++value_end;
    }
    
    return std::string_view(value_start, value_end - value_start);
}
#endif
int Websocket::_HandleHandshake(std::string_view request, HttpRequest& req, std::string& response)
{
    // std::istringstream stream(request);
    // std::string line;
    // std::string web_key;
    // int check_count = 2;
    if((request.size() < 5) || (request.substr(0, 5) != "GET /")) {
        LOG_ERROR("Invalid http request:%s", request.data());
        response = "HTTP/1.1 400 Bad Request\r\n\r\nInvalid request method or path";
        return -1;
    }
    // parse_http_request(request, req, false);

    // if (!is_valid_websocket_handshake(req)) {
    //     LOG_ERROR("Invalid WebSocket handshake:%s", request.c_str());
    //     response = "HTTP/1.1 400 Bad Request\r\n\r\nInvalid WebSocket handshake headers";
    //     return -1;
    // }
    std::string_view sec_key = extract_websocket_key(request.data(), request.size());
    std::string accept_key = _GenerateAcceptKey(sec_key);

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
    response.append("\r\nServer: workerman/4.1.15\r\n\r\n");
    return 0;
}

void form_con_req_to_bw_data()
{

}

// 编码关闭帧
std::string Websocket::EncodeCloseFrame(const std::string& reason)
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
    std::string trimmed_reason = reason.substr(0, 123);
    payload.insert(payload.end(), trimmed_reason.begin(), trimmed_reason.end());
    // 3. 构建帧头
    frame.push_back(0b10000000 | CLOSE_OPCODE); // FIN=1 + Opcode=8
    // 4. 处理掩码和长度
    frame.push_back(payload.size()); // 服务端不掩码
    // 5. 组合完整帧
    frame.insert(frame.end(), payload.begin(), payload.end());
    
    return std::string(frame.begin(), frame.end());
}

std::string Websocket::EncodeWebsocketMessage(int opcode, const std::string& message)
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

// #if defined(__i386__) || defined(__x86_64__)
// #include <smmintrin.h>
// static int check_if_http_end(const char* data, int len) {
//     if (len < 4) return 0;
//     // 加载4字节常量：\r\n\r\n
//     const __m128i pattern = _mm_set1_epi32(0x0A0D0A0D); // 小端序：\r\n\r\n
//     for (int i = 0; i <= len - 16; i += 4) {
//         __m128i chunk = _mm_loadu_si128((const __m128i*)(data + i));
//         __m128i cmp = _mm_cmpeq_epi32(chunk, pattern);
//         if (!_mm_testz_si128(cmp, cmp)) {
//             // 找到匹配位置
//             for (int j = i; j < i + 16; j++) {
//                 if (j + 3 < len && 
//                     data[j]=='\r' && data[j+1]=='\n' && 
//                     data[j+2]=='\r' && data[j+3]=='\n') 
//                     return j + 4;
//             }
//         }
//     }
//     return 0;
// }
// #else
// typedef struct {
//     int state;  // 0:初始 1:收到\r 2:收到\r\n 3:收到\r\n\r
// } ParserState;
#if 0
static int check_if_http_end(const char* data, int len) {
    int state = 0;
    for (int i = 0; i < len; i++) {
        switch (state) {
            case 0: if (data[i] == '\r') state = 1; break;
            case 1: 
                if (data[i] == '\n') state = 2; 
                else state = 0;
                break;
            case 2: 
                if (data[i] == '\r') state = 3; 
                else state = 0;
                break;
            case 3: 
                if (data[i] == '\n') return i + 1; // 返回结束位置
                state = 0;
                break;
        }
    }
    return 0;
}
#endif
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
// #endif

// static int check_if_http_end(const char* data, int len) {
//     if (len < 2) {
//         return 0; // 长度不足时直接返回
//     }

//     int index = 0;
//     while (index < len - 1) { // 确保剩余长度至少2字节
//         // 优先检查标准结束符 \r\n\r\n (4字节)
//         if (index + 3 < len && 
//             data[index] == '\r' && 
//             data[index+1] == '\n' && 
//             data[index+2] == '\r' && 
//             data[index+3] == '\n') {
//             return index + 4; // 返回结束位置后4字节
//         }

//         // 检查非标准结束符 \n\n (2字节)
//         if (data[index] == '\n' && data[index+1] == '\n') {
//             return index + 2; // 返回结束位置后2字节
//         }

//         index++;
//     }
//     return 0; // 未找到结束符
// }

// websocket的解析逻辑，只有毁掉函数和返回值是自定义的，其余都是ai提供的解析代码，目前(2025/07/01)验证结果是正常的
// return  -1 缓存失败，要关闭连接并删除源数据data 0 缓存数据，本次不处理  1 消息处理完成，需要清理缓存
// 缓存区域换成环形缓冲区了，连接建立时创建，关闭时销毁，不再每次缓冲数据时分配
int Websocket::ReadData(void* data, int len)
{
    int buffer_len = ringbuf_size(core_id, fd);
    if(buffer_len < 0) {
        // 缓冲区没有数据
        LOG_ERROR("read data from ringbuf failed.");
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
                    LOG_ERROR("free length is not enough.");
                    return -1;
                }
                return 0;
            }
            if(buf_len < in_len) {// 粘包
                size_t write_len = ringbuf_write(core_id, fd, (char*)input + buf_len, in_len - buf_len);
                if(write_len < in_len) {
                // 缓冲区剩余长度不够了
                    LOG_ERROR("free length is not enough.");
                    return -1;
                }
            }
            HttpRequest req;
            std::string_view request((char*)input, in_len);
            std::string response;
            if (_HandleHandshake(request, req, response) < 0) {
                OnSend(response, FD_WRITE);
                LOG_ERROR("handle shake check failed.");
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
                LOG_WARNING("incomplete frame type %d.", type);
                OnMessage(std::string((char*)payload, msg_len));
                // return 0;
                break;
            case CLOSING_FRAME:
                OnClose();
                return 1;
                break;
            case ERROR_FRAME:
                LOG_ERROR("error frame.");
                return -1;// 返回 -1外部会关闭
                break;
            case PING_FRAME:
                OnPing(std::string((char*)payload, msg_len));
                break;
            case PONG_FRAME:
                OnPong(std::string((char*)payload, msg_len));
                break;
            default:
                LOG_ERROR("unexpected frame type %d.", type);
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
        result = EncodeWebsocketMessage(BINARY_FRAME, data);
    }
    OnSend(result, fd_opt);
}

void Websocket::SendData(const std::string& data, int fd_opt) {
    if(handshake) {
        SendONnoAuth(data, fd_opt);
    } else {
        LOG_ERROR("Session should be authorized before send data.");
    }
}

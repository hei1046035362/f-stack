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

// URL解码函数（参考网页[9][10]）
std::string url_decode(const std::string &src) {
    std::string decoded;
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '%' && i + 2 < src.size()) {
            int hex_val;
            std::istringstream hex_stream(src.substr(i+1, 2));
            if (hex_stream >> std::hex >> hex_val) {
                decoded += static_cast<char>(hex_val);
                i += 2;
            }
        } else if (src[i] == '+') {
            decoded += ' ';
        } else {
            decoded += src[i];
        }
    }
    return decoded;
}

// 解析HTTP请求（参考网页[7][11]的握手处理）
void parse_http_request(const std::string &raw_request, HttpRequest& req) {
    std::istringstream stream(raw_request);
    std::string line;

    // 解析请求行
    if (std::getline(stream, line)) {
        std::istringstream line_stream(line);
        line_stream >> req.method >> req.uri >> req.protocol;
        req.protocol = req.protocol.substr(5); // 去除"HTTP/"
    }

    // 解析请求头
    while (std::getline(stream, line) && line != "\r") {
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            std::string value = line.substr(colon_pos + 2); // 跳过": "
            value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
            req.headers[key] = value;
        }
    }

    // 解析QUERY_STRING（参考网页[9]的URL参数处理）
    size_t query_start = req.uri.find('?');
    if (query_start != std::string::npos) {
        std::string query_str = req.uri.substr(query_start + 1);
        std::istringstream query_stream(query_str);
        std::string pair;
        while (std::getline(query_stream, pair, '&')) {
            size_t eq_pos = pair.find('=');
            std::string key = (eq_pos != std::string::npos) ? 
                url_decode(pair.substr(0, eq_pos)) : url_decode(pair);
            std::string value = (eq_pos != std::string::npos) ? 
                url_decode(pair.substr(eq_pos + 1)) : "";
            req.query[key] = value;
        }
    }

    // 新增：解析 Cookies（需在请求头解析完成后添加）
    if (req.headers.find("cookie") != req.headers.end()) {
        std::string cookieStr = req.headers["cookie"];
        std::istringstream cookieStream(cookieStr);
        std::string cookiePair;

        while (std::getline(cookieStream, cookiePair, ';')) {
            // 去除首尾空格（网页4提到的清理逻辑）
            cookiePair.erase(cookiePair.begin(), 
                std::find_if(cookiePair.begin(), cookiePair.end(), 
                    [](int ch) { return !std::isspace(ch); }));
            cookiePair.erase(std::find_if(cookiePair.rbegin(), cookiePair.rend(),
                [](int ch) { return !std::isspace(ch); }).base(), cookiePair.end());

            // 分割键值对（类似查询参数处理）
            size_t eqPos = cookiePair.find('=');
            if (eqPos != std::string::npos) {
                std::string key = url_decode(cookiePair.substr(0, eqPos));
                std::string value = url_decode(
                    cookiePair.substr(eqPos + 1)
                );
                req.cookies[key] = value;  // 需在 HttpRequest 结构体中定义 cookies 成员
            }
        }
    }
}


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

// 生成websocket连接的唯一键
std::string Websocket::_GenerateAcceptKey(const std::string& key)
{
    std::string concat_key = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";    
    return Encrypt::Base64Encode(Encrypt::sha1(concat_key));
}

std::string Websocket::_HandleHandshake(const std::string& request, HttpRequest& req)
{
    // std::istringstream stream(request);
    // std::string line;
    // std::string web_key;
    // int check_count = 2;
    if((request.size() < 5) || (request.substr(0, 5) != "GET /")) {
        std::cerr << "Invalid http request:" << request << std::endl;
        return "";
    }
    parse_http_request(request, req);

    if (!is_valid_websocket_handshake(req)) {
        std::cerr << "Invalid WebSocket handshake" << request << std::endl;
        return "";
    }
    std::string accept_key = _GenerateAcceptKey(req.headers["sec-websocket-key"]);

        // 构建握手响应
    std::ostringstream response;
    response << "HTTP/1.1 101 Switching Protocols\r\n"
    << "Upgrade: websocket\r\n"
    << "Sec-WebSocket-Version: 13\r\n"
    << "Connection: Upgrade\r\n"
    << "Sec-WebSocket-Accept: " << accept_key << "\r\n"
    << "Server: workerman/4.1.15\r\n"
    << "\r\n";
    return response.str();
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

void Websocket::CleanBuffer()
{
    clean_ws_buffer(this->core_id, this->fd);
}
static int check_if_http_end(const char* data, int len)
{
    if(len < 4) {
        return 0;
    }
    int index = 0;
    while (len - index >= 4) {
        if(data[index] == '\r' && data[index+2] == '\r' && data[index+1] == '\n' && data[index+3] == '\n') {
            break;
        }
        index++;
    }
    if(index < len) {
        index += 4;
        return index;
    }
    return 0;
}

// return  -1 缓存失败，要关闭连接并删除源数据data 0 缓存数据，本次不处理  1 消息处理完成，需要清理缓存
int Websocket::ReadData(void* data, int len)
{
    // if (handshake != AUTH_TYPE_HANDLESHAKED) {
        // int read_len = 0;
        // int buf_len = check_if_http_end((const char*)data, len);
        // int reserve_len = ringbuf_size(core_id, fd);
        // if(reserve_len <= 0 && buf_len == len && len > 0){ // 没找到http结尾字符，直接缓存并返回
        // } else{// http包后面粘了下一个包，粘了的部分要缓存起来
        //     int write_len = ringbuf_write(core_id, fd, (char*)data, len);
        //     if(write_len < len) {
        //         // 唤醒缓冲区剩余长度不够了
        //         LOG_ERROR("free length is not enough.");
        //         return -1;
        //     }
        //     return 0;
        // } 
        // std::string read_data;
        // if(reserve_len > 0) {
        //     if (ringbuf_read(core_id, fd, read_data, reserve_len + buf_len, 1) <= 0) {
        //         LOG_ERROR("read ringbuf failed.");
        //         return -1;
        //     }
        // } else {
        //     read_data.append(static_cast<const char*>((char*)data), len);
        // }
    //     HttpRequest req;
    //     std::string response = _HandleHandshake(read_data, req);
    //     if (response.empty()) {
    //         return -1;
    //     }
    //     OnHandShake(response.c_str(), req);
    //     return 1;
    // }
    // int write_len = ringbuf_write(core_id, fd, (char*)data, len);
    // if(write_len < len) {
    //     // 缓冲区剩余长度不够了
    //     LOG_ERROR("free length is not enough.");
    //     return -1;
    // }
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
            std::string response = _HandleHandshake(std::string((char*)input, in_len), req);
            if (response.empty()) {
                LOG_ERROR("handle shake response is empty.");
                return -1;
            }
            OnHandShake(response.c_str(), req);
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
                // if(msg_len > 0) {// close帧有时候会附带数据
                //     OnMessage(std::string((char*)payload, msg_len));
                // }
                OnClose();
                return 1;
                break;
            case ERROR_FRAME:
                LOG_ERROR("error frame.");
                // OnClose();
                return -1;
                break;
            case PING_FRAME:
                OnPing(std::string((char*)payload, msg_len));
                break;
            case PONG_FRAME:
                // /* ping or pong frame */
                // std::string ping_response = encode_websocket_message(PING_FRAME, std::string(payload, msg_len));
                // std::vec
                // EnqueueData(ping_response, );
                //     // TODO 更新fd的定时器
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

#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <rte_mempool.h>
#include "tgg_common.h"
#include "tgg_struct.h"
extern const struct rte_mempool* g_mempool_write;

const std::string Worker::VERSION = "1.0"; // 假设版本号为1.0，实际根据情况设定

class Websocket {
private:
    int fd;
    tgg_cli_info* cli;
public:
    // 对应PHP中的常量定义
    static const std::string BINARY_TYPE_BLOB;
    static const std::string BINARY_TYPE_BLOB_DEFLATE;
    static const std::string BINARY_TYPE_ARRAYBUFFER;
    static const std::string BINARY_TYPE_ARRAYBUFFER_DEFLATE;

    // 静态初始化常量
    Websocket() {
        BINARY_TYPE_BLOB = "\x81";
        BINARY_TYPE_BLOB_DEFLATE = "\xc1";
        BINARY_TYPE_ARRAYBUFFER = "\x82";
        BINARY_TYPE_ARRAYBUFFER_DEFLATE = "\xc2";
    }

    // 检查包的完整性
    static int input(const std::string& buffer) {
        // 接收长度
        size_t recv_len = buffer.length();
        // 需要更多数据
        if (recv_len < 6) {
            return 0;
        }

        // 尚未完成握手
        if (!(cli->status & FD_STATUS_CONNECTED){//connection->getContextValue("websocketHandshake").empty()) {
            return dealHandshake(buffer);
        }

        // 缓冲WebSocket帧数据
        if (connection->getContextValue("websocketCurrentFrameLength")!= "") {
            size_t currentFrameLength = std::stoul(connection->getContextValue("websocketCurrentFrameLength"));
            // 需要更多帧数据
            if (currentFrameLength > recv_len) {
                // 返回0，因为不清楚完整包长度，等待fin=1的帧
                return 0;
            }
        } else {
            unsigned char first_byte = buffer[0];
            unsigned char second_byte = buffer[1];
            size_t data_len = second_byte & 127;
            bool is_fin_frame = (first_byte >> 7)!= 0;
            bool masked = (second_byte >> 7)!= 0;

            if (!masked) {
                Worker::safeEcho("frame not masked so close the connection\n");
                connection->close();
                return 0;
            }

            unsigned char opcode = first_byte & 0xf;
            switch (opcode) {
                case 0x0:
                break;
                // Blob类型
                case 0x1:
                break;
                // Arraybuffer类型
                case 0x2:
                break;
                // 关闭包
                case 0x8: {
                    std::string close_cb = connection->onWebSocketClose();
                    if (close_cb.empty()) {
                        close_cb = connection->onWebSocketClose();
                    }
                    if (!close_cb.empty()) {
                        try {
                            // 这里假设调用方式类似PHP中的回调调用，实际需要根据具体情况完善
                            std::string result = close_cb;
                        } catch (...) {
                            Worker::stopAll(250, "Error in onWebSocketClose callback");
                        }
                    } else {
                        connection->close("\x88\x02\x03\xe8", true);
                    }
                    return 0;
                }
                // Ping包
                case 0x9:
                break;
                // Pong包
                case 0xa:
                break;
                // 错误的opcode
                default:
                Worker::safeEcho("error opcode " + std::to_string(opcode) + " and close websocket connection. Buffer:" + buffer + "\n");
                connection->close();
                return 0;
            }

            // 计算包长度
            size_t head_len = 6;
            if (data_len == 126) {
                head_len = 8;
                if (head_len > recv_len) {
                    return 0;
                }
                // 假设这里有类似unpack的功能函数来解析数据，实际需要完善
                size_t total_len = 0;
                // unpack('nn/ntotal_len', buffer);
                data_len = total_len;
            } else {
                if (data_len == 127) {
                    head_len = 14;
                    if (head_len > recv_len) {
                        return 0;
                    }
                    // 假设这里有类似unpack的功能函数来解析数据，实际需要完善
                    size_t c1 = 0, c2 = 0;
                    // unpack('n/N2c', buffer);
                    data_len = c1 * 4294967296 + c2;
                }
            }
            size_t current_frame_length = head_len + data_len;

            size_t total_package_size = connection->getContextValue("websocketDataBuffer").length() + current_frame_length;
            if (total_package_size > connection->getmaxPackageSize()) {
                Worker::safeEcho("error package. package_length=" + std::to_string(total_package_size) + "\n");
                connection->close();
                return 0;
            }

            if (is_fin_frame) {
                if (opcode == 0x9) {
                    if (recv_len >= current_frame_length) {
                        std::string ping_data = decode(buffer.substr(0, current_frame_length), connection);
                        connection->consumeRecvBuffer(current_frame_length);
                        std::string tmp_connection_type = connection->getwebsocketType();
                        connection->setwebsocketType("\x8a");
                        std::string ping_cb = connection->onWebSocketPing(ping_data);
                        if (!ping_cb.empty()) {
                            try {
                                // 这里假设调用方式类似PHP中的回调调用，实际需要根据具体情况完善
                                std::string result = ping_cb;
                            } catch (...) {
                                Worker::stopAll(250, "Error in onWebSocketPing callback");
                            }
                        } else {
                            connection->send(ping_data);
                        }
                        connection->setwebsocketType(tmp_connection_type);
                        if (recv_len > current_frame_length) {
                            return input(buffer.substr(current_frame_length), connection);
                        }
                    }
                    return 0;
                } else if (opcode == 0xa) {
                    if (recv_len >= current_frame_length) {
                        std::string pong_data = decode(buffer.substr(0, current_frame_length), connection);
                        connection->consumeRecvBuffer(current_frame_length);
                        std::string tmp_connection_type = connection->getwebsocketType();
                        connection->setwebsocketType("\x8a");
                        std::string pong_cb = connection->onWebSocketPong(pong_data);
                        if (!pong_cb.empty()) {
                            try {
                                // 这里假设调用方式类似PHP中的回调调用，实际需要根据具体情况完善
                                std::string result = pong_cb;
                            } catch (...) {
                                Worker::stopAll(250, "Error in onWebSocketPong callback");
                            }
                        }
                        connection->setwebsocketType(tmp_connection_type);
                        if (recv_len > current_frame_length) {
                            return input(buffer.substr(current_frame_length), connection);
                        }
                    }
                    return 0;
                }
                return static_cast<int>(current_frame_length);
            } else {
                connection->setContextValue("websocketCurrentFrameLength", std::to_string(current_frame_length));
            }
        }

        // 只接收到了帧长度数据
        if (connection->getContextValue("websocketCurrentFrameLength") == std::to_string(recv_len)) {
            decode(buffer, connection);
            connection->consumeRecvBuffer(recv_len);
            connection->setContextValue("websocketCurrentFrameLength", "0");
            return 0;
        } // 接收到的数据长度大于一帧的长度
        else if (connection->getContextValue("websocketCurrentFrameLength") < std::to_string(recv_len)) {
            decode(buffer.substr(0, connection->getContextValue("websocketCurrentFrameLength")), connection);
            connection->consumeRecvBuffer(std::stoul(connection->getContextValue("websocketCurrentFrameLength")));
            size_t current_frame_length = std::stoul(connection->getContextValue("websocketCurrentFrameLength"));
            connection->setContextValue("websocketCurrentFrameLength", "0");
            // 继续读取下一帧
            return input(buffer.substr(current_frame_length), connection);
        } // 接收到的数据长度小于一帧的长度
        else {
            return 0;
        }
    }

    // WebSocket编码
    static std::string encode(const std::string& buffer, ConnectionInterface* connection) {
        if (!is_scalar(buffer)) {
            throw std::runtime_error("You can't send(" + typeid(buffer).name() + ") to client, you need to convert it to a string. ");
        }

        if (connection->getwebsocketType().empty()) {
            connection->setwebsocketType(BINARY_TYPE_BLOB);
        }

        // permessage-deflate，这里假设已有类似功能函数，实际需要完善
        if ((connection->getwebsocketType()[0] & 64)!= 0) {
            buffer = deflate(connection, buffer);
        }

        std::string first_byte = connection->getwebsocketType();
        size_t len = buffer.length();

        if (len <= 125) {
            return first_byte + static_cast<char>(len) + buffer;
        } else {
            if (len <= 65535) {
                return first_byte + static_cast<char>(126) + pack("n", len) + buffer;
            } else {
                return first_byte + static_cast<char>(127) + pack("xxxxN", len) + buffer;
            }
        }
    }

    // WebSocket解码
    static std::string decode(const std::string& buffer, ConnectionInterface* connection) {
        unsigned char first_byte = buffer[0];
        unsigned char second_byte = buffer[1];
        size_t len = second_byte & 127;
        bool is_fin_frame = (first_byte >> 7)!= 0;
        bool rsv1 = (first_byte & 64) == 64;

        if (len == 126) {
            std::string masks = buffer.substr(4, 4);
            std::string data = buffer.substr(8);
        } else {
            if (len == 127) {
                std::string masks = buffer.substr(10, 4);
                std::string data = buffer.substr(14);
            } else {
                std::string masks = buffer.substr(2, 4);
                std
                data = buffer.substr(6);
            }
        }
        size_t dataLength = data.length();
        masks = std::string(dataLength / 4, masks) + masks.substr(0, dataLength % 4);
        std::string decoded = data ^ masks;
        if (connection->getContextValue("websocketCurrentFrameLength")!= "") {
            std::string websocketDataBuffer = connection->getContextValue("websocketDataBuffer");
            websocketDataBuffer += decoded;
            if (rsv1) {
                return inflate(connection, websocketDataBuffer, is_fin_frame);
            }
            return websocketDataBuffer;
        } else {
            if (connection->getContextValue("websocketDataBuffer")!= "") {
                decoded = connection->getContextValue("websocketDataBuffer") + decoded;
                connection->getContextValue("websocketDataBuffer") = "";
            }
            if (rsv1) {
                return inflate(connection, decoded, is_fin_frame);
            }
            return decoded;
        }
    }

    // 解压缩，这里假设已有类似功能函数，实际需要完善
    protected static std::string inflate(ConnectionInterface* connection, const std::string& buffer, bool is_fin_frame) {
        // 假设已有类似inflate_init和inflate_add的功能函数及相关结构体定义，实际需要完善
        return "";
    }

    // 压缩，这里假设已有类似功能函数，实际需要完善
    protected static std::string deflate(ConnectionInterface* connection, const std::string& buffer) {
        // 假设已有类似deflate_init和deflate_add的功能函数及相关结构体定义，实际需要完善
        return "";
    }


// 分割字符串函数
std::vector<std::string> SplitString(const std::string& input, const std::string& delimiter) {
    std::vector<std::string> result;
    size_t pos = 0;
    size_t prevPos = 0;

    while ((pos = input.find(delimiter, prevPos))!= std::string::npos) {
        result.push_back(input.substr(prevPos, pos - prevPos));
        prevPos = pos + delimiter.length();
    }

    // 添加最后一部分字符串（如果还有剩余部分）
    if (prevPos < input.length()) {
        result.push_back(input.substr(prevPos));
    }

    return result;
}

// 解析HTTP头信息
    void parseHttpHeader(const std::string& buffer, std::string& request_method, std::string& request_uri,
       std::string& server_protocol, std::vector<std::string>& http_headers,
       std::string& query_string, std::vector<std::string>& cookies) {
    // 找到 \r\n\r\n 分割HTTP头和正文
        size_t header_end_pos = buffer.find("\r\n\r\n");
        if (header_end_pos == std::string::npos) {
            return;
        }
        std::string http_header = buffer.substr(0, header_end_pos);

    // 解析请求行
        size_t space_pos1 = http_header.find(' ');
        size_t space_pos2 = http_header.find(' ', space_pos1 + 1);
        request_method = http_header.substr(0, space_pos1);
        request_uri = http_header.substr(space_pos1 + 1, space_pos2 - space_pos1 - 1);
        server_protocol = http_header.substr(space_pos2 + 1);

    // 解析其他头信息
        std::vector<std::string> header_lines = SplitString(http_header.substr(space_pos2 + 1), "\r\n");
        for (const auto& line : header_lines) {
            if (line.empty()) {
                continue;
            }
            size_t colon_pos = line.find(':');
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);
            key = convertToUpperCase(key);
            value = trimString(value);

            if (key == "HOST") {
                std::vector<std::string> host_parts = SplitString(value, ":");
                http_headers.push_back("SERVER_NAME: " + host_parts[0]);
                if (host_parts.size() > 1) {
                    http_headers.push_back("SERVER_PORT: " + host_parts[1]);
                }
            } else if (key == "COOKIE") {
                cookies = SplitString(parseCookieString(value), ";");
            } else {
                http_headers.push_back("HTTP_" + key + ": " + value);
            }
        }

    // 解析查询字符串
        query_string = parseQueryString(request_uri);
    }
private:
    tgg_write_data* format_send_data(const std::string& sdata, int fd_opt)
    {
        tgg_write_data* wdata = NULL;
        int ret = rte_mempool_get(g_mempool_write, &wdata);
        // TODO  建议增加循环处理，内存池不够，可以稍微等待消费端释放
        if (ret < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d] get mem from write pool failed,code:%d.", 
                __func__, __LINE__, ret);
            return NULL;
        }
        wdata->lst_fd = dpdk_rte_malloc(sizeof(tgg_fd_list));
        if (!data) {
            memset(wdata, 0, sizeof(tgg_write_data));
            rte_mempool_put(g_mempool_write, wdata);
            return NULL;
        }
        wdata->lst_fd->next = dpdk_rte_malloc(sizeof(tgg_fd_list));
        if (!data) {
            rte_free(wdata->lst_fd);
            memset(wdata, 0, sizeof(tgg_write_data));
            rte_mempool_put(g_mempool_write, wdata);
            return NULL;
        }
        if (sdata.length() > 0) {
            wdata->data = dpdk_rte_malloc(sdata.length());
            if (!wdata->data) {
                rte_free(wdata->lst_fd->next);
                rte_free(wdata->lst_fd);
                memset(wdata, 0, sizeof(tgg_write_data));
                rte_mempool_put(g_mempool_write, wdata);
                return NULL;
            }
            memcpy((char*)wdata->data, sdata.c_str(), sdata.length());
            wdata->len = sdata.length();
        } else {
            wdata->len = 0;
        }
        wdata->lst_fd->next->fd = fd;
        wdata->lst_fd->next->idx = cli->idx;
        wdata->lst_fd->next->fd_opt = fd_opt;
        return wdata;
    }

    int EnqueueData(const std::string& data)
    {
        tgg_write_data* wdata = format_send_data(data);
        if (!wdata) {
            RTE_LOG(ERR, USER1, "[%s][%d] Format send data failed.", 
                __func__, __LINE__);
            return -1;
        }
        int idx = 10;
        while (tgg_enqueue_write(wdata) < 0 && idx-- > 0 ) {
            usleep(10);
        }
        if (idx <= 0) {
            RTE_LOG(ERR, USER1, "[%s][%d] Enqueue write data failed.", 
                __func__, __LINE__);
            return -1;
        }
        return 0;

    }

    void CloseConn()
    {
        std::string data = "HTTP/1.1 200 WebSocket\r\nServer: workerman/" \
            + Worker::VERSION + "\r\n\r\n<div style=\"text-align:center\"><h1>WebSocket</h1><hr>workerman/"\
            + Worker::VERSION + "</div>";
        EnqueueData(data, FD_WRITE);
        EnqueueData("", FD_CLOSE);// 这里理论上没有关闭成功也没事，对端也不会再发心跳了，定时器会监控到并强制关闭
    }
    // 处理WebSocket握手
    int dealHandshake(const std::string& buffer) {
        // HTTP协议
        if (buffer.find("GET") == 0) {
            // 找到 \r\n\r\n
            size_t header_end_pos = buffer.find("\r\n\r\n");
            if (header_end_pos == std::string::npos) {
                return 0;
            }
            size_t header_length = header_end_pos + 4;

            // 获取Sec-WebSocket-Key
            std::string Sec_WebSocket_Key;
            size_t key_pos = buffer.find("Sec-WebSocket-Key:");
            if (key_pos!= std::string::npos) {
                size_t start_pos = key_pos + 16;
                size_t end_pos = buffer.find("\r\n", start_pos);
                Sec_WebSocket_Key = buffer.substr(start_pos, end_pos - start_pos);
            } else {
                //connection->close("HTTP/1.1 200 WebSocket\r\nServer: workerman/" + Worker::VERSION + "\r\n\r\n<div style=\"text-align:center\"><h1>WebSocket</h1><hr>workerman/" + Worker::VERSION + "</div>",
                //    true);
                CloseConn();
                return 0;
            }

            // 计算WebSocket key
            std::string new_key = Encrypt::Base64Encode(Encrypt::sha1(Sec_WebSocket_Key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")));

            // 握手响应数据
            std::string handshake_message = "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + new_key + "\r\n";

        // 初始化WebSocket相关上下文数据
            connection->setContextValue("websocketDataBuffer", "");
            connection->setContextValue("websocketCurrentFrameLength", "0");
            connection->setContextValue("websocketCurrentFrameBuffer", "");

        // 消耗握手数据
            connection->consumeRecvBuffer(header_length);

        // 尝试触发onWebSocketConnect回调
            std::string on_websocket_connect = connection->onWebSocketConnect(buffer);
            if (!on_websocket_connect.empty()) {
                std::string request_method, request_uri, server_protocol;
                std::vector<std::string> http_headers;
                std::string query_string;
                std::vector<std::string> cookies;
                parseHttpHeader(buffer, request_method, request_uri, server_protocol, http_headers, query_string, cookies);
                try {
                // 假设这里有合适的方式调用回调函数，实际需要根据具体情况完善
                    std::string result = on_websocket_connect;
                } catch (...) {
                    Worker::stopAll(250, "Error in onWebSocketConnect callback");
                }
                if (!cookies.empty() && classExists("GatewayWorker\\Lib\\Context")) {
                    connection->setSession(encodeSession(cookies));
                }
            }

        // 设置WebSocket类型为默认值（假设这里是BINARY_TYPE_BLOB对应的类型）
            if (connection->getwebsocketType().empty()) {
                connection->setwebsocketType("\x81");
            }

        // 添加Server头信息到握手响应
            bool has_server_header = false;
            if (connection->hasHeaders()) {
                std::vector<std::string> headers = connection->getHeaders();
                for (const auto& header : headers) {
                    if (header.find("Server:") == 0) {
                        has_server_header = true;
                    }
                    handshake_message += header + "\r\n";
                }
            }
            if (!has_server_header) {
                handshake_message += "Server: workerman/" + Worker::VERSION + "\r\n";
            }
            handshake_message += "\r\n";

        // 发送握手响应
            connection->send(handshake_message, true);

        // 标记握手完成
            connection->setContextValue("websocketHandshake", "true");

        // 发送等待的数据（如果有）
            if (!connection->getContextValue("tmpWebsocketData").empty()) {
                connection->send(connection->getContextValue("tmpWebsocketData"), true);
                connection->setContextValue("tmpWebsocketData", "");
            }

            if (buffer.length() > header_length) {
                return input(buffer.substr(header_length), connection);
            }
            return 0;
        }
    // 错误的WebSocket握手请求
        connection->close("HTTP/1.1 200 WebSocket\r\nServer: workerman/" + Worker::VERSION + "\r\n\r\n<div style=\"text-align:center\"><h1>WebSocket</h1><hr>workerman/" + Worker::VERSION + "</div>",
            true);
        return 0;
    }
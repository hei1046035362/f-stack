#ifndef __WEBSOCKET_HPP__
#define __WEBSOCKET_HPP__

#include <string>
#include <vector>
#include <string_view>
#include "common.hpp"
enum WebSocketFrameType {
    ERROR_FRAME = 0xFF,
    INCOMPLETE_DATA = 0xFE,

    CLOSING_FRAME = 0x8,

    INCOMPLETE_FRAME = 0x81,

    TEXT_FRAME = 0x1,
    BINARY_FRAME = 0x2,

    PING_FRAME = 0x9,
    PONG_FRAME = 0xA
};

#include "picohttpparser.h"
struct ws_handshake_t {
    int is_valid_handshake;
    const char* ws_key;
    size_t ws_key_len;
    const char* ws_version;
    size_t ws_version_len;
    
    // Cookie
    struct phr_header cookies[10];
    int num_cookies;
    
    // 其他常用头部
    const char* host;
    size_t host_len;
    const char* origin;
    size_t origin_len;
} ;


// TODO: 为了快速开发，目前websocket的缓存和握手状态都在st_cli_info中，后续需要重新封装一下
//          方法要和数据隔离
class Websocket
{
public:
    Websocket() {healthcheck=0;}
    ~Websocket() {}
protected:
    int core_id;
    int fd;
    int handshake;  // ws的handleshake是否成功
    int healthcheck;// 标记http请求是否为健康检查
private:

    // 新的连接处理
    std::string _ClientConnect(const std::string& request);
    // 生成websocket连接的唯一键
    int _GenerateAcceptKey(const char* client_key, size_t key_len,
                                            char* accept_key, size_t& accept_key_capacity);

    int _HandleHandshake(std::string_view request, ws_handshake_t& req, std::string& response);

    int _ElbHealthCheck(std::string_view request, std::string& response);

    /* parse base frame according to
     * https://www.rfc-editor.org/rfc/rfc6455#section-5.2
     */
    int _GetWsFrame(unsigned char *in_buffer, size_t buf_len,
        unsigned char **payload_ptr, size_t *out_len);

protected:

    // ws握手前调用的发送接口
    void SendONnoAuth(const std::string& data, int fd_opt);
public:
    // 所有发送数据都在子类执行，这里只做websocket相关的公共操作
    virtual void OnConnect() = 0;
    virtual void OnHandShake(std::string_view request, const std::string& response, struct ws_handshake_t& req) = 0;
    virtual void OnMessage(const std::string& msg) = 0;
    virtual void OnClose() = 0;// 子类继承后要执行clean_buffer清理缓存
    virtual void OnPing(const std::string& response) {};
    virtual void OnPong(const std::string& response) {};
    virtual void OnSend(const std::string& msg, int fd_opt) = 0;

    // return  -1 缓存失败，要关闭连接并删除源数据data 0 缓存数据，本次不处理  1 消息处理完成，需要清理缓存
    int ReadData(void* data, int len);

    // ws握手成功后才能调用
    void SendData(const std::string& data, int fd_opt);


    static std::string EncodeCloseFrame(std::string_view reason);

    static std::string EncodeWebsocketMessage(int opcode, std::string_view message);

    static std::string DecodeWebsocketMessage(const std::vector<uint8_t>& frame);
};


#endif // __WEBSOCKET_HPP__
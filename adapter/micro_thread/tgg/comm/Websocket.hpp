#ifndef __WEBSOCKET_HPP__
#define __WEBSOCKET_HPP__

#include <string>
#include <vector>

// TODO: 为了快速开发，目前websocket的缓存和握手状态都在st_cli_info中，后续需要重新封装一下
//          方法要和数据隔离
class Websocket
{
public:
    Websocket() {}
    ~Websocket() {}
    void InitWebsocket(int fd, int handshake) {this->fd = fd; this->handshake = handshake;}
protected:
    int fd;
    int handshake;

protected:
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
private:

    // 生成websocket连接的唯一键
    std::string _GenerateAcceptKey(const std::string& key);

    std::string _HandleHandshake(const std::string& request);
        
    /* parse base frame according to
     * https://www.rfc-editor.org/rfc/rfc6455#section-5.2
     */
    int _GetWsFrame(unsigned char *in_buffer, size_t buf_len,
        unsigned char **payload_ptr, size_t *out_len);

protected:

    virtual void CleanBuffer();
    // ws握手前调用的发送接口
    void SendONnoAuth(const std::string& data, int fd_opt);
public:
    // 所有发送数据都在子类执行，这里只做websocket相关的公共操作
    virtual void OnHandShake(const std::string& response) = 0;
    virtual void OnMessage(const std::string& msg) = 0;
    virtual void OnClose() = 0;// 子类继承后要执行clean_buffer清理缓存
    virtual void OnPing(const std::string& response) {};
    virtual void OnPong(const std::string& response) {};
    virtual void OnSend(const std::string& msg, int fd_opt) = 0;

    // return  -1 缓存失败，要关闭连接并删除源数据data 0 缓存数据，本次不处理  1 消息处理完成，需要清理缓存
    int ReadData(void* data, int len);

    // ws握手成功后才能调用
    void SendData(const std::string& data, int fd_opt);


    static std::string EncodeWebsocketMessage(int opcode, const std::string& message);

    static std::string DecodeWebsocketMessage(const std::vector<uint8_t>& frame);
};


#endif // __WEBSOCKET_HPP__
#ifndef __WS_CONSUMER_H__
#define __WS_CONSUMER_H__
#include <string>
#include <list>
#include "tgg_struct.h"
#include "comm/Websocket.hpp"


class WsConsumer : Websocket
{
public:

    WsConsumer() {_status = FD_STATUS_READYFORCONNECT;}

    virtual ~WsConsumer() {}

    int ConsumerData(void* data);

    bool SendedClose() {return _status == FD_STATUS_CLOSING;}

protected:
    bool ConnectionValid(int core_id, int fd, void* data);

    virtual void OnConnect();

    // 握手
    virtual void OnHandShake(const std::string& response, struct HttpRequest& req);

    virtual void OnPing(const std::string& response);

    virtual void OnPong(const std::string& response);

    virtual void OnMessage(const std::string& msg);

    // 这个接口只负责发送(入队列)，加解密都不做
    virtual void OnSend(const std::string& msg, int fd_opt);

    virtual void OnClose();

private:
    void _CleanAndClose();
    bool _CheckToken(const std::string& token);


    // 客户端的内容透传到服务端
    
    
    enum SEND_SERVER_STATUS {
        SEND_SUCCESS = 0,
        NO_BW_AVALIABLE = 1,
        SEND_FAILED = 2
    };
    
    // 客户端数据转发给服务端
    /// @param --fd   客户端连接的fd，用来做随机的，发送的队列是固定的，
    ///               防止进程之间不必要的信息交换，直接用fd%队列数做负载均衡,因为fd是可回收的，所以这个均衡还是有一定保障的
    int _Send2Server(const std::string& data, int fd_opt);

private:
    int _idx;
    int _status;
    std::string _uid;
    int _cid;
    void* data;
};


#endif // __WS_CONSUMER_H__
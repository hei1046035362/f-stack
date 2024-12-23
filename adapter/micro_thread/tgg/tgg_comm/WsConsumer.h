#ifndef __WS_CONSUMER_H__
#define __WS_CONSUMER_H__
#include "comm/Websocket.hpp"
#include <string>
#include <list>


class WsConsumer : Websocket
{
public:

    WsConsumer() {}

    virtual ~WsConsumer() {}//clean_read_data((tgg_read_data*)data);}

    int ConsumerData(void* data);

    // 后台直发客户端的数据，token校验成功后才能正常调用本接口
    static void Send2Client(const char* cid, const std::string& data, int fd_opt);

    // 批量发送接口
    static void BatchSend2Client(std::list<std::string> cids, const std::string& data, int fd_opt);

    // 批量发送接口
    static void BatchSend2Client(std::list<int> fds, const std::string& data, int fd_opt);

protected:
    bool ConnectionValid(int fd, void* data);

    virtual void OnClose();
    // 握手
    virtual void OnHandShake(const std::string& response);

    virtual void OnPing(const std::string& response);

    virtual void OnPong(const std::string& response);

    virtual void OnMessage(const std::string& msg);

    // 这个接口只负责发送(入队列)，加解密都不做
    virtual void OnSend(const std::string& msg, int fd_opt);

private:
    void _CleanAndClose();
    void _CleanData();
private:
    int _idx;
    int _status;
    std::string _uid;
    std::string _cid;
    void* data;
};


#endif // __WS_CONSUMER_H__
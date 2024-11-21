#ifndef __WS_CONSUMER_H__
#define __WS_CONSUMER_H__
#include "comm/Websocket.hpp"
#include <string>


class WsConsumer : Websocket
{
public:

    WsConsumer() {}

    virtual ~WsConsumer() {}//clean_read_data((tgg_read_data*)data);}

    int ConsumerData(void* data);

protected:
    bool ConnectionValid(int fd, void* data);

    virtual void OnClose();
    // 握手
    virtual void OnHandShake(const std::string& response);

    virtual void OnPing(const std::string& response);

    virtual void OnPong(const std::string& response);

    virtual void OnMessage(const std::string& msg);

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
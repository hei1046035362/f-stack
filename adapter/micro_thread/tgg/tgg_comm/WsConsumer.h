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

    virtual ~WsConsumer() {}//clean_read_data((tgg_read_data*)data);}

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
    void _CleanData();
    bool _CheckToken(const std::string& token);
private:
    int _idx;
    int _status;
    std::string _uid;
    int _cid;
    void* data;
};


#endif // __WS_CONSUMER_H__
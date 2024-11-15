#ifndef __WS_CONSUMER_H__
#define __WS_CONSUMER_H__
#include "nlohmann/json.hpp"
#include "tgg_bwcomm.h"
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/tgg_struct.h"
#include "comm/Websocket.hpp"
#include "tgg_bwcomm.h"
#include "CmdProcessor.h"
#include <vector>
#include <string>


class WsConsumer : Websocket
{
public:

    WsConsumer() {}

    virtual ~WsConsumer() {}//clean_read_data((tgg_read_data*)data);}

    int ConsumerData(void* data)
    {
        tgg_read_data* rdata = (tgg_read_data*)data;

        if (!ConnectionValid(rdata->fd, data)) {
            return -1;
        }
        InitWebsocket(rdata->fd, tgg_get_cli_authorized(rdata->fd));
        int ret = ReadWsData(rdata->data, rdata->data_len);
        if (ret < 0) {
            OnClose();
            CleanBuffer();
            clean_read_data(rdata);
        } else if (ret > 0) {
            CleanBuffer();
        }
        return 0;
    }

protected:
    bool ConnectionValid(int fd, void* data)
    {
        this->fd = fd;
        this->data = data;
        _idx = tgg_get_cli_idx(fd);
        if(_idx < 0) {// fd超过了可用范围
            return false;
        }
        _status = tgg_get_cli_status(fd);
        if (_status > FD_STATUS_KEEP) {
            // TODO 状态迁移待改进，连接已经关闭了
            return false;
        }
        _cid = tgg_get_cli_cid(fd);
        _uid = tgg_get_cli_uid(fd);
        if (_idx != ((tgg_read_data*)data)->idx) {
            // 说明当前的数据已经是上一个连接的数据了
            RTE_LOG(ERR, USER1, "[%s][%d] client idx[%d] not match to data idx[%d].",
               __func__, __LINE__, _idx, ((tgg_read_data*)data)->idx);
            return false;
        }
        return true;
    }

    void SendData(const std::string& data, int fd_opt) {
        if (enqueue_data_single_fd(data, this->fd, _idx, fd_opt) < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d] Enqueue data Failed: cid:%s,uid:%s,opt:%d",
             __func__, __LINE__, _cid.c_str(), _uid.c_str(), fd_opt);
            enqueue_data_single_fd("", this->fd, _idx, FD_CLOSE);
        }
    }
    virtual void OnClose()
    {// 子类继承后要执行clean_buffer清理缓存
        std::string data = "\x88\x02\x03\xe8";// 关闭websocket
        SendData(data, FD_WRITE);
        SendData("", FD_CLOSE);// 关闭fd，这里理论上没有关闭成功也没事，对端也不会再发心跳了，定时器会监控到并强制关闭
    }
    // 握手
    virtual void OnHandShake(const std::string& request)
    {
        SendData(request, FD_WRITE);// 关闭fd，这里理论上没有关闭成功也没事，对端也不会再发心跳了，定时器会监控到并强制关闭
        tgg_set_cli_authorized(this->fd, 1);
    }

    virtual void OnPing(const std::string& response)
    {

    }

    virtual void OnPong(const std::string& response)
    {

    }

    virtual void OnMessage(const std::string& msg)
    {
        try {
            std::string msg_send;
            std::string message = message_unpack(msg);
            nlohmann::json jmsg = nlohmann::json::parse(message);
            std::vector<uint8_t> vec;
            switch(std::stoi(jmsg["cmd"].get<std::string>())) {
                case 0:// 心跳
                    vec = message_pack(std::stoi(jmsg["cmd"].get<std::string>()),1,1,std::stoi(jmsg["compressFormat"].get<std::string>()),"");
                    break;
                case 1:// 通信消息
                    {
                        nlohmann::json jbody = nlohmann::json::parse(jmsg["body"].get<std::string>());
                        std::string token = jbody["token"];
                        if (token.empty()) {
                            RTE_LOG(ERR, USER1, "[%s][%d] token can't be empty.", __func__, __LINE__);
                            goto OnMessageEnd;
                        }
                        Encrypt encryptor = GetEncryptor();
                        std::string decryptor = encryptor.Aes128Decrypt(token);
                        if (decryptor.empty()) {
                            RTE_LOG(ERR, USER1, "[%s][%d] token decrypted error.", __func__, __LINE__);
                            goto OnMessageEnd;
                        }
                        nlohmann::json jtoken = nlohmann::json::parse(decryptor);
                        char resArray[32] = {0};
                        sprintf(resArray, "bind %s\n", jtoken["user_id"].get<std::string>().c_str());
                        std::string res = std::string(resArray, sizeof(resArray));
                        // TODO uid和cid绑定
                        CmdBindUid(this->fd, this->data, jtoken["user_id"].get<std::string>());
                        vec = message_pack(jmsg["cmd"],1,0,jmsg["compressFormat"], jtoken["user_id"]);
                    }
                    break;
                default:
                    vec = message_pack(0,1,0,std::stoi(jmsg["compressFormat"].get<std::string>()),"message error~\n");
                    break;
            }
            msg_send = std::string(vec.begin(), vec.end());
            SendData(msg_send, FD_WRITE);
            return;
        } catch (const nlohmann::detail::parse_error& e) {
            RTE_LOG(ERR, USER1, "[%s][%d] parse json error:%s.", __func__, __LINE__, e.what());
        } catch (...) {
        // 捕获其他任何未预料到的异常
            RTE_LOG(ERR, USER1, "[%s][%d] Unknown exception catched.", __func__, __LINE__);
        }
OnMessageEnd:
    OnClose();
    }

private:
    int _idx;
    int _status;
    std::string _uid;
    std::string _cid;
    void* data;
};


#endif // __WS_CONSUMER_H__
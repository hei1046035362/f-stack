#include "nlohmann/json.hpp"
#include "tgg_bwcomm.h"
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/tgg_struct.h"
#include "tgg_bwcomm.h"
#include "cmd/CmdProcessor.h"
#include "WsConsumer.h"
#include "comm/Encrypt.hpp"


int WsConsumer::ConsumerData(void* data)
{
    tgg_read_data* rdata = (tgg_read_data*)data;
    if (!ConnectionValid(rdata->fd, data)) {
        _CleanAndClose();
        return 0;
    }
    if (rdata->fd_opt & FD_CLOSE)
    {
        _CleanAndClose();
        return 0;
    }
    InitWebsocket(rdata->fd, tgg_get_cli_authorized(rdata->fd));
    int ret = ReadData(rdata->data, rdata->data_len);
    if (ret < 0) {
        _CleanAndClose();
    } else if (ret > 0) {
        _CleanData();
    }// 等于0属于帧不完整，ws缓存了数据不能清理
    return 0;
}

bool WsConsumer::ConnectionValid(int fd, void* data)
{
    this->fd = fd;
    this->data = data;
    _idx = tgg_get_cli_idx(fd);
    if(_idx <= 0) {// fd超过了可用范围
        return false;
    }
    // _status = tgg_get_cli_status(fd);
    // if (_status > FD_STATUS_KEEP) {
    //     // TODO 状态迁移待改进，连接已经关闭了
    //     return false;
    // }
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

void WsConsumer::OnClose()
{// 子类继承后要执行clean_buffer清理缓存
    std::string data = "\x88\x02\x03\xe8";// 关闭websocket
    SendONnoAuth(data, FD_WRITE|FD_CLOSE);
    // SendData("", FD_CLOSE);// 关闭fd，这里理论上没有关闭成功也没事，对端也不会再发心跳了，定时器会监控到并强制关闭
}
// 握手
void WsConsumer::OnHandShake(const std::string& response)
{
    OnSend(response, FD_WRITE);// 关闭fd，这里理论上没有关闭成功也没事，对端也不会再发心跳了，定时器会监控到并强制关闭
    tgg_set_cli_authorized(this->fd, AUTH_TYPE_HANDLESHAKED);
    SendONnoAuth(message_pack(2, 1, 0, 1,get_valid_cid(this->_idx)), FD_WRITE);
}

void WsConsumer::OnPing(const std::string& response)
{
}

void WsConsumer::OnPong(const std::string& response)
{
}

void WsConsumer::OnMessage(const std::string& msg)
{
    if(msg.empty()) {
        RTE_LOG(ERR, USER1, "[%s][%d] msg can't be empty.", __func__, __LINE__);
        return;
    }
    try {
        std::string msg_send;
        std::string message = message_unpack(msg);
        if(message.empty()) {
            RTE_LOG(ERR, USER1, "[%s][%d] message_unpack msg failed,data:%s\r\n", 
                __func__, __LINE__, Encrypt::bin2hex(msg).c_str());
            return;
        }
        nlohmann::json jmsg = nlohmann::json::parse(message);
        switch(jmsg["cmd"].get<std::int32_t>()) {
            case 0:// 心跳
                msg_send = message_pack(jmsg["cmd"],1,1,jmsg["compressFormat"].get<std::int32_t>(),"");
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
                    // char resArray[32] = {0};
                    std::string res = "bind ";
                    res += std::to_string(jtoken["user_id"].get<std::uint64_t>());
                    res += "\n";
                    // sprintf(resArray, "bind %lu\n", jtoken["user_id"].get<std::uint64_t>());
                    // std::string res = std::string(resArray, strlen(resArray));
                    // TODO uid和cid绑定
                    CmdBindUid(this->fd, this->data, std::to_string(jtoken["user_id"].get<std::uint64_t>()));
                    tgg_set_cli_authorized(this->fd, AUTH_TYPE_TOKENCHECKED);
                    msg_send = message_pack(jmsg["cmd"].get<std::int32_t>(),1,0,jmsg["compressFormat"], res);
                }
                break;
            default:
                msg_send = message_pack(0,1,0,jmsg["compressFormat"],"message error~\n");
                break;
        }
        // msg_send = std::string(vec.begin(), vec.end());
        SendData(msg_send, FD_WRITE);
        return;
    } catch (const nlohmann::detail::parse_error& e) {
        RTE_LOG(ERR, USER1, "[%s][%d] parse json error:%s.", __func__, __LINE__, e.what());
    } catch (const nlohmann::json::exception& e) {
    // 捕获其他任何未预料到的异常
        RTE_LOG(ERR, USER1, "[%s][%d] Exception catched:%s.", __func__, __LINE__, e.what());
    }
OnMessageEnd:
    _CleanData();
}

void WsConsumer::OnSend(const std::string& msg, int fd_opt)
{
    std::cout << "OnSend:" << Encrypt::bin2hex(msg) << std::endl;
    if (enqueue_data_single_fd(msg, this->fd, _idx, fd_opt) < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d] Enqueue data Failed: cid:%s,uid:%s,opt:%d",
         __func__, __LINE__, _cid.c_str(), _uid.c_str(), fd_opt);
        enqueue_data_single_fd("", this->fd, _idx, FD_CLOSE);
    }
}

void WsConsumer::Send2Client(const std::string& data, int fd_opt)
{
    if(tgg_get_cli_authorized(this->fd) != AUTH_TYPE_TOKENCHECKED) {
        RTE_LOG(ERR, USER1, "[%s][%d] Send data to client should check Token at first.", __func__, __LINE__);
        return;
    }
    SendData(data, fd_opt);
}

void WsConsumer::_CleanAndClose()
{
    // idx小于0说明已经发送过关闭的消息了
    if(tgg_get_cli_idx(this->fd) >= 0) {
        OnClose();
    }
    _CleanData();
}

void WsConsumer::_CleanData()
{
    CleanBuffer();
    clean_read_data((tgg_read_data*)(this->data));
}


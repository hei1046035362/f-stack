#include "nlohmann/json.hpp"
#include "tgg_bwcomm.h"
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/tgg_struct.h"
#include "tgg_bwcomm.h"
#include "cmd/CmdProcessor.h"
#include "WsConsumer.h"
#include "comm/Encrypt.hpp"
#include "tgg_comm/tgg_bw_cache.h"


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
    if(_idx < 0) {// fd超过了可用范围
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
    if (tgg_get_cli_authorized(this->fd) == AUTH_TYPE_TOKENCHECKED) {
        // 尚未绑定的连接不需要解绑  TOTO 检查绑定过程中失败的是否有清理
        nlohmann::json obj;
        CmdUnBindUid ubuid(this->fd, this->data, obj);
        ubuid.ExecCmd();// 解绑，从hash表中删除连接
    }
    std::string data = "\x88\x02\x03\xe8";// 关闭websocket
    SendONnoAuth(data, FD_WRITE);
    // SendData("", FD_CLOSE);// 关闭fd，这里理论上没有关闭成功也没事，对端也不会再发心跳了，定时器会监控到并强制关闭
}
// 握手
void WsConsumer::OnHandShake(const std::string& response)
{
    OnSend(response, FD_WRITE);// 关闭fd，这里理论上没有关闭成功也没事，对端也不会再发心跳了，定时器会监控到并强制关闭
    tgg_set_cli_authorized(this->fd, AUTH_TYPE_HANDLESHAKED);
    std::string scid = get_valid_cid(this->_idx);
    tgg_set_cli_cid(this->fd, scid.c_str());
    std::string sendData;
    if (message_pack(2, 1, 0, 1, scid, sendData) < 0)
    {
        RTE_LOG(ERR, USER1, "[%s][%d] message_pack cid[%s] failed.\r\n", 
            __func__, __LINE__, scid.c_str());
        _CleanAndClose();
        return;
    }
    SendONnoAuth(sendData, FD_WRITE);
}

void WsConsumer::OnPing(const std::string& response)
{
    std::string result = EncodeWebsocketMessage(PONG_FRAME, response);
    OnSend(result, FD_WRITE);
}

void WsConsumer::OnPong(const std::string& response)
{
}

void WsConsumer::OnMessage(const std::string& msg)
{
    // TODO 不解析消息，直接转发给bw
    if(msg.empty()) {
        RTE_LOG(ERR, USER1, "[%s][%d] msg can't be empty.", __func__, __LINE__);
        goto OnMessageEnd;
    }
    try {
        std::string msg_send;
        std::string message;
        if(message_unpack(msg, message) < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d] message_unpack msg failed,data:%s\r\n", 
                __func__, __LINE__, Encrypt::bin2hex(msg).c_str());
            goto OnMessageEnd;
        }
        nlohmann::json jmsg = nlohmann::json::parse(message);
        int cmd = jmsg["cmd"].get<std::int32_t>();
        int compress = jmsg["compressFormat"].get<std::int32_t>();
        switch(jmsg["cmd"].get<std::int32_t>()) {
            case 0:// 心跳
                if (message_pack(cmd , 1, 1, compress, "", msg_send)) {
                    RTE_LOG(ERR, USER1, "[%s][%d] message_pack msg failed.\r\n", 
                        __func__, __LINE__);
                        goto OnMessageEnd;
                }
                printf("send heart beat:%s\n", Encrypt::bin2hex(msg_send).c_str());
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
                    std::string s_uid = std::to_string(jtoken["user_id"].get<std::uint64_t>());
                    s_uid.resize(20);
                    // TODO uid和cid绑定
                    CmdBindUid buid(this->fd, this->data, jtoken);
                    if(buid.ExecCmd() == -1) {
                        RTE_LOG(ERR, USER1, "[%s][%d] add uid[%s] failed, closing connection...",
                            __func__, __LINE__, s_uid.c_str());
                        _CleanAndClose();
                        return;
                    }
                    // char resArray[32] = {0};
                    std::string res = "bind ";
                    res += s_uid;
                    res += "\n";
                    // sprintf(resArray, "bind %lu\n", jtoken["user_id"].get<std::uint64_t>());
                    // std::string res = std::string(resArray, strlen(resArray));
                    tgg_set_cli_authorized(this->fd, AUTH_TYPE_TOKENCHECKED);
                    if (message_pack(jmsg["cmd"].get<std::int32_t>() , 1 , 0,
                        jmsg["compressFormat"], res, msg_send) < 0) {
                        // 数据封包失败
                        RTE_LOG(ERR, USER1, "[%s][%d] message_unpack msg failed,data:%s\r\n", 
                            __func__, __LINE__, res.c_str());
                        goto OnMessageEnd;
                    }
                }
                break;
            default:
                if (message_pack(0,1,0,jmsg["compressFormat"],"message error~\n", msg_send) < 0) {
                    RTE_LOG(ERR, USER1, "[%s][%d] message_pack msg failed.\r\n", 
                        __func__, __LINE__);
                        goto OnMessageEnd;
                }
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
    // 连接已关闭或尚未建立
    if(tgg_get_cli_idx(this->fd) < 0)
        return;

    std::cout << "OnSend:" << Encrypt::bin2hex(msg) << std::endl;
    if (enqueue_data_single_fd(msg, this->fd, _idx, fd_opt) < 0) {// 函数内部会循环尝试发送10次
        RTE_LOG(ERR, USER1, "[%s][%d] Enqueue data Failed: cid:%s,uid:%s,opt:%d",
         __func__, __LINE__, _cid.c_str(), _uid.c_str(), fd_opt);
        enqueue_data_single_fd("", this->fd, _idx, FD_CLOSE);
    }
}

void WsConsumer::Send2Client(const char* cid, const std::string& data, int fd_opt)
{
    int fd = tgg_get_fdbycid(cid);
    if(fd < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d] client[%s] not exist.", __func__, __LINE__, cid);
        return;
    }
    int idx = tgg_get_cli_idx(fd);
    if(idx < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d] client[%s] already closed.", __func__, __LINE__, cid);
        return;
    }
    if(tgg_get_cli_authorized(fd) != AUTH_TYPE_TOKENCHECKED) {
        RTE_LOG(ERR, USER1, "[%s][%d] Send data to client[%s] should check Token at first.",
            __func__, __LINE__, cid);
        return;
    }
    std::string sendData;
    // 打包封装到
    if (message_pack(2, 1, 0, 1, data, sendData) < 0)
    {
        RTE_LOG(ERR, USER1, "[%s][%d] message_pack data[%s] failed.\r\n", 
            __func__, __LINE__, data.c_str());
        return;
    }

    std::cout << "send data["<< cid <<"]:" << Encrypt::bin2hex(sendData) << std::endl;
    if (enqueue_data_single_fd(sendData, fd, idx, fd_opt) < 0) {// 函数内部会循环尝试发送10次
        RTE_LOG(ERR, USER1, "[%s][%d] Enqueue data Failed: cid:%s,opt:%d",
         __func__, __LINE__, cid, fd_opt);
    }
}

void WsConsumer::BatchSend2Client(std::list<std::string> cids, const std::string& data, int fd_opt)
{
    std::list<int> lstFds;
    std::list<std::string>::iterator itCid = cids.begin();
    while(itCid != cids.end()) {
        int fd = tgg_get_fdbycid((*itCid).c_str());
        if(fd < 0) {
            RTE_LOG(INFO, USER1, "[%s][%d] client[%s] not exist.", __func__, __LINE__, (*itCid).c_str());
            continue;
        }
        lstFds.push_back(fd);
        itCid++;
    }
    WsConsumer::BatchSend2Client(lstFds, data, fd_opt);
}

void WsConsumer::BatchSend2Client(std::list<int> fds, const std::string& data, int fd_opt)
{
    if(fds.size() <= 0) {
        RTE_LOG(ERR, USER1, "[%s][%d] fd list can't be empty.\r\n", 
            __func__, __LINE__);
        return;
    }
    std::map<int, int> mapFdidx;
    std::list<int>::iterator itFd = fds.begin();
    while(itFd != fds.end()) {
        int idx = tgg_get_cli_idx(*itFd);
        if(idx < 0) {
            std::string sCid = tgg_get_cli_cid(*itFd);
            RTE_LOG(INFO, USER1, "[%s][%d] client[%s] already closed.\n", __func__, __LINE__, sCid.c_str());
            itFd++;
            continue;
        }
        if(tgg_get_cli_authorized(*itFd) != AUTH_TYPE_TOKENCHECKED) {
            std::string sCid = tgg_get_cli_cid(*itFd);
            RTE_LOG(INFO, USER1, "[%s][%d] Send data to client[%s] should check Token at first.\n",
                __func__, __LINE__, sCid.c_str());
            itFd++;
            continue;
        }
        mapFdidx[*itFd] = idx;
        itFd++;
    }
    if(mapFdidx.size() <= 0) {
        RTE_LOG(ERR, USER1, "[%s][%d] no live fd found for.\r\n", 
            __func__, __LINE__);
        return;
    }
    std::string sendData;
    // 打包封装
    if (message_pack(2, 1, 0, 1, data, sendData) < 0)
    {
        RTE_LOG(ERR, USER1, "[%s][%d] message_pack data[%s] failed.\r\n", 
            __func__, __LINE__, data.c_str());
        return;
    }
    
    std::cout << "send group data:" << Encrypt::bin2hex(sendData) << std::endl;
    if (enqueue_data_batch_fd(sendData, mapFdidx, fd_opt) < 0) {// 函数内部会循环尝试发送10次
        RTE_LOG(ERR, USER1, "[%s][%d] Batch Enqueue data Failed\n",
         __func__, __LINE__);
    }
}


void WsConsumer::_CleanAndClose()
{
    // idx小于0说明已经发送过关闭的消息了
    OnClose();
    _CleanData();
}

void WsConsumer::_CleanData()
{
    CleanBuffer();
    clean_read_data((tgg_read_data*)(this->data));
}


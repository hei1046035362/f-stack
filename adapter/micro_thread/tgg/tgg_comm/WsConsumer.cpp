#include "nlohmann/json.hpp"
#include "tgg_bwcomm.h"
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/tgg_struct.h"
#include "tgg_bwcomm.h"
// #include "cmd/CmdProcessor.h"
#include "WsConsumer.h"
#include "comm/Encrypt.hpp"
#include "tgg_comm/tgg_bw_cache.h"
#include "tgg_transport.h"
#include "tgg_struct.h"
#include "tgg_comm/tgg_conf.h"

int WsConsumer::ConsumerData(void* data)
{
    tgg_read_data* rdata = (tgg_read_data*)data;
    if (!ConnectionValid(rdata->coreid, rdata->fd, data)) {
        // TODO close fd or just drop data
        _CleanData();
        // _CleanAndClose();
        return 0;
    }
    if (rdata->fd_opt & FD_CLOSE) {
        // unbind bw connection
        _CleanAndClose();
        return 0;
    }else if (rdata->fd_opt & FD_NEW) {
        // bind bw connection
        OnConnect();
        return 0;
    }

    InitWebsocket(rdata->fd, tgg_get_cli_authorized(rdata->coreid, rdata->fd));
    this->fd = rdata->fd;
    this->data = data;
    this->core_id = core_id;
    _cid = tgg_get_cli_cid(core_id, fd);
    _uid = tgg_get_cli_uid(core_id, fd);

    int ret = ReadData(rdata->data, rdata->data_len);
    if (ret < 0) {
        _CleanAndClose();
    } else if (ret > 0) {
        _CleanData();
    }// 等于0属于帧不完整，ws缓存了数据不能清理
    return 0;
}

bool WsConsumer::ConnectionValid(int core_id, int fd, void* data)
{
    _idx = tgg_get_cli_idx(core_id, fd);
    if(_idx < 0) {// fd超过了可用范围
        return false;
    }
    // _status = tgg_get_cli_status(fd);
    // if (_status > FD_STATUS_KEEP) {
    //     // TODO 状态迁移待改进，连接已经关闭了
    //     return false;
    // }
    if (_idx != ((tgg_read_data*)data)->idx) {
        // 说明当前的数据已经是上一个连接的数据了
        RTE_LOG(ERR, USER1, "[%s][%d] client idx[%d] not match to data idx[%d].\n",
           __FILE__, __LINE__, _idx, ((tgg_read_data*)data)->idx);
        return false;
    }
    return true;
}

void WsConsumer::OnClose()
{// 子类继承后要执行clean_buffer清理缓存
    if (tgg_get_cli_authorized(this->core_id, this->fd) == AUTH_TYPE_HANDLESHAKED) {
        // TODO 构造消息让gwbwrcv去解绑还是就在这里解绑？  
        // 当前选择关闭时直接解绑，防止消息丢失导致连接未解绑
        if (tgg_free_session(this->core_id, this->fd) < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d] free session failed, idx[%d].\r\n", 
                __FILE__, __LINE__, tgg_get_cli_cid(this->core_id, this->fd));
        }
        // nlohmann::json obj;
        // CmdUnBindUid ubuid(this->core_id, this->fd, this->data, obj);
        // ubuid.ExecCmd();// 解绑，从hash表中删除连接
    }
    // std::string data = "\x88\x02\x03\xe8\x00\x00";// 关闭websocket
    // OnSend(data, FD_WRITE|FD_CLOSE);
    SendONnoAuth("", FD_WRITE|FD_CLOSE);// TODO FD_CLOSE会强制关闭socket,这种方式欠妥，会报错
    Send2Server(this->core_id, this->fd, "", FD_CLOSE);
    // SendData("", FD_CLOSE);// 关闭fd，这里理论上没有关闭成功也没事，对端也不会再发心跳了，定时器会监控到并强制关闭
}

void WsConsumer::OnConnect()
{
    tgg_set_cli_authorized(this->core_id, this->fd, AUTH_TYPE_CLIENTCONNECT);
    int cid = ((this->core_id << 24) | this->_idx);
    tgg_set_cli_cid(this->core_id, this->fd, cid);
    // std::string ccid = get_valid_ccid(cid);
    this->_cid = cid;
    Send2Server(this->core_id, this->fd, "", FD_NEW);
}

// 检查请求是否符合tgg的要求，不符合直接断开连接
static bool tgg_request_valid_check(const HttpRequest &req, std::string& token, std::string& properties)
{
    // 检查必需的头字段
    std::map<std::string, std::string>::const_iterator ittoken = req.query.find("token");
    std::map<std::string, std::string>::const_iterator itproperties = req.query.find("client_properties");
    if (itproperties == req.query.end() ||
        req.query.find("authorization") == req.query.end() ||
        ittoken == req.query.end()) {
        return false;
    }
    // token 是否能解析出来
    Encrypt encryptor = GetEncryptor();
    token = encryptor.Aes128Decrypt(ittoken->second);
    if (token.empty()) {
        RTE_LOG(ERR, USER1, "[%s][%d] token[%s] decrypted error.\n", __FILE__, __LINE__, ittoken->second.c_str());
        return false;
    }
    RTE_LOG(ERR, USER1, "[%s][%d] OnHandShake ok, token:%s.\n", __FILE__, __LINE__, token.c_str());
    properties = itproperties->second;
    return true;
}
// 封装发送给bw的握手请求数据
static void build_server_data(const HttpRequest &req, const std::string& ip_str, ushort port, nlohmann::json& data) {
    nlohmann::json server_vars;
    server_vars["REQUEST_METHOD"] = req.method;
    server_vars["REQUEST_URI"] = req.uri;
    server_vars["SERVER_PROTOCOL"] = "HTTP/" + req.protocol;
    server_vars["SERVER_NAME"] = req.headers.count("Host") ? 
        req.headers.at("Host") : "unknown";
    server_vars["CONTENT_TYPE"] = req.headers.count("Content-Type") ? 
        req.headers.at("Content-Type") : "";
    // 构建QUERY_STRING原始字符串
    size_t query_pos = req.uri.find('?');
    server_vars["QUERY_STRING"] = (query_pos != std::string::npos) ? 
        req.uri.substr(query_pos + 1) : "";

    server_vars["REMOTE_ADDR"] = ip_str;
    server_vars["REMOTE_PORT"] = port;
    server_vars["SERVER_PORT"] = TggConfigure::getInstance()->get_gateway_port();

    for (const auto& [key, value] : req.headers) {
        std::string upperKey = key;
        std::transform(upperKey.begin(), upperKey.end(), upperKey.begin(), ::toupper);
        std::replace(upperKey.begin(), upperKey.end(), '-', '_');
        server_vars["HTTP_" + upperKey] = value;
    }
    nlohmann::json query_params;
    for (const auto& [key, value] : req.query) {
        query_params[key] = value;
    }
    nlohmann::json cookies;
    for (const auto& [key, value] : req.cookies) {
        cookies[key] = value;
    }
    data["get"] = query_params; // GET 参数（需解析为 map）
    data["server"] = server_vars;
    data["cookie"] = cookies;
    // return server_vars;
}

bool WsConsumer::_CheckToken(const std::string& token)
{
    nlohmann::json jtoken = nlohmann::json::parse(token);
    uint64_t uid = jtoken["user_id"].get<std::uint64_t>();
    std::string userid = std::to_string(uid);
    if(uid == 0 || userid.length() >= TGG_UID_LEN) {
        RTE_LOG(ERR, USER1, "[%s][%d] invalid uid[%s] failed.\r\n", 
            __FILE__, __LINE__, userid.c_str());
        return false;
    }
    // TODO  直接拿token里面的uid还是等bw发送bind消息再赋值？销毁连接时会去查询
    // tgg_set_cli_uid(this->core_id, this->fd, userid.c_str());
    return true;
}

// 握手
void WsConsumer::OnHandShake(const std::string& response, HttpRequest& req)
{
    std::string token, properties;
    if(!tgg_request_valid_check(req, token, properties)) {
        RTE_LOG(ERR, USER1, "[%s][%d] tgg ws request[%s] check failed.\r\n", 
            __FILE__, __LINE__, req.uri.c_str());
        _CleanAndClose();
        return;
    }
    if(!_CheckToken(token)) {
        RTE_LOG(ERR, USER1, "[%s][%d] check token[%s] failed.\r\n", 
            __FILE__, __LINE__, token.c_str());
        _CleanAndClose();
        return;
    }
    nlohmann::json data;
    std::string ip_str = tgg_get_cli_ip_str(this->core_id, this->fd);
    ushort port = tgg_get_cli_port(this->core_id, this->fd);
    build_server_data(req, ip_str, port, data);
    tgg_set_cli_authorized(this->core_id, this->fd, AUTH_TYPE_HANDLESHAKED);
    // TODO 这里是直接发送给服务端还是自己处理？
    // std::string sendData;
    // if (message_pack(2, 1, 0, 1, ccid, sendData) < 0)
    // {
    //     RTE_LOG(ERR, USER1, "[%s][%d] message_pack cid[%d] failed.\r\n", 
    //         __FILE__, __LINE__, cid);
    //     _CleanAndClose();
    //     return;
    // }
    // SendONnoAuth(sendData, FD_WRITE);
    OnSend(response, FD_WRITE);// 响应客户端的http请求
    std::string result = data.dump();
    RTE_LOG(ERR, USER1, "[%s][%d] OnHandShake:%s.\r\n", 
        __FILE__, __LINE__, result.c_str());

    Send2Server(this->core_id, this->fd, result, FD_HANDLESHAKE);// 通知服务端websocket 握手完成
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
    Send2Server(this->core_id, this->fd, msg, FD_WRITE);




// // TODO 不解析消息，直接转发给bw
//     if(msg.empty()) {
//         RTE_LOG(ERR, USER1, "[%s][%d] msg can't be empty.", __FILE__, __LINE__);
//         goto OnMessageEnd;
//     }
//     try {
//         std::string msg_send;
//         std::string message;
//         if(message_unpack(msg, message) < 0) {
//             RTE_LOG(ERR, USER1, "[%s][%d] message_unpack msg failed,data:%s\r\n", 
//                 __FILE__, __LINE__, Encrypt::bin2hex(msg).c_str());
//             goto OnMessageEnd;
//         }
//         nlohmann::json jmsg = nlohmann::json::parse(message);
//         int cmd = jmsg["cmd"].get<std::int32_t>();
//         int compress = jmsg["compressFormat"].get<std::int32_t>();
//         printf("transport msg:%s\n", message.c_str());
//         switch(jmsg["cmd"].get<std::int32_t>()) {
//             case 0:// 心跳
//                 if (message_pack(cmd , 1, 1, compress, "", msg_send)) {
//                     RTE_LOG(ERR, USER1, "[%s][%d] message_pack msg failed.\r\n", 
//                         __FILE__, __LINE__);
//                         goto OnMessageEnd;
//                 }
//                 printf("send heart beat:%s\n", Encrypt::bin2hex(msg_send).c_str());
//                 break;
//             case 1:// 通信消息
//                 {
//                     printf("nomal msg\n");
//                     nlohmann::json jbody = nlohmann::json::parse(jmsg["body"].get<std::string>());
//                     std::string token = jbody["token"];
//                     if (token.empty()) {
//                         RTE_LOG(ERR, USER1, "[%s][%d] token can't be empty.", __FILE__, __LINE__);
//                         goto OnMessageEnd;
//                     }
//                     Encrypt encryptor = GetEncryptor();
//                     std::string decryptor = encryptor.Aes128Decrypt(token);
//                     if (decryptor.empty()) {
//                         RTE_LOG(ERR, USER1, "[%s][%d] token decrypted error.", __FILE__, __LINE__);
//                         goto OnMessageEnd;
//                     }
//                     nlohmann::json jtoken = nlohmann::json::parse(decryptor);
//                     std::string s_uid = std::to_string(jtoken["user_id"].get<std::uint64_t>());
//                     s_uid.resize(20);
//                     TODO uid和cid绑定
//                     CmdBindUid buid(this->fd, this->data, jtoken);
//                     if(buid.ExecCmd() == -1) {
//                         RTE_LOG(ERR, USER1, "[%s][%d] add uid[%s] failed, closing connection...",
//                             __FILE__, __LINE__, s_uid.c_str());
//                         _CleanAndClose();
//                         return;
//                     }
//                     char resArray[32] = {0};
//                     std::string res = "bind ";
//                     res += s_uid;
//                     res += "\n";
//                     // sprintf(resArray, "bind %lu\n", jtoken["user_id"].get<std::uint64_t>());
//                     // // std::string res = std::string(resArray, strlen(resArray));
//                     tgg_set_cli_authorized(this->fd, AUTH_TYPE_TOKENCHECKED);
//                     if (message_pack(jmsg["cmd"].get<std::int32_t>() , 1 , 0,
//                         jmsg["compressFormat"], res, msg_send) < 0) {
//                         // 数据封包失败
//                         RTE_LOG(ERR, USER1, "[%s][%d] message_unpack msg failed,data:%s\r\n", 
//                             __FILE__, __LINE__, res.c_str());
//                         goto OnMessageEnd;
//                     }
//                 }
//                 break;
//             default:
//                 if (message_pack(0,1,0,jmsg["compressFormat"],"message error~\n", msg_send) < 0) {
//                     RTE_LOG(ERR, USER1, "[%s][%d] message_pack msg failed.\r\n", 
//                         __FILE__, __LINE__);
//                         goto OnMessageEnd;
//                 }
//                 break;
//         }
//         msg_send = std::string(vec.begin(), vec.end());
//         SendData(msg_send, FD_WRITE);
//         return;
//     } catch (const nlohmann::detail::parse_error& e) {
//         RTE_LOG(ERR, USER1, "[%s][%d] parse json error:%s.", __FILE__, __LINE__, e.what());
//     } catch (const nlohmann::json::exception& e) {
//     // 捕获其他任何未预料到的异常
//         RTE_LOG(ERR, USER1, "[%s][%d] Exception catched:%s.", __FILE__, __LINE__, e.what());
//     }
// OnMessageEnd:
//     _CleanData();
}

void WsConsumer::OnSend(const std::string& msg, int fd_opt)
{
    // 连接已关闭或尚未建立
    if(tgg_get_cli_idx(this->core_id, this->fd) < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d] Send data Failed, connection invalid: cid:%d,uid:%s,opt:%d",
            __FILE__, __LINE__, _cid, _uid.c_str(), fd_opt);        
        return;
    }

    std::cout << "OnSend:" << Encrypt::bin2hex(msg) << std::endl;
    if (enqueue_data_single_fd(this->core_id, msg, this->fd, _idx, fd_opt) < 0) {// 函数内部会循环尝试发送10次
        RTE_LOG(ERR, USER1, "[%s][%d] Enqueue data Failed: cid:%d,uid:%s,opt:%d",
         __FILE__, __LINE__, _cid, _uid.c_str(), fd_opt);
        enqueue_data_single_fd(this->core_id, "", this->fd, _idx, FD_CLOSE);
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
    if(this->data) {// 防止可能还没有给this->data赋值，连接就已经关闭了
        clean_read_data((tgg_read_data*)(this->data));
    }
}


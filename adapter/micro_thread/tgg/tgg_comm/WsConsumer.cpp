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
#include "comm/common.hpp"
#include "comm/log.hpp"
#include "mt_api.h"

extern struct rte_mempool* g_mempool_bwrcv;
extern struct rte_mempool* g_mempool_bwrcv_data;
extern struct rte_mempool* g_mempool_trans;
extern struct rte_mempool* g_mempool_trans_data;


tgg_trans_data* format_send_server_data(int core_id, int fd, const std::string& sdata, int fdopt)
{
    if(fd <= 0) {
        LOG_ERROR("invalid fd:%d.", fd);
        return NULL;
    }
    tgg_trans_data* tdata = NULL;
    int ret = high_freq_malloc(g_mempool_trans, (void**)&tdata, sizeof(tgg_bw_data));
        // TODO  建议增加循环处理，内存池不够，可以稍微等待消费端释放
    if (ret < 0) {
        LOG_ERROR("get mem from bwrcv pool failed,code:%d.", ret);
        return NULL;
    }
    if(sdata.size() > 0) {
        ret = high_freq_malloc(g_mempool_trans_data, &tdata->data, sdata.size());
        if (ret < 0) {
            high_freq_free(g_mempool_trans, (void*)tdata, sizeof(tgg_trans_data));
            LOG_ERROR("get mem from bwrcv data pool failed,code:%d.", ret);
            return NULL;
        }
        // bwdata->data = dpdk_rte_malloc(sdata.size());
        memcpy(tdata->data, sdata.c_str(), sdata.size());
    } else {
        tdata->data = NULL;
    }
    tdata->data_len = sdata.size();
    tdata->fd_opt = fdopt;
    tdata->fd = fd;
    tdata->coreid = core_id;
    tdata->peer_ip = (unsigned int)tgg_get_cli_ip(core_id, fd);
    tdata->peer_port = (unsigned int)tgg_get_cli_port(core_id, fd);
    tdata->idx = (unsigned int)tgg_get_cli_idx(core_id, fd);
    return tdata;
}

int enqueue_data_trans(int core_id, int fd, const std::string& data, int fdopt)
{
    tgg_trans_data* tdata = format_send_server_data(core_id, fd, data, fdopt);
    if (!tdata) {
        LOG_ERROR("Format bw server data failed.");
        return -1;
    }
    int maxtry = 10;// 入队列可能会失败最多尝试10次
    int ret = tgg_enqueue_trans(tdata);
    while (ret < 0 && maxtry > 0 ) {
        NS_MICRO_THREAD::mt_sleep(10);
        ret = tgg_enqueue_trans(tdata);
        maxtry--;
    }
    static int loop_times_sndserver = 0;
    // TODO 前期调试要看是否经常出现重试
    if (maxtry < 10) {
        if(loop_times_sndserver++ % 100 == 0) {
            LOG_ERROR("loop times:%d.", loop_times_sndserver);
        }
    }
    if (ret < 0) {
        clean_trans_data(tdata);
        LOG_ERROR("Enqueue bw server data failed.");
        return -1;
    }
    return 0;
}

static int s_enqueued_to_server_count = 0;
int WsConsumer::_Send2Server(const std::string& data, int fd_opt)
{
    if(tgg_get_bwfdx_count() <= 0) {
        LOG_ERROR("Send data to server Failed: no bw found.");
        return NO_BW_AVALIABLE;
    }
    if (enqueue_data_trans(this->core_id, this->fd, data, fd_opt) < 0) {// 函数内部会循环尝试发送10次
        LOG_ERROR("Send data to server Failed,[core:%d][fd:%d] current count:%d.",
         core_id, fd, s_enqueued_to_server_count);
        return SEND_FAILED;
    }
    s_enqueued_to_server_count++;
    LOG_DEBUG("send to server:%s.", bin2hex(data).c_str());
    return SEND_SUCCESS;
}

int WsConsumer::ConsumerData(void* data)
{
    tgg_read_data* rdata = (tgg_read_data*)data;
    if (!ConnectionValid(rdata->coreid, rdata->fd, data)) {
        // TODO close fd or just drop data
        _CleanData();
        // _CleanAndClose();
        return -1;
    }
    this->fd = rdata->fd;
    this->data = data;
    this->core_id = rdata->coreid;
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
    // _cid = tgg_get_cli_cid(core_id, fd);
    // _uid = tgg_get_cli_uid(core_id, fd);

    int ret = ReadData(rdata->data, rdata->data_len);
    if (ret < 0) {
        _CleanAndClose();
        return -1;
    } else if (ret > 0) {
        _CleanData();
    }// 等于0属于帧不完整，ws缓存了数据不能清理
    return 0;
}

bool WsConsumer::ConnectionValid(int core_id, int fd, void* data)
{
    _idx = tgg_get_cli_idx(core_id, fd);
    if(_idx < 0 || fd <= 0) {// fd超过了可用范围
        LOG_ERROR("invalid idx[%d] or invalid fd[%d].", _idx, fd);
        return false;
    }
    if (_idx != ((tgg_read_data*)data)->idx) {
        // 说明当前的数据已经是上一个连接的数据了
        LOG_ERROR("client idx[%d] not match to data idx[%d].", _idx, ((tgg_read_data*)data)->idx);
        return false;
    }
    return true;
}

void WsConsumer::OnClose()
{// 子类继承后要执行clean_buffer清理缓存
    // std::string data = "\x88\x02\x03\xe8\x00\x00";// 关闭websocket
    // OnSend(data, FD_WRITE|FD_CLOSE);
    _status = FD_STATUS_CLOSING;
    _Send2Server("", FD_CLOSE);
    SendONnoAuth("", FD_WRITE|FD_CLOSE);// TODO FD_CLOSE会强制关闭socket,这种方式欠妥，会报错
    // SendData("", FD_CLOSE);// 关闭fd，这里理论上没有关闭成功也没事，对端也不会再发心跳了，定时器会监控到并强制关闭
}

void WsConsumer::OnConnect()
{
    tgg_set_cli_authorized(this->core_id, this->fd, AUTH_TYPE_CLIENTCONNECT);
    // int cid = (this->core_id  | this->_idx << 8);
    // tgg_set_cli_cid(this->core_id, this->fd, cid);
    // this->_cid = cid;
    if(_Send2Server("", FD_NEW) == NO_BW_AVALIABLE) {
        SendONnoAuth("", FD_WRITE|FD_CLOSE);// TODO FD_CLOSE会强制关闭socket,这种方式欠妥，会报错
    }
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
        LOG_ERROR("token[%s] decrypted error.", ittoken->second.c_str());
        return false;
    }
    LOG_INFO("OnHandShake ok, token:%s.\n", token.c_str());
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
        LOG_ERROR("invalid uid[%s] failed.", userid.c_str());
        return false;
    }
    // TODO  直接拿token里面的uid还是等bw发送bind消息再赋值？销毁连接时会去查询
    // tgg_set_cli_uid(this->core_id, this->fd, userid.c_str());
    return true;
}

// 握手
void WsConsumer::OnHandShake(const std::string& response, struct HttpRequest& req)
{
    std::string token, properties;
    if(!tgg_request_valid_check(req, token, properties)) {
        LOG_ERROR("tgg ws request[%s] check failed.", req.uri.c_str());
        _CleanAndClose();
        return;
    }
    if(!_CheckToken(token)) {
        LOG_ERROR("check token[%s] failed.", token.c_str());
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
    LOG_INFO("OnHandShake:%s.", result.c_str());
    // 通知服务端websocket 握手完成
    if (_Send2Server(result, FD_HANDLESHAKE) == NO_BW_AVALIABLE) {
        SendONnoAuth("", FD_WRITE|FD_CLOSE);// TODO FD_CLOSE会强制关闭socket,这种方式欠妥，会报错
    }
}

void WsConsumer::OnPing(const std::string& response)
{
    std::string result = std::move(EncodeWebsocketMessage(PONG_FRAME, response));
    OnSend(result, FD_WRITE);
    // 不单独开线程去发送ping了，收到ping之后发送完pong后即刻发送ping，在Onpong中检测是否超时
    std::string ping = std::move(EncodeWebsocketMessage(PING_FRAME, response));
    OnSend(ping, FD_WRITE);
}

void WsConsumer::OnPong(const std::string& response)
{
    // 在这里可以获取主动检测结果
    // printf("recieve pong:%s\r\n", response.c_str());
}

void WsConsumer::OnMessage(const std::string& msg)
{
    int cli_status = tgg_get_cli_authorized(this->core_id, this->fd);
    if(cli_status != AUTH_TYPE_HANDLESHAKED) {
        LOG_ERROR("cli[coreid:%d fd:%d] status[%d] is not handleshaked, msg[%s] droped.", this->core_id, this->fd, cli_status, msg.c_str());
        return;
    }
    if(_Send2Server(msg, FD_WRITE) == NO_BW_AVALIABLE) {
        SendONnoAuth("", FD_WRITE|FD_CLOSE);// TODO FD_CLOSE会强制关闭socket,这种方式欠妥，会报错
    }
}

void WsConsumer::OnSend(const std::string& msg, int fd_opt)
{
    #if 0
    // 连接已关闭或尚未建立
    if(tgg_get_cli_idx(this->core_id, this->fd) < 0) {
        LOG_ERROR("Send data Failed, connection invalid: cid:%d,uid:%s,opt:%d.", _cid, _uid.c_str(), fd_opt);        
        return;
    }

    LOG_DEBUG("OnSend to client fd[%d] idx[%d]: %s", this->fd, _idx, bin2hex(msg).c_str());
    if (enqueue_data_single_fd(this->core_id, msg, this->fd, _idx, fd_opt) < 0) {// 函数内部会循环尝试发送10次
        LOG_ERROR("Enqueue data Failed: cid:%d,uid:%s,opt:%d.", _cid, _uid.c_str(), fd_opt);
        enqueue_data_single_fd(this->core_id, "", this->fd, _idx, FD_WRITE|FD_CLOSE);
    }
    #endif
    // if(fd_opt & FD_CLOSE) {
    //     tgg_set_cli_idx(g_core_id, cli_fd, TGG_FD_CLOSING);// 设置关闭标记，表明不会再发送数据,直接关闭连接，后续的数据直接丢弃
    // }
    int ret = NS_MICRO_THREAD::mt_send(this->fd, msg.c_str(), msg.size(), 0, 1000);
    if (ret == -4) {
        // 主动断开连接
        LOG_INFO("closing connection affected.");
    } else if (ret < 0) {
        LOG_ERROR("send data to client fd[%d] idx[%d] error, ret[%d]", this->fd, _idx, ret);
    }
    LOG_DEBUG("OnSend to client fd[%d] idx[%d]: %s", this->fd, _idx, bin2hex(msg).c_str());
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
    // 原始数据由外部释放
    // if(this->data) {// 防止可能还没有给this->data赋值，连接就已经关闭了
    //     clean_read_data((tgg_read_data*)(this->data));
    // }
}


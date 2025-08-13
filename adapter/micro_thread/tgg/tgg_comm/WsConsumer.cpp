#include "tgg_bwcomm.h"
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/tgg_struct.h"
#include "tgg_bwcomm.h"
#include "WsConsumer.h"
#include "comm/Encrypt.hpp"
#include "tgg_comm/tgg_bw_cache.h"
#include "tgg_transport.h"
#include "tgg_struct.h"
#include "tgg_comm/tgg_conf.h"
#include "comm/common.hpp"
#include "comm/log.hpp"
#include "mt_api.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

extern struct rte_mempool* g_mempool_trans;
extern struct rte_mempool* g_mempool_trans_data;


tgg_trans_data* format_send_server_data(int core_id, int fd, std::string_view sdata, int fdopt)
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
        memcpy(tdata->data, sdata.data(), sdata.size());
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

int enqueue_data_trans(int core_id, int fd, std::string_view data, int fdopt)
{
    tgg_trans_data* tdata = format_send_server_data(core_id, fd, data, fdopt);
    if (!tdata) {
        LOG_ERROR("Format bw server data failed.");
        return -1;
    }
    int maxtry = 10;// 入队列可能会失败最多尝试10次
    if(tdata->fd_opt & FD_CLOSE) {
        maxtry = 1000;// 关闭命令必须要发送过去，但是又不能造成死循环，所以这里直接把失败尝试次数提高
    }
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
int WsConsumer::_Send2Server(std::string_view data, int fd_opt)
{
    return SEND_SUCCESS;
    // 在透传中判断，这里不需要去管业务侧是否在线，只管上传
    // if(tgg_get_bwfdx_count() <= 0) {
    //     LOG_ERROR("Send data to server Failed: no bw found.");
    //     return NO_BW_AVALIABLE;
    // }
    if (enqueue_data_trans(this->core_id, this->fd, data, fd_opt) < 0) {// 函数内部会循环尝试发送10次
        LOG_ERROR("Send data to server Failed,[core:%d][fd:%d] current count:%d.",
         core_id, fd, s_enqueued_to_server_count);
        return SEND_FAILED;
    }
    s_enqueued_to_server_count++;
    // LOG_DEBUG("send to server:%s.", bin2hex(data).c_str());
    return SEND_SUCCESS;
}

int WsConsumer::ConsumerData(void* data)
{
    tgg_read_data* rdata = (tgg_read_data*)data;
    if (!ConnectionValid(rdata->coreid, rdata->fd, data)) {
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

    int ret = ReadData(rdata->data, rdata->data_len);
    if (ret < 0) {
        _CleanAndClose();
        return -1;
    }
    // 等于0属于帧不完整，ws缓存了数据不能清理
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
{
    tgg_set_cli_status(core_id, fd, FD_STATUS_CLOSING);
    _Send2Server("", FD_CLOSE);
    SendONnoAuth("", FD_WRITE|FD_CLOSE);// TODO FD_CLOSE会强制关闭socket,这种方式欠妥，会报错
}

void WsConsumer::OnConnect()
{
    // tgg_set_cli_authorized(this->core_id, this->fd, AUTH_TYPE_CLIENTCONNECT);
    // if(_Send2Server("", FD_NEW) == NO_BW_AVALIABLE) {
    //     SendONnoAuth("", FD_WRITE|FD_CLOSE);// TODO FD_CLOSE会强制关闭socket,这种方式欠妥，会报错
    // }
}

// 检查请求是否符合tgg的要求，不符合直接断开连接
static bool tgg_request_valid_check(const HttpRequest &req, std::string& properties)
{
    // 检查必需的头字段
    std::map<std::string, std::string>::const_iterator ittoken = req.query.find("token");
    std::map<std::string, std::string>::const_iterator itproperties = req.query.find("client_properties");
    if (itproperties == req.query.end() ||
        ittoken == req.query.end()) {
        LOG_ERROR("token[%s] or properties[%s] can be empty.", ittoken->second.c_str(), itproperties->second.c_str());
        return false;
    }
    // token 是否能解析出来
    // Encrypt encryptor = GetEncryptor();
    // token = encryptor.Aes128Decrypt(ittoken->second);
    // if (token.empty()) {
    //     LOG_ERROR("token[%s] decrypted error.", ittoken->second.c_str());
    //     return false;
    // }
    LOG_INFO("OnHandShake ok.\n");
    properties = itproperties->second;
    return true;
}

bool WsConsumer::_CheckToken(const std::string& token)
{
    rapidjson::Document jtoken;
    jtoken.Parse(token.c_str());
    if (jtoken.HasParseError()) {
        LOG_ERROR("_CheckToken: JSON parse error");
        return false;
    }
    if (!jtoken.HasMember("user_id")) {
        LOG_ERROR("_CheckToken: no user_id found.");
        return false;
    }
    uint64_t uid = jtoken["user_id"].GetUint64();
    std::string userid = std::to_string(uid);
    if(uid == 0 || userid.length() >= TGG_UID_LEN) {
        LOG_ERROR("invalid uid[%s] failed.", userid.c_str());
        return false;
    }
    return true;
}

// 握手
void WsConsumer::OnHandShake(std::string_view request, const std::string& response, struct HttpRequest& req)
{
    // std::string properties;
    // if(!tgg_request_valid_check(req, properties)) {
    //     LOG_ERROR("tgg ws request[%s] check failed.", req.uri.c_str());
    //     _CleanAndClose();
    //     return;
    // }
    // if(!_CheckToken(token)) {
    //     LOG_ERROR("check token[%s] failed.", token.c_str());
    //     _CleanAndClose();
    //     return;
    // }
    // nlohmann::json data;
    // std::string ip_str = tgg_get_cli_ip_str(this->core_id, this->fd);
    // ushort port = tgg_get_cli_port(this->core_id, this->fd);
    // std::string result = build_server_data(req, ip_str, port);
    tgg_set_cli_authorized(this->core_id, this->fd, AUTH_TYPE_HANDLESHAKED);
    OnSend(response, FD_WRITE);// 响应客户端的http请求
    // 通知服务端websocket 握手完成
    LOG_INFO("OnHandShake:%s.", request.data());
    // if(_Send2Server(result, FD_HANDLESHAKE) == NO_BW_AVALIABLE) {
    //     SendONnoAuth("", FD_WRITE|FD_CLOSE);// TODO FD_CLOSE会强制关闭socket,这种方式欠妥，会报错
    // }
    // 通知服务端连接已建立  这里的连接是业务侧连接校验完成，  原本是要发完205之后要再发送一个1，现在只发1了
    if (_Send2Server(request, FD_NEW) == NO_BW_AVALIABLE) {
        SendONnoAuth("", FD_WRITE|FD_CLOSE);// TODO FD_CLOSE会强制关闭socket,这种方式欠妥，会报错
    }
}

void WsConsumer::OnPing(const std::string& response)
{
    std::string result = EncodeWebsocketMessage(PONG_FRAME, response);
    OnSend(result, FD_WRITE);
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
    if((tgg_get_cli_status(this->core_id, this->fd) & FD_STATUS_DISCONNECTED)) {
        LOG_WARNING("send faild, connection is off");
        return;
    }
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
}


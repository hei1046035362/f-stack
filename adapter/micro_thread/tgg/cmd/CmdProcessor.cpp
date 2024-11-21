#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "tgg_comm/WsConsumer.h"
#include <rte_log.h>
#include "tgg_comm/tgg_bwcomm.h"
#include "GatewayProtocal.h"
#include "nlohmann/json.hpp"
#include "CmdProcessor.h"
#include "comm/Encrypt.hpp"

static int s_compress_flag = 0;
static int s_is_open_binary = 0;

int CmdWorkerConnect::ExecCmd()
{
    tgg_bw_info* bwinfo = lookup_bwinfo(this->fd);
    if (!(bwinfo->status & FD_STATUS_NEWSESSION) || (bwinfo->status & FD_STATUS_CLOSING)) {
        close(bwinfo->fd);
        memset(bwinfo, 0, sizeof(tgg_bw_info));
        clean_bw_data((tgg_bw_data*)this->data);
        this->data = NULL;
        return -1;
    }
    bwinfo->status |= FD_STATUS_CONNECTED;
    bwinfo->authorized = true;
    // nlohmann::json worker_info = nlohmann::json::parse(jdata["body"]);
    // if ($worker_info['secret_key'] != bwinfo->secretKey) {
    //     RTE_LOG(ERR, USER1, "[%s][%d] Gateway: Worker key[%s] does not match conn key[%s].", 
    //         __func__, __LINE__, worker_info["secretKey"].c_str(), bwinfo->secretKey));
    //     return;
    // }
    return 0;

}


int CmdSendToGroup::ExecCmd()
{
    // nlohmann::json obj = nlohmann::json::parse(jdata);
    
    // int raw = std::stoi(obj["flag"].get<std::string>()) & FLAG_NOT_CALL_ENCODE;
    // std::string body = obj["body"].get<std::string>();

    // if (!raw && $this->protocolAccelerate && $this->protocol) {
    //     $body = $this->preEncodeForClient($body);
    //     $raw = true;
    // }
    // $ext_data = json_decode($data['ext_data'], true);
    // $group_array = $ext_data['group'];
    // $exclude_connection_id = $ext_data['exclude'];

    // foreach ($group_array as $group) {
    //     if (!empty($this->_groupConnections[$group])) {
    //         foreach ($this->_groupConnections[$group] as $connection) {
    //             if(!isset($exclude_connection_id[$connection->id]))
    //             {
    //                             /** @var TcpConnection $connection */
    //                 $connection->send($body, $raw);
    //             }
    //         }
    //     }
    // }
    return -1;
}




static std::string format_data(const std::string& jdata)
{
    std::string result;
    try {
        nlohmann::json obj = nlohmann::json::parse(jdata);
        if (!obj["cmd"].get<std::string>().empty()) {
            if (s_is_open_binary) {
                result = obj["data"].get<std::string>();
            } else {
                std::string bin = Encrypt::hex2bin(obj["data"].get<std::string>());
                if (bin.length() <= 0) {
                    return result;
                }
                result = message_pack(obj["cmd"], 1, 2,
                    (uint8_t)s_compress_flag, bin);
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
        RTE_LOG(ERR, USER1, "[%s][%d] parse json error:%s", __func__, __LINE__, e.what());
    }
    return result;
}

CmdBaseProcessor* get_cmd_processor(int fd, void* data)
{
    //std::string json_str = R"({"name": "Jane Smith", "age": 25, "is_student": true})";
    tgg_bw_data* bdata = (tgg_bw_data*)data;
    nlohmann::json jdata = nlohmann::json::parse((char*)bdata->data);
    int cmd = std::stoi(jdata["cmd"].get<std::string>());
    tgg_bw_info* binfo = lookup_bwinfo(fd); 
    if (!binfo->authorized && cmd != CMD_WORKER_CONNECT && 
        cmd != CMD_GATEWAY_CLIENT_CONNECT) {
        close(fd);
        RTE_LOG(ERR, USER1, "[%s][%d] command[%d] error or not authorized[%d].", 
            __func__, __LINE__, cmd, binfo->authorized);
        return NULL;
    }
    std::string pack_data = format_data(jdata["data"].get<std::string>());
    if (pack_data.empty()) {
        return NULL;
    } 
    switch(cmd) {
        case CMD_WORKER_CONNECT:
            return new CmdWorkerConnect(fd, data, jdata);
        // GatewayClient连接Gateway
        case CMD_GATEWAY_CLIENT_CONNECT:
            return new CmdGatewayClientConnect(fd, data, jdata);
        // 向某客户端发送数据
        case CMD_SEND_TO_ONE:
            return new CmdSendToOne(fd, data, jdata);
        // 踢出用户
        case CMD_KICK:
            return new CmdKick(fd, data, jdata);
        // 立即销毁用户连接
        case CMD_DESTROY:
            return new CmdDestroy(fd, data, jdata);
        // 广播
        case CMD_SEND_TO_ALL:
            return new CmdSendToALL(fd, data, jdata);
        case CMD_SELECT:
            return new CmdSelect(fd, data, jdata);
        // 获取在线群组列表
        case CMD_GET_GROUP_ID_LIST:
            return new CmdGetGroupIdList(fd, data, jdata);
        // 重新赋值 session
        case CMD_SET_SESSION:
            return new CmdSetSession(fd, data, jdata);
        // session合并
        case CMD_UPDATE_SESSION:
            return new CmdUpdateSession(fd, data, jdata);
        case CMD_GET_SESSION_BY_CLIENT_ID:
            return new CmdGetSessionByCid(fd, data, jdata);
        // 获得客户端sessions
        case CMD_GET_ALL_CLIENT_SESSIONS:
            return new CmdGetAllClientSession(fd, data, jdata);
        // 判断某个 client_id 是否在线
        case CMD_IS_ONLINE:
            return new CmdIsOnline(fd, data, jdata);
        // 将 client_id 与 uid 绑定
        case CMD_BIND_UID:
            return new CmdBindUid(fd, data, jdata);
        // client_id 与 uid 解绑
        case CMD_UNBIND_UID:
            return new CmdUnBindUid(fd, data, jdata);
        // 发送数据给 uid
        case CMD_SEND_TO_UID:
            return new CmdSendToUid(fd, data, jdata);
        // 将 $client_id 加入用户组
        case CMD_JOIN_GROUP:
            return new CmdJoinGroup(fd, data, jdata);
        // 将 $client_id 从某个用户组中移除
        case CMD_LEAVE_GROUP:
            return new CmdLeaveGroup(fd, data, jdata);
        // 解散分组
        case CMD_UNGROUP:
            return new CmdUnGroup(fd, data, jdata);
        // 向某个用户组发送消息
        case CMD_SEND_TO_GROUP:
            return new CmdSendToGroup(fd, data, jdata);
        // 获取某用户组成员信息
        case CMD_GET_CLIENT_SESSIONS_BY_GROUP:
            return new CmdGetClientSessionsByGroup(fd, data, jdata);
        // 获取用户组成员数
        case CMD_GET_CLIENT_COUNT_BY_GROUP:
            return new CmdGetClientCountByGroup(fd, data, jdata);
        // 获取与某个 uid 绑定的所有 client_id
        case CMD_GET_CLIENT_ID_BY_UID:
            return new CmdGetClientIdByUid(fd, data, jdata);
        // 批量获取与 uid 绑定的所有 client_id
        case CMD_BATCH_GET_CLIENT_ID_BY_UID:
            return new CmdBatchGetClientIdByUid(fd, data, jdata);
        // 批量获取群组ID内客户端个数
        case CMD_BATCH_GET_CLIENT_COUNT_BY_GROUP:
            return new CmdBatchGetClientCountByGroup(fd, data, jdata);
        default :
            RTE_LOG(ERR, USER1, "[%s][%d] Gateway inner pack err, Unknown cmd=%d", __func__, __LINE__, cmd);
    }
    return NULL;
}

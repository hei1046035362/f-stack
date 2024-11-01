#include "CmdBaseProcessor.hpp"
#include <rte_log.h>
#include "tgg_bwcomm.h"
static int s_compress_flag = 0;
static int s_is_open_binary = 0;

int CmdWorkerConnect::ExecCmd()
{
    tgg_bw_info* bwinfo = lookup_bwinfo(this->fd);
    if (!(bwinfo->status & FD_STATUS_NEWSESSION) || (bwinfo->status & FD_STATUS_CLOSING)) {
        close(bwinfo->fd)
        memset(*bwinfo, 0, sizeof(tgg_bw_info));
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
    int raw = jdata['flag'] & FLAG_NOT_CALL_ENCODE;
    std::string body = $data['body'];
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




static int format_data(std::string& jdata)
{
    try {
        nlohmann::json obj = nlohmann::json::parse(jdata["body"]);
        if (obj["cmd"])) {
            if (s_is_open_binary) {
                jdata = obj["data"];
            } else {
                std::string bin = hex2bin(obj["data"]);
                if (bin.length() <= 0) {
                    return -1;
                }
                std::vector<uint_8> vec = message_pack(obj["cmd"], 1, 2,
                    s_compress_flag, bin);
                jdata = std::string(vec.begin(), vec.end());
            }
        }
    } catch (const json::parse_error& e) {
        RTE_LOG(ERR, USER1, "[%s][%d] parse json error:%s", __func__, __LINE__, e.what());
        return -1;
    }
    return 0;
}

CmdBaseProcessor* get_cmd_processor(int fd, void* data)
{
    //std::string json_str = R"({"name": "Jane Smith", "age": 25, "is_student": true})";
    tgg_bw_data* bdata = (tgg_bw_data*)data;
    nlohmann::json jdata = nlohmann::json::parse((char*)bdata->data);
    GatewayProtocal cmd = jdata["cmd"];
    tgg_bw_info* binfo = lookup_bwinfo(fd); 
    if (!binfo->authorized && cmd != CMD_WORKER_CONNECT && 
        cmd != CMD_GATEWAY_CLIENT_CONNECT) {
        close(fd);
        RTE_LOG(ERR, USER1, "[%s][%d] command[%d] error or not authorized[%d].", 
            __func__, __LINE__, cmd, binfo->authorized);
        return NULL;
    }
    if (format_data(jdata["data"]) < 0) {
        return NULL;
    } 
    switch(cmd) {
            case GatewayProtocol::CMD_WORKER_CONNECT:
                return new(CmdWorkerConnect(fd, data, jdata));
            // GatewayClient连接Gateway
            case GatewayProtocol::CMD_GATEWAY_CLIENT_CONNECT:
                return new(CmdGatewayClientConnect(fd, data, jdata));
            // 向某客户端发送数据
            case GatewayProtocol::CMD_SEND_TO_ONE:
                return new(CmdSendToOne(fd, data, jdata));
            // 踢出用户
            case GatewayProtocol::CMD_KICK:
                return new(CmdKick(fd, data, jdata));
            // 立即销毁用户连接
            case GatewayProtocol::CMD_DESTROY:
                return new(CmdDestroy(fd, data, jdata));
            // 广播
            case GatewayProtocol::CMD_SEND_TO_ALL:
                return new(CmdSendToALL(fd, data, jdata));
            case GatewayProtocol::CMD_SELECT:
                return new(CmdSelect(fd, data, jdata));
            // 获取在线群组列表
            case GatewayProtocol::CMD_GET_GROUP_ID_LIST:
                return new(CmdGetGroupIdList(fd, data, jdata));
            // 重新赋值 session
            case GatewayProtocol::CMD_SET_SESSION:
                return new(CmdSetSession(fd, data, jdata));
            // session合并
            case GatewayProtocol::CMD_UPDATE_SESSION:
                return new(CmdUpdateSession(fd, data, jdata));
            case GatewayProtocol::CMD_GET_SESSION_BY_CLIENT_ID:
                return new(CmdGetSessionByCid(fd, data, jdata));
            // 获得客户端sessions
            case GatewayProtocol::CMD_GET_ALL_CLIENT_SESSIONS:
                return new(CmdGetAllClientSession(fd, data, jdata));
            // 判断某个 client_id 是否在线
            case GatewayProtocol::CMD_IS_ONLINE:
                return new(CmdIsOnline(fd, data, jdata));
            // 将 client_id 与 uid 绑定
            case GatewayProtocol::CMD_BIND_UID:
                return new(CmdBindUid(fd, data, jdata));
            // client_id 与 uid 解绑
            case GatewayProtocol::CMD_UNBIND_UID:
                return new(CmdUnBindUid(fd, data, jdata));
            // 发送数据给 uid
            case GatewayProtocol::CMD_SEND_TO_UID:
                return new(CmdSendToUid(fd, data, jdata));
            // 将 $client_id 加入用户组
            case GatewayProtocol::CMD_JOIN_GROUP:
                return new(CmdJoinGroup(fd, data, jdata));
            // 将 $client_id 从某个用户组中移除
            case GatewayProtocol::CMD_LEAVE_GROUP:
                return new(CmdLeaveGroup(fd, data, jdata));
            // 解散分组
            case GatewayProtocol::CMD_UNGROUP:
                return new(CmdUnGroup(fd, data, jdata));
            // 向某个用户组发送消息
            case GatewayProtocol::CMD_SEND_TO_GROUP:
                return new(CmdSendToGroup(fd, data, jdata));
            // 获取某用户组成员信息
            case GatewayProtocol::CMD_GET_CLIENT_SESSIONS_BY_GROUP:
                return new(CmdGetClientSessionsByGroup(fd, data, jdata));
            // 获取用户组成员数
            case GatewayProtocol::CMD_GET_CLIENT_COUNT_BY_GROUP:
                return new(CmdGetClientCountByGroup(fd, data, jdata));
            // 获取与某个 uid 绑定的所有 client_id
            case GatewayProtocol::CMD_GET_CLIENT_ID_BY_UID:
                return new(CmdGetClientIdByUid(fd, data, jdata));
            // 批量获取与 uid 绑定的所有 client_id
            case GatewayProtocol::CMD_BATCH_GET_CLIENT_ID_BY_UID:
                return new(CmdBatchGetClientIdByUid(fd, data, jdata));
            // 批量获取群组ID内客户端个数
            case GatewayProtocol::CMD_BATCH_GET_CLIENT_COUNT_BY_GROUP:
                return new(CmdBatchGetClientCountByGroup(fd, data, jdata));
            default :
                RTE_LOG(ERR, USER1, "[%s][%d] gateway inner pack err cmd=%d", __func__, __LINE__, cmd);
                echo $err_msg;
    }
    free(fdata);
}

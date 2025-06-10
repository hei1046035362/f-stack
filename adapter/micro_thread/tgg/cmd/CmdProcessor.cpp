#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <set>
#include <list>
#include "tgg_comm/WsConsumer.h"
#include <rte_log.h>
#include "tgg_comm/tgg_bwcomm.h"
#include "GatewayProtocal.h"
#include "CmdProcessor.h"
#include "comm/Encrypt.hpp"
#include "tgg_comm/BwMsgPack.hpp"
#include "tgg_comm/tgg_bw_cache.h"
#include "comm/common.hpp"
#include "comm/log.hpp"
#include "tgg_comm/tgg_transport.h"
#include "tgg_comm/tgg_conf.h"

static int s_compress_flag = 0;
static int s_is_open_binary = 0;

void CmdBaseProcessor::Send2BW(const nlohmann::json& data, bool serialize)
{
    std::string result = std::move(serialize ? Php_Serialize(data) : data.dump());
    int len = big_endian() ? htonl(result.size()) : result.size();
    std::string rsp;
    rsp.resize(sizeof(int));
    memcpy(const_cast<char* >(rsp.data()), &len, sizeof(int));
    rsp += std::move(result);
    int ret = write(this->fd, rsp.c_str(), rsp.size());
    if(ret < 0) {
        LOG_ERROR("send data[%s] to BW failed.", result.c_str());        
    }
}


static int get_remote_info(int sockfd, uint32_t& ip, ushort& port)
{
    // 获取IP地址信息
    struct sockaddr_in remote_addr;
    socklen_t addrlen = sizeof(remote_addr);
    // 获取远端地址信息
    if (getpeername(sockfd, (struct sockaddr *)&remote_addr, &addrlen) == -1) {
        LOG_ERROR("getpeername error.");
        return -1;
    }
    // 获取 IP 地址
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(remote_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
    LOG_INFO("Remote IP address: %s.", ip_str);
    // 获取 IP 地址的整数值
    ip = remote_addr.sin_addr.s_addr;
    // 获取端口号
    port = ntohs(remote_addr.sin_port);
    LOG_INFO("IP address in decimal: %u", ip);
    LOG_INFO("Remote port: %u", port);
    return 0;
}


int CmdWorkerConnect::ExecCmd()
{
    // int idx = tgg_get_bw_idx(this->prc_id, this->fd);
    // if(idx < 0) {
    //     RTE_LOG(ERR, USER1, "[%s][%d] Get idx[%d] for fd[%d] Failed.\n", __FILE__, __LINE__, fd, idx);
    //     return -1;
    // }
    // TODO Seckey是配置文件中的，不是跟连接绑定的？  抓包看seckey都是空的
    std::string bwSeckey = TggConfigure::getInstance()->get_secret_key();// tgg_get_bwfdx_seckey(this->prc_id, this->fd);
    // if(bwSeckey.empty()) {
    //     RTE_LOG(ERR, USER1, "[%s][%d] Get secret_key for fd[%d] Failed.\n", __FILE__, __LINE__, fd);
    //     close(this->fd);
    //     return -1;
    // }
    try {
        // printf("jdata:%s\n", jdata.dump(4).c_str());
        nlohmann::json worker_info = nlohmann::json::parse(std::string(jdata["body"]));
        if (worker_info["secret_key"].get<std::string>() != bwSeckey) {
            LOG_ERROR("Gateway: Worker key[%s] does not match conn key[%s].", 
                worker_info["secretKey"].get<std::string>().c_str(), bwSeckey.c_str());
            close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
            //tgg_close_bw_session(this->prc_id, this->fd);
            return -1;
        }
        uint32_t remote_ip; 
        ushort remote_port;
        if (get_remote_info(this->fd, remote_ip, remote_port) < 0) {// 获取远端ip port 失败
            LOG_ERROR("get remote info failed, fd:[%d].", this->fd);
            close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
            return -1;
        }
        std::string bwWokerkey = uint32_to_hex(remote_ip) + ":" + worker_info["worker_key"].get<std::string>();
        if (tgg_check_bwwkkey_exist(bwWokerkey.c_str()) >= 0) {// 在一台服务器上businessWorker->name不能相同
            close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
            // tgg_close_bw_session(this->prc_id, this->fd);
            LOG_ERROR("bw[%s] already exist.", bwWokerkey.c_str());
            return -1;
        }
        // tgg_add_bwwkkey(bwWokerkey.c_str());
        tgg_new_bw_session(this->prc_id, this->fd, GatewayProtocal::CMD_WORKER_CONNECT
            , bwWokerkey.c_str(), remote_ip, remote_port);
        // 初始化完成后，加入到客户端可选服务池中以便网关能将客户端连接绑定到该bw
        if (tgg_add_bwfdx((fd << 8) | (prc_id & 0xff)) < 0) {
            // 如果加入失败，就要销毁连接，否则这个服务就没有人使用
            tgg_close_bw_session(this->prc_id, this->fd);
            close(this->fd);
            LOG_ERROR("add bw[%d] fd[%d] failed.", prc_id, fd);
            return -1;
        }
        LOG_DEBUG("WorkerConnect: added bw[prc:%d,fd:%d] success, total bw count:%d.", 
            prc_id, fd, tgg_get_bwfdx_count());


        /// 1、考虑负载均衡  
        /// 2、要新增一个hash用来确定worker_key的唯一性
        /// 3、客户端绑定woker还没
        /// 4、进程退出要解绑的资源， woker对应的资源销毁，客户端连接绑定到新的woker





    } catch (const nlohmann::json::exception& e) {
    // 捕获其他任何未预料到的异常
        LOG_ERROR("Exception catched:%s.", e.what());
        close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
        // free_bw_session(this->prc_id, this->fd);
        return -1;
    }
    return 0;
}

int CmdGatewayClientConnect::ExecCmd()
{
    // int idx = tgg_get_bw_idx(this->prc_id, this->fd);
    // if(idx < 0) {
    //     RTE_LOG(ERR, USER1, "[%s][%d] Get idx[%d] for fd[%d] Failed.\n", __FILE__, __LINE__, fd, idx);
    //     return -1;
    // }
    // TODO Seckey是配置文件中的，不是跟连接绑定的？  抓包看seckey都是空的
    std::string bwSeckey = tgg_get_bwfdx_seckey(this->prc_id, this->fd);
    // if(bwSeckey.empty()) {
    //     RTE_LOG(ERR, USER1, "[%s][%d] Get secret_key for fd[%d] Failed.\n", __FILE__, __LINE__, fd);
    //     close(this->fd);
    //     return -1;
    // }
    try {
        uint32_t remote_ip; 
        ushort remote_port;
        if (get_remote_info(this->fd, remote_ip, remote_port) < 0) {// 获取远端ip port 失败
            LOG_ERROR("get remote info failed, fd:%d.", this->fd);
            close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
            return -1;
        }
        // printf("jdata:%s\n", jdata.dump(4).c_str());
        nlohmann::json worker_info = nlohmann::json::parse(std::string(jdata["body"]));
        if (worker_info["secret_key"].get<std::string>() != bwSeckey) {
            LOG_ERROR("Gateway: Worker key[%s] does not match conn key[%s].", 
                worker_info["secretKey"].get<std::string>().c_str(), bwSeckey.c_str());
            close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
            //tgg_close_bw_session(this->prc_id, this->fd);
            return -1;
        }
    } catch (const nlohmann::json::exception& e) {
    // 捕获其他任何未预料到的异常
        LOG_ERROR("Exception catched:%s.", e.what());
        close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
        // free_bw_session(this->prc_id, this->fd);
        return -1;
    }
    LOG_DEBUG("GatewayClientConnect: cmd executed body:%s.",
     jdata["body"].dump().c_str());
    // CMD_GATEWAY_CLIENT_CONNECT 类型的连接没有workerkey
    tgg_new_bw_session(this->prc_id, this->fd, GatewayProtocal::CMD_GATEWAY_CLIENT_CONNECT, "");
    return 0;
}

int CmdSendToOne::ExecCmd()
{
    int cid = jdata["connection_id"];
    int raw = jdata["flag"].get<std::int32_t>() & GatewayProtocal::FLAG_NOT_CALL_ENCODE;
    std::string body = std::move(hex2bin(jdata["body"].get<std::string>()));
    // TODO 目前只支持ws发送
    LOG_DEBUG("SendToOne: cmd executed cid[%d] data:%s.",
     cid, jdata["body"].get<std::string>().c_str());
    Send2Client(cid, body, FD_WRITE, !raw);
    return 0;
}

int CmdSendToGroup::ExecCmd()
{
    int raw = jdata["flag"].get<std::int32_t>() & GatewayProtocal::FLAG_NOT_CALL_ENCODE;
    std::string body = hex2bin(jdata["body"].get<std::string>());
    // 要排除的cid
    std::set<std::string> setExeptCid;
    nlohmann::json ext_data = nlohmann::json::parse(jdata["ext_data"].get<std::string>());
    if (ext_data.contains("exclude") && ext_data["exclude"].is_array()) {
        for (const auto& element : ext_data["exclude"]) {
            setExeptCid.insert(element.get<std::string>());
        }
    }
    // 所有需要发送数据的cid对应的fd
    std::list<int> lstAllFds;
    // 判断是否存在group字段且为数组类型
    if (ext_data.contains("group") && ext_data["group"].is_array()) {
        // 遍历需要发送数据的所有group
        for (const auto& element : ext_data["group"]) {
            // 通过gid找到在线的fdx列表
            std::list<int> lstFds;
            if (tgg_get_fdsbygid(element.get<std::string>().c_str(), lstFds) < 0) {// 没找到gid
                LOG_WARNING("gid[%s] not exist.", element.get<std::string>().c_str());
                continue;
            }
            // 根据hash<gid,list<fdid>>找到gid对应的fdid列表,根据fdid找到cid
            std::list<int>::iterator itFd = lstFds.begin();
            while (itFd != lstFds.end()) {
                int fdid = *itFd;
                if(fdid < 0) {
                    LOG_WARNING("Invalid fdid[%d] for gid[%s].", fdid, element.get<std::string>().c_str());
                    itFd++;
                    continue;
                }
                int coreid = fdid & 0xff;
                int fd = fdid >> 8;

                int cid = tgg_get_cli_cid(coreid, fd);
                if(cid <= 0) {
                    LOG_WARNING("cid for fdid[%d] gid[%s] not exist.", 
                        fdid, element.get<std::string>().c_str());
                    itFd++;
                    continue;
                }
                // 确认cid是否要排除
                std::set<std::string>::iterator iter = setExeptCid.find(std::to_string(cid));
                if(iter == setExeptCid.end()) {
                    // 不在排除队列中就加入发送队列
                    lstAllFds.push_back(fdid);
                }
                itFd++;
            }
            //std::cout << element << std::endl;
        }
        if(lstAllFds.size() > 0) {
            BatchSend2ClientByfds(lstAllFds, body, FD_WRITE, !raw);
            LOG_DEBUG("SendToGroup: cmd executed gid[%s].", ext_data["group"].dump().c_str());
        }
    } else {
        LOG_WARNING("SendToGroup: cmd executed, no Group found.");
        return -1;
    }
    LOG_DEBUG("SendToGroup: cmd executed.");
    return 0;
}

int CmdKick::ExecCmd()
{
    int cid = jdata["connection_id"];
    // std::string body = jdata["body"].get<std::string>();
    int raw = jdata["flag"].get<std::int32_t>() & GatewayProtocal::FLAG_NOT_CALL_ENCODE;
    // Send2Client(cid, body, FD_WRITE, !raw);
    Send2Client(cid, "kick", FD_WRITE|FD_CLOSE, !raw);
    LOG_DEBUG("Kick: cmd executed cid[%d].", cid);
    return 0;
}

int CmdDestroy::ExecCmd()
{
    int cid = jdata["connection_id"];
    // std::string data = "";// 关闭websocket
    int raw = jdata["flag"].get<std::int32_t>() & GatewayProtocal::FLAG_NOT_CALL_ENCODE;
    Send2Client(cid, "destroy", FD_WRITE|FD_CLOSE, !raw);// TODO 是否要立即销毁，不发送ws的关闭帧(去掉FD_WRITE就行)了
    LOG_DEBUG("Destroy: cmd executed cid[%d].", cid);
    return 0;
}


int CmdSendToALL::ExecCmd()
{
    int raw = jdata["flag"].get<std::int32_t>() & GatewayProtocal::FLAG_NOT_CALL_ENCODE;
    std::string body = hex2bin(jdata["body"].get<std::string>());
    // if(!raw) {
    // }

    std::list<int> lstCids;
    std::string ext_data = jdata["ext_data"];
    if(!ext_data.empty()) {
        nlohmann::json jext = nlohmann::json::parse(ext_data);
        if(jext.contains("connections") && jext["connections"].is_array()) {
            // 发送给所有指定的cid
            for (const auto& element : jext["connections"]) {
                // 通过gid找到在线的fd列表
                lstCids.push_back(element);
            }
            if(lstCids.size() > 0) {
                BatchSend2ClientBycids(lstCids, body, FD_WRITE, !raw);
            }
        }
        LOG_DEBUG("SendToALL: cmd executed cids[%s] data:%s.", ext_data.c_str(), jdata["body"].get<std::string>().c_str());
        return 0;
    }

    // 所有在线的客户端fd
    std::list<int> lstFds;
    if(tgg_get_allfds(lstFds) < 0) {
        LOG_WARNING("SendToALL: get all online clients failed.");
        return -1;
    }
    if(lstFds.size() > 0) {
        BatchSend2ClientByfds(lstFds, body, FD_WRITE, !raw);
    }
 
    LOG_DEBUG("SendToALL: sendto all clients, extend:%s.", jdata["body"].get<std::string>().c_str());
    return 0;
}

void CmdSelect::FormatResult(const std::list<int>& lst_fd, int mask, nlohmann::json& result)
{
    std::list<int>::const_iterator itFd = lst_fd.begin();
    while (itFd != lst_fd.end()) {
        if(*itFd < 0) {
            LOG_WARNING("invalid fd.");
            itFd++;
            continue;
        }
        int coreid = *itFd & 0xff;
        int fd = *itFd >> 8;
        int cid = tgg_get_cli_cid(coreid, fd);
        if(cid <= 0) {
            LOG_WARNING("cid for fd[%d] not exist.", fd);
            itFd++;
            continue;
        }
        std::string scid = std::to_string(cid);
        if(!result.contains(scid)) {
            result[scid] = nlohmann::json::object();
        }
        std::string uid = tgg_get_cli_uid(coreid, fd);
        if(uid.empty()) {
            LOG_WARNING("[%s][%d] uid for fd[%d] not exist.", fd);
            itFd++;
            continue;
        }
        if(mask & FIELD_GID) {
            std::set<std::string> set_gids;
            if (!tgg_get_gidsbyuid(uid.c_str(), set_gids)) {
                if (!result[scid].contains("groups")) {
                    result[scid]["groups"] = nlohmann::json::array();
                } else {
                    // 已经填充过了就不要再次执行了
                    LOG_WARNING("cid[%d] groups already exist.", cid);
                }
                std::set<std::string>::iterator itGid = set_gids.begin();
                while(itGid != set_gids.end()) {
                    result[scid]["groups"].push_back(*itGid);
                    itGid++;
                }
            }
        }
        if(mask & FIELD_UID) {
            if (!result[scid].contains("uid")) {
                result[scid]["uid"] = nlohmann::json::object();
                result[scid]["uid"] = uid;
            } else {
                // 已经填充过了就不要再次执行了
                LOG_WARNING("cid[%d] groups already exist.", cid);
            }
        }
        itFd++;
    }
}

int CmdSelect::ExecCmd()
{
    std::string ext_data = jdata["ext_data"];
    nlohmann::json result = nlohmann::json::object();
    if(ext_data.empty()) {
        LOG_WARNING("Select cmd, extend data:%s.", ext_data.c_str());
        Send2BW(result);
        return 0;
    }
    try {
        nlohmann::json jext_data = nlohmann::json::parse(ext_data);
        std::vector<std::string> fields = jext_data["fields"].get<std::vector<std::string>>();
        int mask = 0;// 根据fields字段设置返回数据的掩码
        for(auto& it : fields) {
            if(it == "cid") {
                mask |= FIELD_CID;
            } else if (it == "uid") {
                mask |= FIELD_UID;
            } else if (it == "gid") {
                mask |= FIELD_GID;
            }
        }
        nlohmann::json where = jext_data["where"];
        result = nlohmann::json::object();
        //std::map<int, std::map<std::string, std::string>> client_info_array;
        if (!where.is_null()) {
            for (auto& it : where.items()) {
                const std::string& key = it.key();
                if (key!= "connection_id") {// json数据格式不一样，所以要区分一下
                    // group user session [123123213213,123123123123]
                    auto& items = it.value();
                        
                    for (const auto& item : items) {// item为gid,uid等  where 条件中的item
                        // 通过gid获取该group下的所有fd
                        std::list<int> lst_fd;
                        if(key == "groups") {
                            if (tgg_get_fdsbygid(item.get<std::string>().c_str(), lst_fd) < 0) {// gid是否存在,并取出gid所有连接
                                continue;
                            }
                        }else if (key == "uid"){
                            if (tgg_get_fdsbyuid(item.get<std::string>().c_str(), lst_fd) < 0) {// uid是否存在,并取出uid所有连接
                                continue;
                            }
                        }
                        if(lst_fd.empty()) {
                            continue;
                        }
                        FormatResult(lst_fd, mask, result);
                    }
                } else {
                    // cid {"9527":9527}
                    std::list<int> lst_fds;
                    for (const auto& connection_id : it.value()) {
                        int cid = connection_id;
                        int fdid = tgg_get_fdbycid(cid);
                        if (fdid > 0) {
                            lst_fds.push_back(fdid);
                        }
                    }
                    FormatResult(lst_fds, mask, result);
                }
            }
        } else {
            std::list<int> lst_fds;
            if (!tgg_get_allfds(lst_fds)) {
                if(lst_fds.size() > 0) {
                    FormatResult(lst_fds, mask, result);
                }
            }
        }
        // Php json转php格式化字符串
    } catch (const std::exception& e) {
        LOG_ERROR("Error parsing data:%s.", e.what());
    }
    Send2BW(result);
    LOG_DEBUG("Select: cmd executed Select[%s] data:%s.", ext_data.c_str(), result.dump().c_str());
    return 0;
}

int CmdGetGroupIdList::ExecCmd()
{
    std::list<std::string> lst_gid;
    if (tgg_get_allonlinegids(lst_gid) < 0) {
        LOG_WARNING("get all online gids failed.");
    }
    nlohmann::json result = nlohmann::json::array();
    std::list<std::string>::iterator it = lst_gid.begin();
    while (it != lst_gid.end()) {
        result.push_back(*it);
        it++;
    }
    Send2BW(result);
    LOG_DEBUG("GetGroupIdList: cmd executed data:%s.", result.dump().c_str());
    return 0;
}

int CmdSetSession::ExecCmd()
{
    std::string session = jdata["ext_data"];
    int cid = jdata["connection_id"];
    if(cid <= 0) {
        LOG_ERROR("set session failed, invalid cid[%d].", cid);
        return -1;
    }
    int fdid = tgg_get_fdbycid(cid);
    if(fdid < 0) {
        LOG_ERROR("get fdid by cid[%d] failed.", cid);
        return -1;
    }
    // 判断是不是有效的 php序列化后的字符串
    if(session.length() > 2 && session[1] != ':') {
        LOG_ERROR("get fdid by cid[%d] failed.", cid);
        return -1;
    }
    LOG_DEBUG("SetSession: cmd executed cid[%d] data:%s.", cid, session.c_str());
    return tgg_set_cli_reserved(fdid & 0xff, fdid >> 8, session.c_str());
}

int CmdGetSessionByCid::ExecCmd()
{
    nlohmann::json result;
    int cid = jdata["connection_id"];
    int fdid = -1;
    std::string session;
    if(cid <= 0) {
        LOG_ERROR("set session failed, invalid cid[%d].", cid);
        goto SEND_GET_SESSION;
    }
    fdid = tgg_get_fdbycid(cid);
    if(fdid <= 0) {
        LOG_ERROR("get fdid by cid[%d] failed.", cid);
        goto SEND_GET_SESSION;
    }
    session = tgg_get_cli_reserved(fdid & 0xff, fdid >> 8);
    if(session.empty()) {
        result = nlohmann::json::array();
        LOG_INFO("session is empty of cid[%d].", cid);
        goto SEND_GET_SESSION;
    }
    result = session;
    LOG_DEBUG("GetSession: cmd executed cid[%d] data:%s.", cid, session.c_str());
    Send2BW(result, false);
    return 0;

SEND_GET_SESSION:
    Send2BW(result);
    return 0;
}

int CmdGetAllClientSession::ExecCmd()
{
    nlohmann::json result = nlohmann::json::array();
    std::list<int> lst_fds;
    tgg_get_allfds(lst_fds);
    for (auto fdid : lst_fds) {
        std::string session = tgg_get_cli_reserved(fdid & 0xff, fdid >> 8);
        int cid = tgg_get_cli_cid(fdid & 0xff, fdid >> 8);
        nlohmann::json node = nlohmann::json::object();
        node[std::to_string(cid)] = session;
        result.push_back(node);
    }
    LOG_DEBUG("GetAllClientSession: cmd executed data:%s.", result.dump().c_str());
    Send2BW(result);
    return 0;
}

static void json_replace_recursive(nlohmann::json& target, const nlohmann::json& source) {
    // 处理数组类型（您的特殊格式）
    if (target.is_array() && source.is_array()) {
        // 遍历源数组中的每个单键对象
        for (const auto& source_item : source) {
            if (!source_item.is_object() || source_item.size() != 1) 
                continue;  // 跳过非单键对象

            // 提取源对象的键值对
            auto it = source_item.begin();
            const std::string key = it.key();
            const nlohmann::json& value = it.value();

            // 在target中查找相同键的对象
            bool found = false;
            for (auto& target_item : target) {
                if (target_item.is_object() && target_item.contains(key)) {
                    found = true;
                    // 递归合并值（支持嵌套对象）
                    json_replace_recursive(target_item[key], value);
                    break;  // 单键对象只需处理一次
                }
            }

            // 未找到则添加新对象
            if (!found) {
                target.push_back({{key, value}});
            }
        }
    } 
    // 处理标准对象类型
    else if (target.is_object() && source.is_object()) {
        for (auto it = source.begin(); it != source.end(); ++it) {
            const auto& key = it.key();
            const auto& value = it.value();

            if (target.contains(key) && target[key].is_object() && value.is_object()) {
                json_replace_recursive(target[key], value);
            } else {
                target[key] = value;
            }
        }
    } 
    // 其他类型直接覆盖
    else {
        target = source;
    }
}

// 多源版本（支持多个source）
// static nlohmann::json json_replace_recursive(const nlohmann::json& target, 
//     std::initializer_list<nlohmann::json> sources) {
//     nlohmann::json result = target; // 复制初始目标
//     for (const auto& src : sources) {
//         json_replace_recursive(result, src); // 依次合并每个源
//     }
//     return result;
// }

int CmdUpdateSession::ExecCmd()
{
    // TODO 稍微有点复杂，且当前拿不到数据
    int cid = jdata["connection_id"];
    if(cid <= 0) {
        LOG_ERROR("set session failed, invalid cid[%d].",  cid);
        return -1;
    }
    int fdid = tgg_get_fdbycid(cid);
    if(fdid < 0) {
        LOG_ERROR("get fd by cid[%d] failed.", cid);
        return -1;
    }
    int coreid = fdid & 0xff;
    int clifd = fdid >> 8;
    std::string ext_data = jdata["ext_data"];
    std::string session = tgg_get_cli_reserved(coreid, clifd);
    if(session.empty()) {
        if (tgg_set_cli_reserved(coreid, clifd, ext_data.c_str()) < 0) {
            LOG_ERROR("update session failed cid[%d] session[%s] failed.", cid, session.c_str());
            return -1;
        }
        return 0;
    }
    nlohmann::json jsession = Php_UnSerialize(session);
    nlohmann::json jsession_for_merge = Php_UnSerialize(ext_data);
    json_replace_recursive(jsession, jsession_for_merge);
    std::string data = Php_Serialize(jsession);
    tgg_set_cli_reserved(coreid, clifd, data.c_str());
    LOG_DEBUG("UpdateSession: cmd executed cid[%d] data:%s.", cid, data.c_str());
    return 0;
}

int CmdIsOnline::ExecCmd()
{
    nlohmann::json result;
    int cid = jdata["connection_id"];
    int clifdx = tgg_get_fdbycid(cid);
    if(clifdx <= 0) {
        result = "0";
    } else {
        result = "1";
    }
    LOG_DEBUG("IsOnline: send cid[%d] IsOnline result[%s] to server.", cid, result.dump().c_str());
    Send2BW(result);
    return 0;
}

int CmdBindUid::ExecCmd()
{
    // std::string s_uid = std::to_string(jdata["user_id"].get<std::uint64_t>());
    // return tgg_bind_session(this->fd, s_uid.c_str(), tgg_get_cli_cid(this->fd).c_str());
    // TODO Binduid到底是客户端过来消息绑定，还是服务端过来消息绑定
    std::string suid = jdata["ext_data"].get<std::string>();
    int cid = jdata["connection_id"];
    if(suid.empty() || cid < 0) {
        LOG_ERROR("bind uid failed, uid[%s] and cid[%d] shouldn't be empty.", suid.c_str(), cid);
        return -1;
    }
    LOG_DEBUG("BindUid: cid[%d] bind to uid[%s].", cid, suid.c_str());
    return tgg_bind_session(suid.c_str(), cid);

}

int CmdUnBindUid::ExecCmd()
{
    int cid = jdata["connection_id"];
    if(cid < 0) {
        LOG_ERROR("unbind failed, invalid cid[%d].", cid);
        return -1;
    }
    LOG_DEBUG("UnBindUid: unbind cid[%d].", cid);
    return tgg_unbind_session(cid);
    // return tgg_free_session(fdid & 0xff, fdid >> 8);
}


int CmdSendToUid::ExecCmd()
{
    bool raw = jdata["flag"].get<std::int32_t>() & GatewayProtocal::FLAG_NOT_CALL_ENCODE;
    std::string body = jdata["body"];
    std::list<int> lst_fds;
    nlohmann::json juid = nlohmann::json::parse(jdata["ext_data"].get<std::string>());
    std::vector<std::string> vec_uids = juid.get<std::vector<std::string> >();
    for(auto& it : vec_uids) {
        std::list<int> lst_fd;
        if (tgg_get_fdsbyuid(it.c_str(), lst_fd) < 0) {
            LOG_WARNING("SendToUid: no fd found for uid[%s].", it.c_str());
            continue;
        }
        lst_fds.splice(lst_fds.end(), lst_fd);
    }
    if(lst_fds.size() > 0) {
        BatchSend2ClientByfds(lst_fds, body, FD_WRITE, !raw);
        LOG_DEBUG("SendToUid: cmd exec success.");
    } else {
        LOG_WARNING("SendToUid: no fd found for all uids[%s].", juid["ext_data"].get<std::string>().c_str());
    }
    return 0;
}


int CmdJoinGroup::ExecCmd()
{
    std::string group = jdata["ext_data"];
    int cid = jdata["connection_id"];
    if(group.empty() || cid <= 0) {
        LOG_ERROR("set session failed, ext_data[%s] and cid[%d] shouldn't be empty.", group.c_str(), cid);
        return -1;
    }
    int fdid = tgg_get_fdbycid(cid);
    if(fdid < 0) {
        RTE_LOG(INFO, USER1, "[%s][%d] get fdid by cid[%d] failed.", __FILE__, __LINE__, cid);
        return -1;
    }
    tgg_join_group(group.c_str(), cid);
    LOG_DEBUG("JoinGroup: cmd executed cid[%d] gid[%s].", cid, group.c_str());
    return 0;
}


int CmdLeaveGroup::ExecCmd()
{
    std::string group = jdata["ext_data"];
    int cid = jdata["connection_id"];
    if(group.empty() || cid <= 0) {
        LOG_ERROR("set session failed, ext_data[%s] and cid[%d] shouldn't be empty.", group.c_str(), cid);
        return -1;
    }
    int fdid = tgg_get_fdbycid(cid);
    if(fdid < 0) {
        LOG_ERROR("get fdid by cid[%d] failed.", cid);
        return -1;
    }
    tgg_exit_group(group.c_str(), cid);
    LOG_DEBUG("LeaveGroup: cmd executed cid[%d] gid[%s].", cid, group.c_str());
    return 0;
}

int CmdUnGroup::ExecCmd()
{
    std::string group = jdata["ext_data"];
    if(group.empty()) {
        LOG_ERROR("ungroup failed, group[%s] shouldn't be empty.",
                 __FILE__, __LINE__, group.c_str());
        return -1;
    }

    tgg_del_gid_cidgid(group.c_str());// 这里顺序不能动，得先删除hash<cid,gid>中的部分，才能删除hash<gid,list<fdid>>
    tgg_del_gid(group.c_str());
    LOG_DEBUG("UnGroup: cmd executed gid[%s].", group.c_str());
    return 0;
}

int CmdGetClientSessionsByGroup::ExecCmd()
{
    nlohmann::json result = nlohmann::json::array();
    std::string group = jdata["ext_data"];
    if(group.empty()) {
        LOG_ERROR("get session by group failed, group[%s] shouldn't be empty.", group.c_str());
        Send2BW(result);
        return -1;
    }
    std::list<int> lst_sfd;
    if (!tgg_get_fdsbygid(group.c_str(), lst_sfd)) {
        std::list<int>::iterator itFd = lst_sfd.begin();
        while (itFd != lst_sfd.end()) {
            int coreid = *itFd & 0xff;
            int fd = *itFd >> 8;
            int cid = tgg_get_cli_cid(coreid, fd);
            if(cid <= 0) {
                LOG_WARNING("invalid cid[%d].", cid);
                itFd++;
                continue;
            }
            std::string connection_id = std::to_string(cid);// cid的前12位是ip和port，后面的才是connection_id
            std::string session = tgg_get_cli_reserved(coreid, fd);
            nlohmann::json unit = nlohmann::json::object();
            unit[connection_id] = session;
            result.push_back(unit);
            itFd++;
        }
    }
    Send2BW(result);
    LOG_DEBUG("GetClientSessionsByGroup: cmd executed gid[%s] data:%s.", group.c_str(), result.dump().c_str());
    return 0;
}


int CmdGetClientCountByGroup::ExecCmd()
{
    nlohmann::json result = 0;
    std::string group = jdata["ext_data"];
    if(group.empty()) {
        std::list<int> lst_cid;
        tgg_get_allonlinecids(lst_cid);
        result = lst_cid.size();
        LOG_DEBUG("GetAllClientCount:%s.", result.dump().c_str());
        Send2BW(result);
        return 0;
    }
    std::list<int> lst_sfd;
    int count = 0;// TODO  前期调试需要排查格式等问题，后期应该直接计算lst_sfd的长度即可
    if (!tgg_get_fdsbygid(group.c_str(), lst_sfd)) {
        count = lst_sfd.size();
    }
    result = count;
    Send2BW(result);
    LOG_DEBUG("GetClientCountByGroup: cmd executed gid[%s] data:%s.", group.c_str(), result.dump().c_str());
    return 0;
}

int CmdGetClientIdByUid::ExecCmd()
{
    nlohmann::json result = nlohmann::json::array();
    std::string data;
    std::string suid = jdata["ext_data"];
    if(suid.empty()) {
        LOG_ERROR("get session by uid failed, uid[%s] shouldn't be empty.", suid.c_str());
        Send2BW(result);
        return -1;
    }
    std::list<int> lst_sfd;
    if (tgg_get_fdsbyuid(suid.c_str(), lst_sfd) == 0) {
        std::list<int>::iterator itFd = lst_sfd.begin();
        while (itFd != lst_sfd.end()) {
          int cid = tgg_get_cli_cid(*itFd & 0xff, *itFd >> 8);
            if(cid < 0) {
                LOG_ERROR("invalid cid[%d].", cid);
                itFd++;
                continue;
            }
            std::string connection_id = std::to_string(cid);// cid的前12位是ip和port，后面的才是connection_id
            result.push_back(connection_id);
            itFd++;
        }
    } else {
        LOG_ERROR("no session found for uid[%s].", suid.c_str());
    }

    Send2BW(result);
    LOG_DEBUG("GetClientIdByUid: cmd executed uid[%s] data:%s.", suid.c_str(), result.dump().c_str());
    return 0;
}

int CmdBatchGetClientIdByUid::ExecCmd()
{
    nlohmann::json result = nlohmann::json::array();
    std::string data;
    nlohmann::json juid = nlohmann::json::parse(jdata["ext_data"].get<std::string>());
    std::vector<std::string> vec_uids = juid.get<std::vector<std::string> >();
    for(auto& it : vec_uids) {
        std::list<int> lst_sfd;
        nlohmann::json juid = nlohmann::json::object();
        juid[it] = nlohmann::json::array();
        if (!tgg_get_fdsbyuid(it.c_str(), lst_sfd)) {
            std::list<int>::iterator itFd = lst_sfd.begin();
            while (itFd != lst_sfd.end()) {
                int cid = tgg_get_cli_cid(*itFd & 0xff, *itFd >> 8);
                if(cid < 0) {
                    LOG_WARNING("invalid cid[%d].", cid);
                    itFd++;
                    continue;
                }
                // std::string connection_id = std::to_string(cid);// cid的前12位是ip和port，后面的才是connection_id
                juid[it].push_back(cid);
                itFd++;
            }
        }
        result.push_back(juid);
    }
    Send2BW(result);
    LOG_DEBUG("BatchGetClientIdByUid: cmd executed data:%s.", result.dump().c_str());
    return 0;
}

static int json_parse_body(unsigned char flag, nlohmann::json& jdata)//const std::string& jdata, std::string& result)
{
    int cmd = 0;
    std::string result;
    nlohmann::json obj;
    std::string body = jdata["body"].get<std::string>();
    if(body.length() <= 4) {
        LOG_DEBUG("invalid body length:%d.", body.length());
        return 0;
    }
    //                                           0x32 -> ":"                 0x7b -> "{"
    if(body.length() > 4 && body.substr(2, 2) != "3a" && body.substr(0, 2) != "7b") {
        // 当前body为字符串，需要在发送的时候转换成二进制
        LOG_DEBUG("bin data[%s] to send.", body.c_str());
        return 0;
    }
    jdata["body"] = hex2bin(body);
    try {
        
        if(!flag) {
            obj = Php_UnSerialize(jdata["body"].get<std::string>());
        } else {
            obj = nlohmann::json::parse(jdata["body"].get<std::string>());
        }
        jdata["body"] = obj.dump();
        if(!obj.contains("cmd")) {// 没有cmd就不需要解包
            LOG_DEBUG("no cmd found in body:\n%s.", jdata["body"].get<std::string>().c_str());
            return 0;
        }
        LOG_DEBUG("body: %s", obj.dump(4).c_str());
        cmd = obj["cmd"].get<std::int32_t>();
    } catch (const nlohmann::json::parse_error& e) {
        LOG_ERROR("parse json error:%s", e.what());
        return -1;
    }
    if (cmd) {
        if (s_is_open_binary) {
            result = obj["data"].get<std::string>();
        } else {
            std::string bin = hex2bin(obj["data"].get<std::string>());
            if (bin.length() <= 0) {
                LOG_ERROR("hex2bin failed:%s.", obj["data"].get<std::string>().c_str());
            }
            if (message_pack(cmd, 1, 2,
                (uint8_t)s_compress_flag, bin, result) < 0) {
                LOG_ERROR("message_pack failed:%s.", bin.c_str());
            }
        }
        jdata["body"] = result;
    }

    return 0;
}

// 接收数据帧的校验
static bool bwdata_frame_check(tgg_bw_data* bdata, tgg_bw_protocal* bwdata)
{
    // 包长度校验
    unsigned int pack_len = big_endian() ? htonl(bwdata->pack_len) : bwdata->pack_len;
    unsigned int ext_len = big_endian() ? htonl(bwdata->ext_len) : bwdata->ext_len;
    if(pack_len != bdata->data_len) {
        LOG_ERROR("data fram length[%d] check failed, read buf_size[%d].", pack_len, bdata->data_len);
        return false;
    }
    // cmd 范围校验
    if(bwdata->cmd > CMD_MAX_INDEX || bwdata->cmd <= 0) {
        LOG_ERROR("cmd check failed, invalid cmd[%d].", bwdata->cmd);
        return false;
    }
    // 扩展长度校验
    if(ext_len > pack_len - sizeof(tgg_bw_protocal)) {
        LOG_ERROR("ext_len[%d] check failed, pack_len[%d].", ext_len, pack_len);
        return false;
    }
    return true;
}

void exec_cmd_processor(int prc_id, int fd, void* data)
{
    //std::string json_str = R"({"name": "Jane Smith", "age": 25, "is_student": true})";
    tgg_bw_data* bdata = (tgg_bw_data*)data;
    tgg_bw_protocal* bwdata = (tgg_bw_protocal*)bdata->data;
    if(!bwdata_frame_check(bdata, bwdata)) {
        return;
    }
    CmdBaseProcessor* pro = NULL;
    nlohmann::json jdata;
    // 解析帧并生成json对象
    BwPackageHandler::decode(bwdata, jdata);

    LOG_DEBUG("jdata:%s", jdata.dump(4).c_str());

        // 首次连接判断
    int cmd = jdata["cmd"].get<std::int32_t>();
    int authorized = tgg_get_bwfdx_authorized(prc_id, fd);
    if (!authorized && cmd != CMD_WORKER_CONNECT && cmd != CMD_GATEWAY_CLIENT_CONNECT) {
        tgg_close_bw_session(prc_id, fd);
        close(fd);
        LOG_ERROR("command[%d] error or not authorized[%d].", cmd, authorized);
        return ;
    }

    // TODO 这里的逻辑还不确定到底是什么意思，上行数据，待调试
    json_parse_body(bwdata->flag, jdata);

    switch(cmd) {
        case CMD_WORKER_CONNECT:
            pro = new CmdWorkerConnect(prc_id, fd, data, jdata);
            break;
        case CMD_GATEWAY_CLIENT_CONNECT:
            pro = new CmdGatewayClientConnect(prc_id, fd, data, jdata);
            break;
        // 向某客户端发送数据
        case CMD_SEND_TO_ONE:
            pro = new CmdSendToOne(prc_id, fd, data, jdata);
            break;
        // 踢出用户
        case CMD_KICK:
            pro = new CmdKick(prc_id, fd, data, jdata);
            break;
        // 立即销毁用户连接
        case CMD_DESTROY:
            pro = new CmdDestroy(prc_id, fd, data, jdata);
            break;
        // 广播
        case CMD_SEND_TO_ALL:
            // 暂时不需要
            pro = new CmdSendToALL(prc_id, fd, data, jdata);
            break;
        case CMD_SELECT:
            pro = new CmdSelect(prc_id, fd, data, jdata);
            break;
        // 获取在线群组列表
        case CMD_GET_GROUP_ID_LIST:
            pro = new CmdGetGroupIdList(prc_id, fd, data, jdata);// 暂时不需要
            break;
        // 重新赋值 session
        case CMD_SET_SESSION:
            pro = new CmdSetSession(prc_id, fd, data, jdata);
            break;
        // session合并
        case CMD_UPDATE_SESSION:
            pro = new CmdUpdateSession(prc_id, fd, data, jdata);
            break;
        case CMD_GET_SESSION_BY_CLIENT_ID:
            pro = new CmdGetSessionByCid(prc_id, fd, data, jdata);// 暂时不需要
            break;
        // 获得客户端sessions
        case CMD_GET_ALL_CLIENT_SESSIONS:
            pro = new CmdGetAllClientSession(prc_id, fd, data, jdata);// 暂时不需要
            break;
        // 判断某个 client_id 是否在线
        case CMD_IS_ONLINE:
            pro = new CmdIsOnline(prc_id, fd, data, jdata);
            break;
        // 将 client_id 与 uid 绑定
        case CMD_BIND_UID:
            pro = new CmdBindUid(prc_id, fd, data, jdata);
            break;
        // client_id 与 uid 解绑
        case CMD_UNBIND_UID:
            pro = new CmdUnBindUid(prc_id, fd, data, jdata);// 暂时不需要
            break;
        // 发送数据给 uid
        case CMD_SEND_TO_UID:
            pro = new CmdSendToUid(prc_id, fd, data, jdata);
            break;
        // 将 $client_id 加入用户组
        case CMD_JOIN_GROUP:
            pro = new CmdJoinGroup(prc_id, fd, data, jdata);
            break;
        // 将 $client_id 从某个用户组中移除
        case CMD_LEAVE_GROUP:
            pro = new CmdLeaveGroup(prc_id, fd, data, jdata);
            break;
        // 解散分组
        case CMD_UNGROUP:
            pro = new CmdUnGroup(prc_id, fd, data, jdata);
            break;
        // 向某个用户组发送消息
        case CMD_SEND_TO_GROUP:
            pro = new CmdSendToGroup(prc_id, fd, data, jdata);
            break;
        // 获取某用户组成员信息
        case CMD_GET_CLIENT_SESSIONS_BY_GROUP:
            pro = new CmdGetClientSessionsByGroup(prc_id, fd, data, jdata);
            break;
        // 获取用户组成员数
        case CMD_GET_CLIENT_COUNT_BY_GROUP:
            pro = new CmdGetClientCountByGroup(prc_id, fd, data, jdata);
            break;
        // 获取与某个 uid 绑定的所有 client_id
        case CMD_GET_CLIENT_ID_BY_UID:
            pro = new CmdGetClientIdByUid(prc_id, fd, data, jdata);
            break;
        // 批量获取与 uid 绑定的所有 client_id
        case CMD_BATCH_GET_CLIENT_ID_BY_UID:
            pro = new CmdBatchGetClientIdByUid(prc_id, fd, data, jdata);
            break;
        // 批量获取群组ID内客户端个数
        case CMD_BATCH_GET_CLIENT_COUNT_BY_GROUP:
            pro = new CmdBatchGetClientCountByGroup(prc_id, fd, data, jdata);// 暂时不需要
            break;
        default :
            LOG_ERROR("Gateway inner pack err, Unknown cmd=%d.", cmd);
            break;
    }
    if(pro) {
        pro->ExecCmd();
        delete pro;
        pro = NULL;
    }
}


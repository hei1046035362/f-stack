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
static int s_compress_flag = 0;
static int s_is_open_binary = 0;


// 从 fd:idx 中分离出来fd并转换成整数
int get_fd_by_fdidx(std::string& sfdidx)
{
    size_t pos = sfdidx.find(':');
    if (pos!= std::string::npos) {
        std::string numStr = sfdidx.substr(0, pos);
        int num = std::stoi(numStr);
        return num;
    }
    return -1;
}
// 从 fd:idx 中分离出来idx并转换成整数
int get_fd_idx_fdidx(std::string& sfdidx)
{
    size_t pos = sfdidx.find(':');
    if (pos!= std::string::npos) {
        std::string numStr = sfdidx.substr(pos+1);
        int num = std::stoi(numStr);
        return num;
    }
    return -1;
}

void CmdBaseProcessor::Send2BW(const std::string& data)
{
    int len = big_endian() ? htonl(data.length()) : data.length();
    std::string rsp;
    rsp.resize(sizeof(int));
    memcpy(const_cast<char* >(rsp.data()), &len, sizeof(int));
    rsp += data;
    int ret = write(this->fd, rsp.c_str(), rsp.length());
    if(ret < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d] send data[%s] to BW failed.\n", __func__, __LINE__, data.c_str());        
    }
}


static int get_remote_info(int sockfd, uint32_t& ip, ushort& port)
{
    // 获取IP地址信息
    struct sockaddr_in remote_addr;
    socklen_t addrlen = sizeof(remote_addr);
    // 获取远端地址信息
    if (getpeername(sockfd, (struct sockaddr *)&remote_addr, &addrlen) == -1) {
        perror("getpeername");
        return -1;
    }
    // 获取 IP 地址
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(remote_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
    printf("Remote IP address: %s\n", ip_str);
    // 获取 IP 地址的整数值
    ip = remote_addr.sin_addr.s_addr;
    // 获取端口号
    port = ntohs(remote_addr.sin_port);
    printf("IP address in decimal: %u\n", ip);
    printf("Remote port: %u\n", port);
    return 0;
}


int CmdWorkerConnect::ExecCmd()
{
    // int idx = tgg_get_bw_idx(this->prc_id, this->fd);
    // if(idx < 0) {
    //     RTE_LOG(ERR, USER1, "[%s][%d] Get idx[%d] for fd[%d] Failed.\n", __func__, __LINE__, fd, idx);
    //     return -1;
    // }
    // TODO Seckey是配置文件中的，不是跟连接绑定的？  抓包看seckey都是空的
    std::string bwSeckey = tgg_get_bwfdx_seckey(this->prc_id, this->fd);
    // if(bwSeckey.empty()) {
    //     RTE_LOG(ERR, USER1, "[%s][%d] Get secret_key for fd[%d] Failed.\n", __func__, __LINE__, fd);
    //     close(this->fd);
    //     return -1;
    // }
    try {
        // printf("jdata:%s\n", jdata.dump(4).c_str());
        nlohmann::json worker_info = nlohmann::json::parse(std::string(jdata["body"]));
        if (worker_info["secret_key"].get<std::string>() != bwSeckey) {
            RTE_LOG(ERR, USER1, "[%s][%d] Gateway: Worker key[%s] does not match conn key[%s].", 
                __func__, __LINE__, worker_info["secretKey"].get<std::string>().c_str(), bwSeckey.c_str());
            close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
            //tgg_close_bw_session(this->prc_id, this->fd);
            return -1;
        }
        uint32_t remote_ip; 
        ushort remote_port;
        if (get_remote_info(this->fd, remote_ip, remote_port) < 0) {// 获取远端ip port 失败
            close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
            return -1;
        }
        std::string bwWokerkey = uint32_to_hex() + ":" + worker_info["worker_key"];
        if (tgg_check_bwwkkey_exist(bwWokerkey.c_str()) < 0) {// 在一台服务器上businessWorker->name不能相同
            close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
            // tgg_close_bw_session(this->prc_id, this->fd);
            return -1;
        }
        tgg_add_bwwkkey(bwWokerkey.c_str());
        tgg_new_bw_session(this->prc_id, this->fd, GatewayProtocal::CMD_WORKER_CONNECT
            , bwWokerkey.c_str(), remote_ip, remote_port);
        // 初始化完成后，加入到客户端可选服务池中以便网关能将客户端连接绑定到该bw
        if (tgg_add_bwfdx((fd << 8) | (prc_id & 0xf)) < 0) {
            // 如果加入失败，就要销毁连接，否则这个服务就没有人使用
            tgg_close_bw_session(this->prc_id, this->fd);
            close(this->fd);
            return -1;
        }


        /// 1、考虑负载均衡  
        /// 2、要新增一个hash用来确定worker_key的唯一性
        /// 3、客户端绑定woker还没
        /// 4、进程退出要解绑的资源， woker对应的资源销毁，客户端连接绑定到新的woker





    } catch (const nlohmann::json::exception& e) {
    // 捕获其他任何未预料到的异常
        RTE_LOG(ERR, USER1, "[%s][%d] Exception catched:%s.\n", __func__, __LINE__, e.what());
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
    //     RTE_LOG(ERR, USER1, "[%s][%d] Get idx[%d] for fd[%d] Failed.\n", __func__, __LINE__, fd, idx);
    //     return -1;
    // }
    // TODO Seckey是配置文件中的，不是跟连接绑定的？  抓包看seckey都是空的
    std::string bwSeckey = tgg_get_bwfdx_seckey(this->prc_id, this->fd);
    // if(bwSeckey.empty()) {
    //     RTE_LOG(ERR, USER1, "[%s][%d] Get secret_key for fd[%d] Failed.\n", __func__, __LINE__, fd);
    //     close(this->fd);
    //     return -1;
    // }
    try {
        // printf("jdata:%s\n", jdata.dump(4).c_str());
        nlohmann::json worker_info = nlohmann::json::parse(std::string(jdata["body"]));
        if (worker_info["secret_key"].get<std::string>() != bwSeckey) {
            RTE_LOG(ERR, USER1, "[%s][%d] Gateway: Worker key[%s] does not match conn key[%s].", 
                __func__, __LINE__, worker_info["secretKey"].get<std::string>().c_str(), bwSeckey.c_str());
            close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
            //tgg_close_bw_session(this->prc_id, this->fd);
            return -1;
        }

    } catch (const nlohmann::json::exception& e) {
    // 捕获其他任何未预料到的异常
        RTE_LOG(ERR, USER1, "[%s][%d] Exception catched:%s.\n", __func__, __LINE__, e.what());
        close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
        // free_bw_session(this->prc_id, this->fd);
        return -1;
    }
    tgg_new_bw_session(this->prc_id, this->fd, GatewayProtocal::CMD_GATEWAY_CLIENT_CONNECT);
    return 0;
}

int CmdSendToOne::ExecCmd()
{
    std::string scid = get_valid_cid(jdata["connection_id"]);
    int raw = jdata["flag"].get<std::int32_t>() & GatewayProtocal::FLAG_NOT_CALL_ENCODE;
    std::string body = jdata["body"].get<std::string>();
    if(!raw) {
        // TODO 调用encode方法，不清楚encode到底是做什么
        // body = 
    }
    // TODO 目前只支持ws发送
    Send2Client(scid.c_str(), body, FD_WRITE);
    return 0;
}

int CmdSendToGroup::ExecCmd()
{
    int raw = jdata["flag"].get<std::int32_t>() & GatewayProtocal::FLAG_NOT_CALL_ENCODE;
    std::string body = jdata["body"].get<std::string>();
    if(!raw) {
        // TODO raw是什么意思？
    }
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
            // 通过gid找到在线的fd列表
            std::list<std::string> lstFds;
            if (tgg_get_fdsbygid(element.get<std::string>().c_str(), lstFds) < 0) {// 没找到gid
                RTE_LOG(INFO, USER1, "[%s][%d] gid[%s] not exist.\n", __func__, __LINE__, element.get<std::string>().c_str());
                continue;
            }
            // 遍历group中的<fd:idx>列表,根据fd找到cid
            std::list<std::string>::iterator itFd = lstFds.begin();
            while (itFd != lstFds.end()) {
                int fd = get_fd_by_fdidx(*itFd);
                if(fd < 0) {
                    RTE_LOG(INFO, USER1, "[%s][%d] parse fdidx[%s] failed.\n", __func__, __LINE__, (*itFd).c_str());
                    itFd++;
                    continue;
                }
                std::string cid = tgg_get_cli_cid(fd);
                if(cid.empty()) {
                    RTE_LOG(INFO, USER1, "[%s][%d] cid for fd[%d] gid[%s] not exist.\n", 
                        __func__, __LINE__, fd, element.get<std::string>().c_str());
                    itFd++;
                    continue;
                }
                // 确认cid是否要排除
                std::set<std::string>::iterator iter = setExeptCid.find(cid);
                if(iter == setExeptCid.end()) {
                    // 不在排除队列中就加入发送队列
                    lstAllFds.push_back(fd);
                }
                itFd++;
            }
            //std::cout << element << std::endl;
        }
        if(lstAllFds.size() > 0) {
            BatchSend2Client(lstAllFds, body, FD_WRITE);
        }
    } else {
        return -1;
    }
    return 0;
}

int CmdKick::ExecCmd()
{
    std::string scid = get_valid_cid(jdata["connection_id"]);
    std::string body = jdata["body"].get<std::string>();
    Send2Client(scid.c_str(), body, FD_WRITE);
    std::string data = "\x88\x02\x03\xe8";// 关闭websocket
    Send2Client(scid.c_str(), data, FD_WRITE);
    return 0;
}

int CmdDestroy::ExecCmd()
{
    std::string scid = get_valid_cid(jdata["connection_id"]);
    std::string data = "\x88\x02\x03\xe8";// 关闭websocket
    WsConsumer::Send2Client(scid.c_str(), data, FD_WRITE | FD_CLOSE);
    return 0;
}


int CmdSendToALL::ExecCmd()
{
    int raw = jdata["flag"].get<std::int32_t>() & GatewayProtocal::FLAG_NOT_CALL_ENCODE;
    std::string body = jdata["body"];
    if(!raw) {
    }

    std::string ext_data = jdata["ext_data"];
    if(!ext_data.empty()) {
        nlohmann::json jext = nlohmann::json::parse(ext_data);
        std::list<std::string> lstCids;
        if(jext.contains("connections") && jext["connections"].is_array()) {
            // 发送给所有指定的cid
            for (const auto& element : jext["connections"]) {
                // 通过gid找到在线的fd列表
                lstCids.push_back(element);
            }
            if(lstCids.size() > 0) {
                BatchSend2Client(lstCids, body, FD_WRITE);
            }
        } else {
            // 所有在线的客户端fd
            std::list<int> lstFds;
            if(tgg_get_allfds(lstFds) < 0) {
                return -1;
            }
            if(lstFds.size() > 0) {
                BatchSend2Client(lstFds, body, FD_WRITE);
            }
        }
        return 0;
    }

    return -1;
}

void CmdSelect::FormatResult(const std::list<int>& lst_fd, int mask, nlohmann::json& result)
{
    std::list<int>::const_iterator itFd = lst_fd.begin();
    while (itFd != lst_fd.end()) {
        if(*itFd < 0) {
            RTE_LOG(INFO, USER1, "[%s][%d] invalid fd.\n", __func__, __LINE__);
            itFd++;
            continue;
        }
        int coreid = *itFd & 0xf;
        int fd = *itFd >> 8;
        std::string cid = tgg_get_cli_cid(coreid, fd);
        if(cid.empty()) {
            RTE_LOG(INFO, USER1, "[%s][%d] cid for fd[%d] not exist.\n", 
                __func__, __LINE__, fd);
            itFd++;
            continue;
        }
        if(!result.contains(cid)) {
            result[cid] = nlohmann::json::array();
        }
        std::string uid = tgg_get_cli_uid(coreid, fd);
        if(uid.empty()) {
            RTE_LOG(INFO, USER1, "[%s][%d] uid for fd[%d] not exist.\n", 
                __func__, __LINE__, fd);
            itFd++;
            continue;
        }
        if(mask & FIELD_GID) {
            std::list<std::string> lst_gids;
            if (!tgg_get_gidsbyuid(uid.c_str(), lst_gids)) {
                if (!result[cid].contains("groups")) {
                    result[cid]["groups"] = nlohmann::json::array();
                } else {
                                            // 已经填充过了就不要再次执行了
                    RTE_LOG(INFO, USER1, "[%s][%d] cid[%s] groups already exist.\n", 
                        __func__, __LINE__, cid.c_str());
                }
                std::list<std::string>::iterator itGid = lst_gids.begin();
                while(itGid != lst_gids.end()) {
                    result[cid]["groups"].push_back(*itGid);
                    itGid++;
                }
            }
        }
        if(mask & FIELD_UID) {
            if (!result[cid].contains("uid")) {
                result[cid]["uid"] = uid;
            } else {
                // 已经填充过了就不要再次执行了
                RTE_LOG(INFO, USER1, "[%s][%d] cid[%s] groups already exist.\n", 
                    __func__, __LINE__, cid.c_str());
            }
        }
        itFd++;
    }
}

int CmdSelect::ExecCmd()
{
    std::string ext_data = jdata["ext_data"];
    nlohmann::json result = nlohmann::json::array();
    if(ext_data.empty()) {
        std::string data = Php_Serialize(result);
        Send2BW(data);
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
        result = nlohmann::json::array();
        //std::map<int, std::map<std::string, std::string>> client_info_array;
        if (!where.is_null()) {
            for (auto& it : where.items()) {
                const std::string& key = it.key();
                if (key!= "connection_id") {// json数据格式不一样，所以要区分一下
                    // group user session [123123213213,123123123123]
                    auto& items = it.value();
                        
                    for (const auto& item : items) {// item为gid,uid等  where 条件中的item
                        // 通过gid获取该group下的所有fd
                        std::list<std::string> lst_fd;
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
                        // 遍历每个fd，获取fd对应的cid uid 和 gids
                        std::list<int> lst_fds;
                        std::list<std::string>::iterator itFd = lst_fd.begin();
                        while (itFd != lst_fd.end()) {
                            int fd = get_fd_by_fdidx(*itFd);
                            if(fd > 0) {
                                lst_fds.push_back(fd);
                            } else {
                                RTE_LOG(ERR, USER1, "[%s][%d] invalid fd_idx format:%s\n",
                                 __func__, __LINE__, itFd->c_str());
                            }
                            itFd++;
                        }
                        FormatResult(lst_fds, mask, result);
                    }
                } else {
                    // cid {"9527":9527}
                    for (const auto& connection_id : it.value()) {
                        std::string scid = get_valid_cid(connection_id);
                        int fd = tgg_get_fdbycid(scid.c_str());
                        if (fd > 0) {
                            std::list<int> lst_fds;
                            lst_fds.push_back(fd);
                            FormatResult(lst_fds, mask, result);
                        }
                    }
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
        std::cerr << "Error parsing data: " << e.what() << std::endl;
    }
    std::string data = Php_Serialize(result);
    //BwPackageHandler::encode();
    Send2BW(data);
    return 0;
}

int CmdGetGroupIdList::ExecCmd()
{
    std::list<std::string> lst_gid;
    if (tgg_get_allonlinegids(lst_gid) < 0) {
        RTE_LOG(INFO, USER1, "[%s][%d] get all online gids failed.\n", __func__, __LINE__);
    }
    nlohmann::json result = nlohmann::json::array();
    std::list<std::string>::iterator it = lst_gid.begin();
    while (it != lst_gid.end()) {
        result.push_back(*it);
        it++;
    }
    std::string data = Php_Serialize(result);
    Send2BW(data);
    return 0;
}

int CmdSetSession::ExecCmd()
{
    std::string ext_data = jdata["ext_data"];
    std::string scid = get_valid_cid(jdata["connection_id"]);
    if(ext_data.empty() || scid.empty()) {
        RTE_LOG(INFO, USER1, "[%s][%d] set session failed, ext_data[%s] and scid[%s] shouldn't be empty.\n",
                 __func__, __LINE__, ext_data.c_str(), scid.c_str());
        return -1;
    }
    int clifdx = tgg_get_fdbycid(scid.c_str());
    if(clifdx < 0) {
        RTE_LOG(INFO, USER1, "[%s][%d] get clifdx by cid[%s] failed.\n", __func__, __LINE__, scid.c_str());
        return -1;
    }
    return tgg_set_cli_reserved(clifdx & 0xf, clifdx >> 8, ext_data.c_str());
}

int CmdUpdateSession::ExecCmd()
{
    // TODO 稍微有点复杂，且当前拿不到数据
    // std::string scid = get_valid_cid(jdata["connection_id"]);
    // if(ext_data.empty() || scid.empty()) {
    //     RTE_LOG(INFO, USER1, "[%s][%d] set session failed, ext_data[%s] and scid[%s] shouldn't be empty.\n",
    //              __func__, __LINE__, ext_data.c_str(), scid.c_str());
    //     return -1;
    // }
    // int clifdx = tgg_get_fdbycid(scid.c_str());
    // if(clifdx < 0) {
    //     RTE_LOG(INFO, USER1, "[%s][%d] get fd by cid[%s] failed.\n", __func__, __LINE__, scid.c_str());
    //     return -1;
    // }
    // int coreid = clifdx & 0xf;
    // int clifd = clifdx >> 8
    // std::string ext_data = jdata["ext_data"];
    // std::string session = tgg_get_cli_reserved(coreid, clifd);
    // if(session.empty()) {
    //     if (tgg_set_cli_reserved(coreid, clifd, ext_data.c_str()) < 0) {
    //         RTE_LOG(INFO, USER1, "[%s][%d] update session failed cid[%s] session[%s] failed.\n", 
    //                 __func__, __LINE__, scid.c_str(), session.c_str());
    //         return -1;
    //     }
    //     return 0;
    // }
    // nlohmann::json jsession = Php_UnSerialize(session);
    // nlohmann::json jsession_for_merge = Php_UnSerialize(ext_data);
    return 0;
}

int CmdIsOnline::ExecCmd()
{
    std::string result = "i:";
    std::string scid = get_valid_cid(jdata["connection_id"]);
    int clifdx = tgg_get_fdbycid(scid.c_str());
    if(clifdx < 0) {
        result += "0";
    } else {
        result += "1";
    }
    Send2BW(result);
    return 0;
}

int CmdBindUid::ExecCmd()
{
    // std::string s_uid = std::to_string(jdata["user_id"].get<std::uint64_t>());
    // return tgg_bind_session(this->fd, s_uid.c_str(), tgg_get_cli_cid(this->fd).c_str());
    // TODO Binduid到底是客户端过来消息绑定，还是服务端过来消息绑定
    std::string suid = std::to_string(jdata["user_id"].get<std::uint64_t>());
    std::string scid = get_valid_cid(jdata["connection_id"]);
    if(suid.empty() || scid.empty()) {
        RTE_LOG(INFO, USER1, "[%s][%d] bind uid failed, uid[%s] and cid[%s] shouldn't be empty.\n",
                 __func__, __LINE__, suid.c_str(), scid.c_str());
        return -1;
    }
    int fdx = tgg_get_fdbycid(scid.c_str());
    if(fdx < 0) {
        RTE_LOG(INFO, USER1, "[%s][%d] get fd by cid[%s] failed.\n", __func__, __LINE__, scid.c_str());
        return -1;
    }

    return tgg_bind_session(fdx & 0xf, fdx >> 8, suid.c_str(), scid.c_str());

}

int CmdUnBindUid::ExecCmd()
{
    std::string suid = std::to_string(jdata["user_id"].get<std::uint64_t>());
    std::string scid = get_valid_cid(jdata["connection_id"]);
    if(suid.empty() || scid.empty()) {
        RTE_LOG(INFO, USER1, "[%s][%d] bind uid failed, uid[%s] and cid[%s] shouldn't be empty.\n",
                 __func__, __LINE__, suid.c_str(), scid.c_str());
        return -1;
    }
    int fdx = tgg_get_fdbycid(scid.c_str());
    if(fdx < 0) {
        RTE_LOG(INFO, USER1, "[%s][%d] get fdx by cid[%s] failed.\n", __func__, __LINE__, scid.c_str());
        return -1;
    }
    return tgg_free_session(fdx & 0xf, fdx >> 8);
}


int CmdSendToUid::ExecCmd()
{
    bool raw = jdata["flag"].get<std::int32_t>() & GatewayProtocal::FLAG_NOT_CALL_ENCODE;
    std::string body = jdata["body"];
    if (!raw) {
        raw = true;
    }
    std::list<int> lst_fds;
    std::vector<std::string> uids = jdata["ext_data"].get<std::vector<std::string> >();
    for(auto& it : uids) {
        std::list<std::string> lst_fd;
        if (tgg_get_fdsbyuid(it.c_str(), lst_fd) < 0) {
            continue;
        }
        // 遍历每个fd，获取fd对应的cid uid 和 gids
        std::list<std::string>::iterator itFd = lst_fd.begin();
        while (itFd != lst_fd.end()) {
            int fd = get_fd_by_fdidx(*itFd);
            if(fd > 0) {
                lst_fds.push_back(fd);
            } else {
                RTE_LOG(ERR, USER1, "[%s][%d] invalid fd_idx format:%s\n",
                 __func__, __LINE__, itFd->c_str());
            }
            itFd++;
        }
    }
    if(lst_fds.size() > 0) {
        BatchSend2Client(lst_fds, body, FD_WRITE);
    }
    return 0;
}


int CmdJoinGroup::ExecCmd()
{
    std::string group = jdata["ext_data"];
    std::string scid = get_valid_cid(jdata["connection_id"]);
    if(group.empty() || scid.empty()) {
        RTE_LOG(INFO, USER1, "[%s][%d] set session failed, ext_data[%s] and scid[%s] shouldn't be empty.\n",
                 __func__, __LINE__, group.c_str(), scid.c_str());
        return -1;
    }
    int fd = tgg_get_fdbycid(scid.c_str());
    if(fd < 0) {
        RTE_LOG(INFO, USER1, "[%s][%d] get fd by cid[%s] failed.\n", __func__, __LINE__, scid.c_str());
        return -1;
    }
    tgg_join_group(scid.c_str(), group.c_str());
    return 0;
}


int CmdLeaveGroup::ExecCmd()
{
    std::string group = jdata["ext_data"];
    std::string scid = get_valid_cid(jdata["connection_id"]);
    if(group.empty() || scid.empty()) {
        RTE_LOG(INFO, USER1, "[%s][%d] set session failed, ext_data[%s] and scid[%s] shouldn't be empty.\n",
                 __func__, __LINE__, group.c_str(), scid.c_str());
        return -1;
    }
    int fd = tgg_get_fdbycid(scid.c_str());
    if(fd < 0) {
        RTE_LOG(INFO, USER1, "[%s][%d] get fd by cid[%s] failed.\n", __func__, __LINE__, scid.c_str());
        return -1;
    }
    tgg_exit_group(scid.c_str(), group.c_str());
    return 0;
}

int CmdUnGroup::ExecCmd()
{
    std::string group = jdata["ext_data"];
    if(group.empty()) {
        RTE_LOG(INFO, USER1, "[%s][%d] set session failed, group[%s] shouldn't be empty.\n",
                 __func__, __LINE__, group.c_str());
        return -1;
    }
    tgg_del_gid(group.c_str());
    tgg_del_gid_uidgid(group.c_str());
    return 0;
}

int CmdGetClientSessionsByGroup::ExecCmd()
{
    nlohmann::json result = nlohmann::json::array();
    std::string group = jdata["ext_data"];
    if(group.empty()) {
        RTE_LOG(INFO, USER1, "[%s][%d] set session failed, group[%s] shouldn't be empty.\n",
                 __func__, __LINE__, group.c_str());
        std::string data = Php_Serialize(result);
        Send2BW(data);
        return -1;
    }
    std::list<std::string> lst_sfd;
    if (tgg_get_fdsbygid(group.c_str(), lst_sfd) > 0) {
        std::list<std::string>::iterator itFd = lst_sfd.begin();
        while (itFd != lst_sfd.end()) {
            int clidfdx = get_fd_by_fdidx(*itFd);
            if(clidfdx < 0) {
                itFd++;
                continue;
            }
            int coreid = clidfdx & 0xf;
            int fd = clidfdx >> 8;
            std::string scid = tgg_get_cli_cid(coreid, fd);
            if(scid.length() <= TGG_IPPORT_LEN) {
                RTE_LOG(INFO, USER1, "[%s][%d] cid[%s] length should be longger than %d.\n",
                    __func__, __LINE__, scid.c_str(), TGG_IPPORT_LEN);
                itFd++;
                continue;
            }
            std::string connection_id = scid.substr(TGG_IPPORT_LEN);// cid的前12位是ip和port，后面的才是connection_id
            std::string session = tgg_get_cli_reserved(coreid, fd);
            result[connection_id] = session;
            itFd++;
        }
    }
    std::string data = Php_Serialize(result);
    Send2BW(data);
    return 0;
}


int CmdGetClientCountByGroup::ExecCmd()
{
    nlohmann::json result = 0;
    std::string group = jdata["ext_data"];
    if(group.empty()) {
        RTE_LOG(INFO, USER1, "[%s][%d] set session failed, group[%s] shouldn't be empty.\n",
                 __func__, __LINE__, group.c_str());
        std::string data = Php_Serialize(result);
        Send2BW(data);
        return -1;
    }
    std::list<std::string> lst_sfd;
    int count = 0;// TODO  前期调试需要排查格式等问题，后期应该直接计算lst_sfd的长度即可
    if (tgg_get_fdsbygid(group.c_str(), lst_sfd) < 0) {
        std::list<std::string>::iterator itFd = lst_sfd.begin();
        while (itFd != lst_sfd.end()) {
            int clifdx = get_fd_by_fdidx(*itFd);
            if(clifdx < 0) {
                RTE_LOG(INFO, USER1, "[%s][%d] gid_fd_string[%s] invalid.\n",
                    __func__, __LINE__, itFd->c_str());
                // TODO 已知格式错误，是否可以直接删除这个节点
                itFd++;
                continue;
            }
            std::string scid = tgg_get_cli_cid(clifdx & 0xf, clifdx >> 8);
            if(scid.length() <= TGG_IPPORT_LEN) {
                RTE_LOG(INFO, USER1, "[%s][%d] cid[%s] length should be longger than %d.\n",
                    __func__, __LINE__, scid.c_str(), TGG_IPPORT_LEN);
                itFd++;
                continue;
            }
            count++;
            itFd++;
        }
    }
    result = count;
    std::string data = Php_Serialize(result);
    Send2BW(data);
    return 0;

}

int CmdGetClientIdByUid::ExecCmd()
{
    nlohmann::json result = nlohmann::json::array();
    std::string data;
    std::string suid = jdata["ext_data"];
    if(suid.empty()) {
        RTE_LOG(INFO, USER1, "[%s][%d] set session failed, uid[%s] shouldn't be empty.\n",
                 __func__, __LINE__, suid.c_str());
        data = Php_Serialize(result);
        Send2BW(data);
        return -1;
    }
    std::list<std::string> lst_sfd;
    if (tgg_get_fdsbyuid(suid.c_str(), lst_sfd) < 0) {
        std::list<std::string>::iterator itFd = lst_sfd.begin();
        while (itFd != lst_sfd.end()) {
            int clifdx = get_fd_by_fdidx(*itFd);
            if(clifdx < 0) {
                itFd++;
                continue;
            }
            std::string scid = tgg_get_cli_cid(clifdx & 0xf, clifdx >> 8);
            if(scid.length() <= TGG_IPPORT_LEN) {
                RTE_LOG(INFO, USER1, "[%s][%d] cid[%s] length should be longger than %d.\n",
                    __func__, __LINE__, scid.c_str(), TGG_IPPORT_LEN);
                itFd++;
                continue;
            }
            std::string connection_id = scid.substr(TGG_IPPORT_LEN);// cid的前12位是ip和port，后面的才是connection_id
            result.push_back(connection_id);
            itFd++;
        }
    }

    data = Php_Serialize(result);
    Send2BW(data);
    return 0;
}

int CmdBatchGetClientIdByUid::ExecCmd()
{
    nlohmann::json result = nlohmann::json::array();
    std::string data;
    nlohmann::json juid = nlohmann::json::parse(jdata["ext_data"].get<std::string>());
    std::vector<std::string> vec_uids = juid.get<std::vector<std::string> >();
    for(auto& it : vec_uids) {
        std::list<std::string> lst_sfd;
        result[it] = nlohmann::json::array();
        if (tgg_get_fdsbyuid(it.c_str(), lst_sfd) < 0) {
            std::list<std::string>::iterator itFd = lst_sfd.begin();
            while (itFd != lst_sfd.end()) {
                int clifdx = get_fd_by_fdidx(*itFd);
                if(clifdx < 0) {
                    itFd++;
                    continue;
                }
                std::string scid = tgg_get_cli_cid(clifdx & 0xf, clifdx >> 8);
                if(scid.length() <= TGG_IPPORT_LEN) {
                    RTE_LOG(INFO, USER1, "[%s][%d] cid[%s] length should be longger than %d.\n",
                        __func__, __LINE__, scid.c_str(), TGG_IPPORT_LEN);
                    itFd++;
                    continue;
                }
                std::string connection_id = scid.substr(TGG_IPPORT_LEN);// cid的前12位是ip和port，后面的才是connection_id
                result[it].push_back(connection_id);
                itFd++;
            }
        } 
    }
    data = Php_Serialize(result);
    Send2BW(data);
    return 0;
}

static int json_parse_body(nlohmann::json& jdata)//const std::string& jdata, std::string& result)
{
    int cmd = 0;
    std::string result;
    nlohmann::json obj;
    std::string body = jdata["body"].get<std::string>();
    if(body.empty()) {
        return 0;
    }
    try {
        obj = nlohmann::json::parse(jdata["body"].get<std::string>());
        if(!obj.contains("cmd")) {// 没有cmd就不需要解包
            return 0;
        }
        printf("body: %s\n", obj.dump(4).c_str());
        cmd = obj["cmd"].get<std::int32_t>();
    } catch (const nlohmann::json::parse_error& e) {
        RTE_LOG(ERR, USER1, "[%s][%d] parse json error:%s\n", __func__, __LINE__, e.what());
        return -1;
    }
    if (cmd) {
        if (s_is_open_binary) {
            result = obj["data"].get<std::string>();
        } else {
            std::string bin = Encrypt::hex2bin(obj["data"].get<std::string>());
            if (bin.length() <= 0) {
                RTE_LOG(ERR, USER1, "[%s][%d] hex2bin failed:%s.\n",
                    __func__, __LINE__, obj["data"].get<std::string>().c_str());
            }
            if (message_pack(cmd, 1, 2,
                (uint8_t)s_compress_flag, bin, result) < 0) {
                RTE_LOG(ERR, USER1, "[%s][%d] message_pack failed:%s.\n", 
                    __func__, __LINE__, bin.c_str());
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
        RTE_LOG(ERR, USER1, "[%s][%d] data fram length[%d] check failed, read buf_size[%d].\n", 
            __func__, __LINE__, pack_len, bdata->data_len);
        return false;
    }
    // cmd 范围校验
    if(bwdata->cmd > CMD_MAX_INDEX || bwdata->cmd <= 0) {
        RTE_LOG(ERR, USER1, "[%s][%d] cmd check failed, invalid cmd[%d].\n", 
            __func__, __LINE__, bwdata->cmd);
        return false;
    }
    // 扩展长度校验
    if(ext_len > pack_len - sizeof(tgg_bw_protocal)) {
        RTE_LOG(ERR, USER1, "[%s][%d] ext_len[%d] check failed, pack_len[%d].\n", 
            __func__, __LINE__, ext_len, pack_len);
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

    printf("jdata:%s\n", jdata.dump(4).c_str());

    // 首次连接判断
    int cmd = jdata["cmd"].get<std::int32_t>();
    int authorized = tgg_get_bwfdx_authorized(prc_id, fd);
    if (!authorized && cmd != CMD_WORKER_CONNECT && 
        cmd != CMD_GATEWAY_CLIENT_CONNECT) {
        free_bw_session(prc_id, fd);
        close(fd);
        RTE_LOG(ERR, USER1, "[%s][%d] command[%d] error or not authorized[%d].\n", 
            __func__, __LINE__, cmd, authorized);
        return ;
    }

    // TODO 这里的逻辑还不确定到底是什么意思，上行数据，待调试
    json_parse_body(jdata); //{jdata["data"].get<std::string>(), pack_data) < 0) {
    switch(cmd) {
        case CMD_WORKER_CONNECT:
            pro = new CmdWorkerConnect(prc_id, fd, data, jdata);
            break;
        case CMD_GATEWAY_CLIENT_CONNECT:
            pro = new CmdGatewayClientConnect(prc_id, fd, data, jdata);
            break;
        // GatewayClient连接Gateway
            // return new CmdGatewayClientConnect(prc_id, fd, data, jdata);
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
            RTE_LOG(ERR, USER1, "[%s][%d] Gateway inner pack err, Unknown cmd=%d.\n", __func__, __LINE__, cmd);
            break;
    }
    if(pro) {
        pro->ExecCmd();
        delete pro;
        pro = NULL;
    }
}

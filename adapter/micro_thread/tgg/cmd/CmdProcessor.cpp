#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <algorithm>

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
int g_need_authorize = 1;// bw的连接是否要先校验权限(第一个命令必须是200或202)

// 辅助函数：将 rapidjson::Value 转换为字符串
// bForLog 是否作为日志打印使用，作为日志打印时，非DEBUG情况下直接返回空字符串，防止性能损耗
static std::string rapidjson_to_string(const rapidjson::Value& val, bool bForLog = true) {
    if(bForLog && AsyncLogger::getInstance().getloglevel() != LogLevel::DEBUG) {
        return "";
    }
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    val.Accept(writer);
    return buffer.GetString();
}

static void get_body_string(const rapidjson::Value& jdata, std::string& body)
{
    if(jdata["body"].IsString()) {
        body = jdata["body"].GetString();
    } else if (jdata["body"].IsUint64()) {
        const char* sbody = reinterpret_cast<char*>(jdata["body"].GetUint64());
        int body_len = jdata["body_len"].GetInt();
        if(body_len) {
            body = std::string(sbody, body_len);
        }
    }  
}

void CmdBaseProcessor::Send2BW(const rapidjson::Value& data, bool serialize)
{
    std::string result = serialize ? Php_Serialize(data) : rapidjson_to_string(data, false);
    int len = big_endian() ? htonl(result.size()) : result.size();
    std::string rsp;
    rsp.resize(sizeof(int));
    memcpy(const_cast<char* >(rsp.data()), &len, sizeof(int));
    rsp += result;
    int ret = write(this->fd, rsp.c_str(), rsp.size());
    if(ret < 0) {
        LOG_ERROR("send data[%s] to BW failed.", result.c_str());        
    }
}

int CmdWorkerConnect::ExecCmd()
{
    std::string bwSeckey = TggConfigure::getInstance()->get_secret_key();// tgg_get_bwfdx_seckey(this->prc_id, this->fd);
    try {
        std::string body;
        get_body_string(jdata, body);
        rapidjson::Document worker_info;
        worker_info.Parse(body.c_str());
        if (worker_info.HasParseError()) {
            LOG_ERROR("WorkerConnect: JSON parse error");
            this->need_close = 1;
            // close(this->fd);
            return -1;
        }
        if (!worker_info.HasMember("secret_key") || !worker_info.HasMember("worker_key")) {
            LOG_ERROR("WorkerConnect: no Worker key found.");
            this->need_close = 1;
            // close(this->fd);
            return -1;
        }
        if (std::string(worker_info["secret_key"].GetString()) != bwSeckey) {
            LOG_ERROR("WorkerConnect:  Worker key[%s] does not match conn key[%s].", 
                worker_info["secretKey"].GetString(), bwSeckey.c_str());
            this->need_close = 1;
            // close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
            return -1;
        }
        char ip_str[INET_ADDRSTRLEN];
        struct in_addr addr;
        addr.s_addr = ((tgg_bw_data*)data)->peer_ip; 
        if (!inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str))) {// 获取远端ip port 失败
            LOG_ERROR("WorkerConnect: get remote info failed, fd:[%d].", this->fd);
            this->need_close = 1;
            // close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
            return -1;
        }
        LOG_INFO("New WorkerConnect: ip:%s:%u", ip_str, ((tgg_bw_data*)data)->peer_port);
        std::string bwWokerkey = ip_str;
        bwWokerkey += ":";
        bwWokerkey += worker_info["worker_key"].GetString();
        if (tgg_check_bwwkkey_exist(bwWokerkey.c_str()) >= 0) {// 在一台服务器上businessWorker->name不能相同
            this->need_close = 1;
            // close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
            // tgg_close_bw_session(this->prc_id, this->fd);
            LOG_ERROR("WorkerConnect: bw[%s] already exist.", bwWokerkey.c_str());
            return -1;
        }
        // tgg_add_bwwkkey(bwWokerkey.c_str());
        tgg_new_bw_session(this->prc_id, this->fd, GatewayProtocal::CMD_WORKER_CONNECT
            , bwWokerkey.c_str(), ((tgg_bw_data*)data)->peer_ip, ((tgg_bw_data*)data)->peer_port);
        // 初始化完成后，加入到客户端可选服务池中以便网关能将客户端连接绑定到该bw
        if (tgg_add_bwfdx(generate_bwfdx(this->prc_id, this->fd)) < 0) {
            // 如果加入失败，就要销毁连接，否则这个服务就没有人使用
            tgg_close_bw_session(this->prc_id, this->fd);
            this->need_close = 1;
            // close(this->fd);
            LOG_ERROR("WorkerConnect: add bw[%d] fd[%d] failed.", prc_id, fd);
            return -1;
        }
        LOG_DEBUG("WorkerConnect: added bw[prc:%d,fd:%d] success, total bw count:%d.", 
            prc_id, fd, tgg_get_bwfdx_count());
    } catch (...) {
    // 捕获其他任何未预料到的异常
        LOG_ERROR("Exception catched.");
        this->need_close = 1;
        // close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
        // free_bw_session(this->prc_id, this->fd);
        return -1;
    }
    return 0;
}

int CmdGatewayClientConnect::ExecCmd()
{
    std::string bwSeckey = tgg_get_bwfdx_seckey(this->prc_id, this->fd);
    try {
        // uint32_t remote_ip; 
        // ushort remote_port;
        char ip_str[INET_ADDRSTRLEN];
        struct in_addr addr;
        addr.s_addr = ((tgg_bw_data*)data)->peer_ip; 
        if (!inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str))) {// 获取远端ip port 失败
            LOG_ERROR("GatewayClientConnect:get remote info failed, fd:%d.", this->fd);
            this->need_close = 1;
            // close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
            return -1;
        }
        // printf("jdata:%s\n", jdata.dump(4).c_str());
        LOG_INFO("New GatewayClientConnect: ip:%s:%u", ip_str, ((tgg_bw_data*)data)->peer_port);
        std::string body;
        get_body_string(jdata, body);
        LOG_INFO("GatewayClientConnect:JSON parse:%s", body.c_str());
        rapidjson::Document worker_info;
        worker_info.Parse(body.c_str());
        if (worker_info.HasParseError()) {
            LOG_ERROR("GatewayClientConnect:JSON parse error:%s", body.c_str());
            this->need_close = 1;
            // close(this->fd);
            return -1;
        }
        if (!worker_info.HasMember("secret_key")) {
            LOG_ERROR("GatewayClientConnect:no Worker key found.");
            this->need_close = 1;
            // close(this->fd);
            return -1;
        }
        if (std::string(worker_info["secret_key"].GetString()) != bwSeckey) {
            LOG_ERROR("GatewayClientConnect: Worker key[%s] does not match conn key[%s].", 
                worker_info["secretKey"].GetString(), bwSeckey.c_str());
            this->need_close = 1;
            // close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
            return -1;
        }
        LOG_DEBUG("GatewayClientConnect: cmd executed body:%s.", body.c_str());
    } catch (...) {
    // 捕获其他任何未预料到的异常
        LOG_ERROR("Exception catched.");
        this->need_close = 1;
        // close(this->fd);// 连接还没有缓存到内存中，不需要清理，直接关闭fd就行
        // free_bw_session(this->prc_id, this->fd);
        return -1;
    }
    // CMD_GATEWAY_CLIENT_CONNECT 类型的连接没有workerkey
    tgg_new_bw_session(this->prc_id, this->fd, GatewayProtocal::CMD_GATEWAY_CLIENT_CONNECT, "");
    return 0;
}

int CmdSendToOne::ExecCmd()
{
    int cid = jdata["connection_id"].GetInt();
    int raw = true;//jdata["flag"].get<std::int32_t>() & GatewayProtocal::FLAG_NOT_CALL_ENCODE;
    std::string body;
    get_body_string(jdata, body);
  // TODO 目前只支持ws发送
    LOG_DEBUG("SendToOne: cmd executed cid[%d] data:%s.", cid, bin2hex(body).c_str());
    Send2Client(cid, body, FD_WRITE, !raw);
    return 0;
}

int CmdSendToGroup::ExecCmd()
{
    int raw = true; // 原始标志位 //jdata["flag"].get<std::int32_t>() & GatewayProtocal::FLAG_NOT_CALL_ENCODE;
    std::string body;
    get_body_string(jdata, body);

    // 解析 ext_data
    rapidjson::Document ext_data;
    ext_data.Parse(jdata["ext_data"].GetString());
    if (ext_data.HasParseError()) {
        LOG_ERROR("Failed to parse ext_data");
        return -1;
    }

    // 构建排除cid集合
    std::set<int> setExeptCid;
    if (ext_data.HasMember("exclude") && ext_data["exclude"].IsObject()) {
        const rapidjson::Value& excludeObj = ext_data["exclude"];
        for (rapidjson::Value::ConstMemberIterator itr = excludeObj.MemberBegin(); 
             itr != excludeObj.MemberEnd(); ++itr) {
            // 提取键（需转为字符串）
            // const char* key = itr->name.GetString();
            // 提取值（需检查类型）
            if (itr->value.IsInt()) {
                // int value = itr->value.GetInt();
                setExeptCid.insert(itr->value.GetInt());
            } else {
                LOG_ERROR("Invalid type of value for key:%s", itr->name.GetString());
            }
        }
    }

    // 收集待发送的fd列表
    std::list<int64_t> lstAllFds;
    if (ext_data.HasMember("group") && ext_data["group"].IsArray()) {
        const rapidjson::Value& groupArray = ext_data["group"];
        for (rapidjson::SizeType i = 0; i < groupArray.Size(); i++) {
            const char* gid = groupArray[i].GetString();
            std::list<int64_t> lstFds;
            if (tgg_get_fdsbygid(gid, lstFds) < 0) {
                LOG_DEBUG("gid[%s] not exist.", gid);
                continue;
            }

            for (int64_t fdidcid : lstFds) {
                if (fdidcid < 0) {
                    LOG_WARNING("Invalid fdidcid[%lld] for gid[%s].", fdidcid, gid);
                    continue;
                }

                int cid = GET_CID_FDCID_MASK(fdidcid);
                if (cid <= 0) {
                    LOG_WARNING("cid for fdidcid[%lld] gid[%s] not exist.", fdidcid, gid);
                    continue;
                }

                if (setExeptCid.find(cid) == setExeptCid.end()) {
                    lstAllFds.push_back(fdidcid);
                }
            }
        }

        if (!lstAllFds.empty()) {
            BatchSend2ClientByfds(lstAllFds, body, FD_WRITE, !raw);
            
            // 日志优化：直接记录gid数量而非完整JSON[1](@ref)
            LOG_DEBUG("SendToGroup: cmd executed for %d groups", groupArray.Size());
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
    int cid = jdata["connection_id"].GetInt();
    // std::string body = jdata["body"].get<std::string>();
    int raw = true;//jdata["flag"].get<std::int32_t>() & GatewayProtocal::FLAG_NOT_CALL_ENCODE;
    // Send2Client(cid, body, FD_WRITE, !raw);
    Send2Client(cid, "kick", FD_WRITE|FD_CLOSE, !raw);
    int64_t fdidcid = tgg_get_fdbycid(cid);
    if(fdidcid <= 0) {
        LOG_DEBUG("Kick: cmd executed failed, get fdidcid[%ld] by cid[%d] failed.", fdidcid, cid);
        return -1;
    }
    tgg_free_session(GET_COREID_FDCID_MASK(fdidcid), GET_FD_FDCID_MASK(fdidcid), cid);
    LOG_DEBUG("Kick: cmd executed cid[%d].", cid);
    return 0;
}

int CmdDestroy::ExecCmd()
{
    int cid = jdata["connection_id"].GetInt();
    int raw = true;//jdata["flag"].get<std::int32_t>() & GatewayProtocal::FLAG_NOT_CALL_ENCODE;
    Send2Client(cid, "destroy", FD_WRITE|FD_CLOSE, !raw);// TODO 是否要立即销毁，不发送ws的关闭帧(去掉FD_WRITE就行)了
    int64_t fdidcid = tgg_get_fdbycid(cid);
    tgg_free_session(GET_COREID_FDCID_MASK(fdidcid), GET_FD_FDCID_MASK(fdidcid), cid);
    LOG_DEBUG("Destroy: cmd executed cid[%d].", cid);
    return 0;
}


int CmdSendToALL::ExecCmd()
{
    int raw = true;
    std::string body;
    get_body_string(jdata, body);

    std::list<int> lstCids;
    std::string ext_data = jdata["ext_data"].GetString();  // 直接获取字符串值

    if (!ext_data.empty()) {
        // 创建 RapidJSON 文档对象
        rapidjson::Document jext;
        jext.Parse(ext_data.c_str());  // 解析 JSON 字符串

        // 检查解析是否成功且包含 connections 数组
        if (!jext.HasParseError() && 
            jext.HasMember("connections") && 
            jext["connections"].IsArray()) 
        {
            const rapidjson::Value& connections = jext["connections"];
            // 遍历数组元素
            for (rapidjson::SizeType i = 0; i < connections.Size(); i++) {
                if (connections[i].IsInt()) {  // 确保元素是整数
                    lstCids.push_back(connections[i].GetInt());
                }
            }

            if (!lstCids.empty()) {
                BatchSend2ClientBycids(lstCids, body, FD_WRITE, !raw);
            }
        }
        LOG_DEBUG("SendToALL: cmd executed cids[%s] body:%s.", ext_data.c_str(), bin2hex(body).c_str());
        return 0;
    }

    // 所有在线的客户端fd
    std::list<int64_t> lstFds;
    if (tgg_get_allfds(lstFds) < 0) {
        LOG_WARNING("SendToALL: get all online clients failed.");
        return -1;
    }

    if (!lstFds.empty()) {
        BatchSend2ClientByfds(lstFds, body, FD_WRITE, !raw);
    }

    LOG_DEBUG("SendToALL: sendto all clients, extend:%s.", ext_data.c_str());
    return 0;
}

void CmdSelect::FormatResult(const std::list<int64_t>& lst_fd, int mask, rapidjson::Document& result)
{
    // 获取分配器引用（关键优化点）
    rapidjson::Document::AllocatorType& allocator = result.GetAllocator();
    
    std::list<int64_t>::const_iterator itFd = lst_fd.begin();
    while (itFd != lst_fd.end()) {
        if(*itFd < 0) {
            LOG_WARNING("invalid fd.");
            itFd++;
            continue;
        }
        
        int coreid = GET_COREID_FDCID_MASK(*itFd);
        int fd = GET_FD_FDCID_MASK(*itFd);
        int cid = GET_CID_FDCID_MASK(*itFd);
        std::string scid = std::to_string(cid);
        
        // 检查并创建 CID 对象（使用 rapidjson API）
        if(!result.HasMember(scid.c_str())) {
            rapidjson::Value cidObj(rapidjson::kObjectType);
            result.AddMember(
                rapidjson::Value(scid.c_str(), allocator).Move(),
                cidObj,
                allocator
            );
        }
        
        rapidjson::Value& cidObj = result[scid.c_str()];
        std::string uid = tgg_get_cli_uid(coreid, fd);
        
        if(uid.empty()) {
            LOG_WARNING("[%s][%d] uid for fd[%d] not exist.", fd);
            itFd++;
            continue;
        }
        
        // 处理 GROUP ID 字段
        if(mask & FIELD_GID) {
            std::set<std::string> set_gids;
            if (!tgg_get_gidsbyuid(uid.c_str(), set_gids)) {
                if (!cidObj.HasMember("groups")) {
                    rapidjson::Value groups(rapidjson::kArrayType);
                    cidObj.AddMember("groups", groups, allocator);
                } else {
                    LOG_WARNING("cid[%d] groups already exist.", cid);
                }
                
                rapidjson::Value& groupsArray = cidObj["groups"];
                for (auto& gid : set_gids) {
                    groupsArray.PushBack(
                        rapidjson::Value(gid.c_str(), allocator).Move(),
                        allocator
                    );
                }
            }
        }
        
        // 处理 UID 字段
        if(mask & FIELD_UID) {
            if (!cidObj.HasMember("uid")) {
                cidObj.AddMember(
                    "uid",
                    rapidjson::Value(uid.c_str(), allocator).Move(),
                    allocator
                );
            } else {
                LOG_WARNING("cid[%d] uid already exist.", cid);
            }
        }
        
        itFd++;
    }
}

int CmdSelect::ExecCmd()
{
    // 获取 ext_data 字段
    const rapidjson::Value& extDataValue = jdata["ext_data"];
    std::string ext_data;
    if (extDataValue.IsString()) {
        ext_data = extDataValue.GetString();
    }

    // 创建结果文档和分配器
    rapidjson::Document result(rapidjson::kObjectType);
    // rapidjson::Document::AllocatorType& allocator = result.GetAllocator();

    if (ext_data.empty()) {
        LOG_WARNING("Select cmd, extend data is empty");
        Send2BW(result);
        return 0;
    }

    try {
        // 解析 ext_data
        rapidjson::Document jext_data;
        jext_data.Parse(ext_data.c_str());
        if (jext_data.HasParseError()) {
            LOG_ERROR("JSON parse error in ext_data");
            return -1;
        }

        // 处理 fields 数组
        std::vector<std::string> fields;
        if (jext_data.HasMember("fields") && jext_data["fields"].IsArray()) {
            const rapidjson::Value& fieldsArray = jext_data["fields"];
            for (rapidjson::SizeType i = 0; i < fieldsArray.Size(); i++) {
                fields.push_back(fieldsArray[i].GetString());
            }
        }

        // 设置字段掩码
        int mask = 0;
        for (auto& it : fields) {
            if (it == "cid") mask |= FIELD_CID;
            else if (it == "uid") mask |= FIELD_UID;
            else if (it == "gid") mask |= FIELD_GID;
        }

        // 处理 where 条件
        result.SetObject();
        if (jext_data.HasMember("where") && !jext_data["where"].IsNull()) {
            const rapidjson::Value& where = jext_data["where"];
            
            for (rapidjson::Value::ConstMemberIterator it = where.MemberBegin(); 
                 it != where.MemberEnd(); ++it) 
            {
                const std::string key = it->name.GetString();
                const rapidjson::Value& value = it->value;
                
                if (key != "connection_id") {
                    // 处理 groups 和 uid 条件
                    if (value.IsArray()) {
                        for (rapidjson::SizeType i = 0; i < value.Size(); i++) {
                            std::list<int64_t> lst_fd;
                            const char* item = value[i].GetString();
                            
                            if (key == "groups") {
                                if (tgg_get_fdsbygid(item, lst_fd) < 0) continue;
                            } 
                            else if (key == "uid") {
                                if (tgg_get_fdsbyuid(item, lst_fd) < 0) continue;
                            }
                            
                            if (!lst_fd.empty()) {
                                FormatResult(lst_fd, mask, result);
                            }
                        }
                    }
                } 
                else {
                    // 处理 connection_id
                    std::list<int64_t> lst_fds;
                    if (value.IsArray()) {
                        for (rapidjson::SizeType i = 0; i < value.Size(); i++) {
                            int cid = value[i].GetInt();
                            int64_t fdidcid = tgg_get_fdbycid(cid);
                            if (fdidcid > 0) {
                                lst_fds.push_back(fdidcid);
                            }
                        }
                    }
                    FormatResult(lst_fds, mask, result);
                }
            }
        } 
        else {
            // 处理全局条件
            std::list<int64_t> lst_fds;
            if (!tgg_get_allfds(lst_fds)) {
                if (!lst_fds.empty()) {
                    FormatResult(lst_fds, mask, result);
                }
            }
        }
    } 
    catch (const std::exception& e) {
        LOG_ERROR("Error parsing data: %s", e.what());
    }

    Send2BW(result);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    result.Accept(writer);
    LOG_DEBUG("Select: cmd executed data: %s", buffer.GetString());
    
    return 0;
}

int CmdGetGroupIdList::ExecCmd()
{
    std::list<std::string> lst_gid;
    if (tgg_get_allonlinegids(lst_gid) < 0) {
        LOG_WARNING("get all online gids failed.");
    }

    // 创建 rapidjson 文档（数组类型）
    rapidjson::Document result(rapidjson::kArrayType);
    rapidjson::Document::AllocatorType& allocator = result.GetAllocator(); // 获取分配器

    // 遍历列表并添加到 JSON 数组
    for (const auto& gid : lst_gid) {
        // 将 std::string 转换为 rapidjson::Value（需显式拷贝）
        rapidjson::Value val;
        val.SetString(gid.c_str(), allocator);
        result.PushBack(val, allocator); // 添加到数组[1,7](@ref)
    }

    // 发送序列化后的字符串
    Send2BW(result); // 需确保 Send2BW 支持 std::string 参数[1,7](@ref)
    
    // 记录日志（直接使用序列化字符串）
    LOG_DEBUG("GetGroupIdList: cmd executed data:%s.", rapidjson_to_string(result).c_str());
    return 0;
}

int CmdSetSession::ExecCmd()
{
    std::string session = jdata["ext_data"].GetString();
    int cid = jdata["connection_id"].GetInt();
    if(cid <= 0) {
        LOG_ERROR("set session failed, invalid cid[%d].", cid);
        return -1;
    }
    int64_t fdidcid = tgg_get_fdbycid(cid);
    if(fdidcid < 0) {
        LOG_ERROR("get fdidcid by cid[%d] failed.", cid);
        return -1;
    }
    // 判断是不是有效的 php序列化后的字符串
    if(session.length() > 2 && session[1] != ':') {
        LOG_ERROR("get fdidcid by cid[%d] failed.", cid);
        return -1;
    }
    LOG_DEBUG("SetSession: cmd executed cid[%d] data:%s.", cid, session.c_str());
    return tgg_set_cli_reserved(GET_COREID_FDCID_MASK(fdidcid), GET_FD_FDCID_MASK(fdidcid), session.c_str());
}

int CmdGetSessionByCid::ExecCmd()
{
    rapidjson::Document result;
    result.SetObject(); // 初始化为空对象
    rapidjson::Document::AllocatorType& allocator = result.GetAllocator();
    int cid = jdata["connection_id"].GetInt();
    int64_t fdicid = -1;
    std::string session;
    if(cid <= 0) {
        LOG_ERROR("set session failed, invalid cid[%d].", cid);
        goto SEND_GET_SESSION;
    }
    fdicid = tgg_get_fdbycid(cid);
    if(fdicid <= 0) {
        LOG_ERROR("get fdicid by cid[%d] failed.", cid);
        goto SEND_GET_SESSION;
    }
    session = tgg_get_cli_reserved(GET_COREID_FDCID_MASK(fdicid), GET_FD_FDCID_MASK(fdicid));
    if(session.empty()) {
        result.SetArray(); // 设为空数组
        LOG_INFO("session is empty of cid[%d].", cid);
        goto SEND_GET_SESSION;
    }
    result.SetString(session.c_str(), allocator);
    LOG_DEBUG("GetSession: cmd executed cid[%d] data:%s.", cid, session.c_str());
    Send2BW(result);
    return 0;

SEND_GET_SESSION:
    Send2BW(result);
    return 0;
}

int CmdGetAllClientSession::ExecCmd()
{
    rapidjson::Document result;
    result.SetObject();
    rapidjson::Document::AllocatorType& allocator = result.GetAllocator();

    std::list<int64_t> lst_fds;
    tgg_get_allfds(lst_fds);
    for (auto fdidcid : lst_fds) {
        std::string session = tgg_get_cli_reserved(GET_COREID_FDCID_MASK(fdidcid), GET_FD_FDCID_MASK(fdidcid));
        int cid = tgg_get_cli_cid(GET_COREID_FDCID_MASK(fdidcid), GET_FD_FDCID_MASK(fdidcid));
        std::string scid = std::to_string(cid);
        // 添加节点对象 { "cid": "session_data" }
        result.AddMember(
            rapidjson::Value().SetString(scid.c_str(), scid.size(), allocator), 
            rapidjson::Value().SetString(session.c_str(), session.size(), allocator), 
            allocator
        );
    }
    LOG_DEBUG("GetAllClientSession: cmd executed data:%s.", rapidjson_to_string(result).c_str());
    Send2BW(result);
    return 0;
}

static void json_replace_recursive(rapidjson::Value& target, 
                                  const rapidjson::Value& source,
                                  rapidjson::Document::AllocatorType& allocator) {
    // 处理数组类型
    if (target.IsArray() && source.IsArray()) {
        for (rapidjson::SizeType i = 0; i < source.Size(); i++) {
            const rapidjson::Value& source_item = source[i];
            if (!source_item.IsObject() || source_item.MemberCount() != 1) 
                continue;
                
            // 获取键值对
            auto it = source_item.MemberBegin();
            const char* key = it->name.GetString();
            const rapidjson::Value& value = it->value;
            
            // 在target中查找相同键
            bool found = false;
            for (rapidjson::SizeType j = 0; j < target.Size(); j++) {
                rapidjson::Value& target_item = target[j];
                if (target_item.IsObject() && target_item.HasMember(key)) {
                    found = true;
                    // 递归合并
                    json_replace_recursive(target_item[key], value, allocator);
                    break;
                }
            }
            
            // 未找到则添加新对象
            if (!found) {
                rapidjson::Value new_item(rapidjson::kObjectType);
                new_item.AddMember(
                    rapidjson::Value().SetString(key, strlen(key), allocator), 
                    rapidjson::Value(value, allocator), 
                    allocator
                );
                target.PushBack(new_item, allocator);
            }
        }
    } 
    // 处理对象类型
    else if (target.IsObject() && source.IsObject()) {
        for (auto it = source.MemberBegin(); it != source.MemberEnd(); ++it) {
            const char* key = it->name.GetString();
            if (target.HasMember(key) && 
                target[key].IsObject() && 
                it->value.IsObject()) {
                // 递归合并嵌套对象
                json_replace_recursive(target[key], it->value, allocator);
            } else {
                // 直接覆盖值
                target.AddMember(
                    rapidjson::Value().SetString(key, strlen(key), allocator), 
                    rapidjson::Value(it->value, allocator), 
                    allocator
                );
            }
        }
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
    int cid = jdata["connection_id"].GetInt();
    if(cid <= 0) {
        LOG_ERROR("set session failed, invalid cid[%d].",  cid);
        return -1;
    }
    int64_t fdidcid = tgg_get_fdbycid(cid);
    if(fdidcid < 0) {
        LOG_ERROR("get fd by cid[%d] failed.", cid);
        return -1;
    }
    int coreid = GET_COREID_FDCID_MASK(fdidcid);
    int clifd = GET_FD_FDCID_MASK(fdidcid);
    std::string ext_data = jdata["ext_data"].GetString();
    std::string session = tgg_get_cli_reserved(coreid, clifd);
    if(session.empty()) {
        if (tgg_set_cli_reserved(coreid, clifd, ext_data.c_str()) < 0) {
            LOG_ERROR("update session failed cid[%d] session[%s] failed.", cid, session.c_str());
            return -1;
        }
        return 0;
    }
    // 反序列化PHP字符串（假设Php_UnSerialize返回rapidjson::Document）
    rapidjson::Document jsession = Php_UnSerialize(session);
    rapidjson::Document jsession_for_merge = Php_UnSerialize(ext_data);
    
    // 递归合并JSON
    rapidjson::Document result = Php_ArrayReplaceRecursive(jsession, jsession_for_merge, jsession.GetAllocator());
    
    // 序列化回PHP格式
    std::string data = Php_Serialize(result.GetObject());
    tgg_set_cli_reserved(coreid, clifd, data.c_str());
    
    LOG_DEBUG("UpdateSession: cmd executed cid[%d] data:%s.", cid, data.c_str());
    return 0;
}

int CmdIsOnline::ExecCmd()
{
    rapidjson::Document result;
    int cid = jdata["connection_id"].GetInt();
    int clifdx = tgg_get_fdbycid(cid);
    if(clifdx <= 0) {
        result.SetString("0");
    } else {
        result.SetString("1");
    }
    LOG_DEBUG("IsOnline: send cid[%d] IsOnline result[%s] to server.", cid, rapidjson_to_string(result).c_str());
    Send2BW(result);
    return 0;
}

int CmdBindUid::ExecCmd()
{
    // std::string s_uid = std::to_string(jdata["user_id"].get<std::uint64_t>());
    // return tgg_bind_session(this->fd, s_uid.c_str(), tgg_get_cli_cid(this->fd).c_str());
    // TODO Binduid到底是客户端过来消息绑定，还是服务端过来消息绑定
    std::string suid = jdata["ext_data"].GetString();
    int cid = jdata["connection_id"].GetInt();
    if(suid.empty() || cid < 0) {
        LOG_ERROR("bind uid failed, uid[%s] and cid[%d] shouldn't be empty.", suid.c_str(), cid);
        return -1;
    }
    LOG_DEBUG("BindUid: cid[%d] bind to uid[%s].", cid, suid.c_str());
    return tgg_bind_session(suid.c_str(), cid);

}

int CmdUnBindUid::ExecCmd()
{
    int cid = jdata["connection_id"].GetInt();
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
    bool raw = true;//jdata["flag"].get<std::int32_t>() & GatewayProtocal::FLAG_NOT_CALL_ENCODE;
    std::string body;
    get_body_string(jdata, body);
    std::list<int64_t> lst_fds;
// 1. 获取ext_data字符串
    std::string ext_data_str = jdata["ext_data"].GetString(); // 直接获取字符串[1,4](@ref)

    // 2. 解析JSON字符串为rapidjson文档
    rapidjson::Document juid;
    juid.Parse(ext_data_str.c_str());
    if (juid.HasParseError() || !juid.IsArray()) { // 检查解析结果[6,8](@ref)
        LOG_WARNING("SendToUid: invalid ext_data format");
        return -1;
    }

    // 3. 提取UID数组
    std::vector<std::string> vec_uids;
    for (rapidjson::SizeType i = 0; i < juid.Size(); i++) { // 遍历数组[6](@ref)
        if (juid[i].IsString()) {
            vec_uids.push_back(juid[i].GetString());
        } else {
            LOG_WARNING("SendToUid: non-string element in UID array at index %d", i);
        }
    }

    // 4. 收集所有UID对应的文件描述符
    for (auto& it : vec_uids) {
        std::list<int64_t> lst_fd;
        if (tgg_get_fdsbyuid(it.c_str(), lst_fd) < 0) {
            LOG_DEBUG("SendToUid: no fd found for uid[%s]", it.c_str());
            continue;
        }
        lst_fds.splice(lst_fds.end(), lst_fd);
    }

    // 5. 批量发送数据
    if (!lst_fds.empty()) {
        BatchSend2ClientByfds(lst_fds, body, FD_WRITE, !raw);
        LOG_DEBUG("SendToUid: cmd exec success. Sent to %zu fds", lst_fds.size());
    } else {
        LOG_DEBUG("SendToUid: no fd found for all uids[%s]", ext_data_str.c_str());
    }
    return 0;
}


int CmdJoinGroup::ExecCmd()
{
    std::string group = jdata["ext_data"].GetString();
    int cid = jdata["connection_id"].GetInt();
    if(group.empty() || cid <= 0) {
        LOG_ERROR("set session failed, ext_data[%s] and cid[%d] shouldn't be empty.", group.c_str(), cid);
        return -1;
    }
    int fdid = tgg_get_fdbycid(cid);
    if(fdid < 0) {
        RTE_LOG(INFO, USER1, "[%s][%d] get fdid by cid[%d] failed.", __FILE__, __LINE__, cid);
        return -1;
    }
    std::vector<std::string> vec_group;
    if(group[0] == '[' && group[group.size()-1] == ']') {// 是数组
        group[0] = ' ';// 首位的中括号换成空格
        group[group.size()-1] = ' ';
        group.erase(std::remove(group.begin(), group.end(), '\"'), group.end());// 去掉 "
        group.erase(std::remove(group.begin(), group.end(), ' '), group.end()); // 去掉空格
        split_string(group, ',', vec_group);
    } else {
        vec_group.push_back(group);
    }
    for(auto group_unit : vec_group) {
        tgg_join_group(group_unit.c_str(), cid);
    }
    LOG_DEBUG("JoinGroup: cmd executed cid[%d] gid[%s].", cid, group.c_str());
    return 0;
}


int CmdLeaveGroup::ExecCmd()
{
    std::string group = jdata["ext_data"].GetString();
    int cid = jdata["connection_id"].GetInt();
    if(group.empty() || cid <= 0) {
        LOG_ERROR("set session failed, ext_data[%s] and cid[%d] shouldn't be empty.", group.c_str(), cid);
        return -1;
    }
    int fdid = tgg_get_fdbycid(cid);
    if(fdid < 0) {
        LOG_ERROR("get fdid by cid[%d] failed.", cid);
        return -1;
    }
    std::vector<std::string> vec_group;
    if(group[0] == '[' && group[group.size()-1] == ']') {// 是数组
        group[0] = ' ';// 首位的中括号换成空格
        group[group.size()-1] = ' ';
        group.erase(std::remove(group.begin(), group.end(), '\"'), group.end());// 去掉 "
        group.erase(std::remove(group.begin(), group.end(), ' '), group.end()); // 去掉空格
        split_string(group, ',', vec_group);
    } else {
        vec_group.push_back(group);
    }
    for(auto group_unit : vec_group) {
        tgg_exit_group(group_unit.c_str(), cid);
    }
    LOG_DEBUG("LeaveGroup: cmd executed cid[%d] gid[%s].", cid, group.c_str());
    return 0;
}

int CmdUnGroup::ExecCmd()
{
    std::string group = jdata["ext_data"].GetString();
    if(group.empty()) {
        LOG_ERROR("ungroup failed, group[%s] shouldn't be empty.",
                 __FILE__, __LINE__, group.c_str());
        return -1;
    }

    std::vector<std::string> vec_group;
    if(group[0] == '[' && group[group.size()-1] == ']') {// 是数组
        group[0] = ' ';// 首位的中括号换成空格
        group[group.size()-1] = ' ';
        group.erase(std::remove(group.begin(), group.end(), '\"'), group.end());// 去掉 "
        group.erase(std::remove(group.begin(), group.end(), ' '), group.end()); // 去掉空格
        split_string(group, ',', vec_group);
    } else {
        vec_group.push_back(group);
    }
    for(auto group_unit : vec_group) {
        tgg_del_gid_cidgid(group_unit.c_str());// 这里顺序不能动，得先删除hash<cid,gid>中的部分，才能删除hash<gid,list<fdid>>
        tgg_del_gid(group_unit.c_str());
    }
    LOG_DEBUG("UnGroup: cmd executed gid[%s].", group.c_str());
    return 0;
}

int CmdGetClientSessionsByGroup::ExecCmd()
{
    rapidjson::Document result;
    result.SetObject();
    rapidjson::Document::AllocatorType& allocator = result.GetAllocator();
    
    std::string group = jdata["ext_data"].GetString();
    if(group.empty()) {
        LOG_ERROR("get session by group failed, group[%s] shouldn't be empty.", group.c_str());
        Send2BW(result);
        return -1;
    }
    std::list<int64_t> lst_sfd;
    if (!tgg_get_fdsbygid(group.c_str(), lst_sfd)) {
        std::list<int64_t>::iterator itFd = lst_sfd.begin();
        while (itFd != lst_sfd.end()) {
            int coreid = GET_COREID_FDCID_MASK(*itFd);
            int fd = GET_FD_FDCID_MASK(*itFd);
            int cid = GET_CID_FDCID_MASK(*itFd);
            if(cid <= 0) {
                LOG_WARNING("invalid cid[%d].", cid);
                itFd++;
                continue;
            }
            std::string connection_id = std::to_string(cid);// cid的前12位是ip和port，后面的才是connection_id
            std::string session = tgg_get_cli_reserved(coreid, fd);
            result.AddMember(
                rapidjson::Value().SetString(connection_id.c_str(), connection_id.size(), allocator),
                rapidjson::Value().SetString(session.c_str(), session.size(), allocator),
                allocator
            );
            itFd++;
        }
    }
    Send2BW(result);
    LOG_DEBUG("GetClientSessionsByGroup: cmd executed gid[%s] data:%s.", group.c_str(), rapidjson_to_string(result).c_str());
    return 0;
}


int CmdGetClientCountByGroup::ExecCmd()
{
    rapidjson::Document result;
    result.SetInt(0);
    std::string group = jdata["ext_data"].GetString();
    if(group.empty()) {
        std::list<int64_t> lst_cid;
        tgg_get_allonlinecids(lst_cid);
        result.SetInt(lst_cid.size());
        LOG_DEBUG("GetAllClientCount:%s.", rapidjson_to_string(result).c_str());
        Send2BW(result);
        return 0;
    }
    std::list<int64_t> lst_sfd;
    int count = 0;// TODO  前期调试需要排查格式等问题，后期应该直接计算lst_sfd的长度即可
    if (!tgg_get_fdsbygid(group.c_str(), lst_sfd)) {
        count = lst_sfd.size();
    }
    result.SetInt(count);
    Send2BW(result);
    LOG_DEBUG("GetClientCountByGroup: cmd executed gid[%s] data:%s.", group.c_str(), rapidjson_to_string(result).c_str());
    return 0;
}

int CmdGetClientIdByUid::ExecCmd()
{
    rapidjson::Document result;
    result.SetArray(); // 创建数组
    rapidjson::Document::AllocatorType& allocator = result.GetAllocator();
    std::string data;
    std::string suid = jdata["ext_data"].GetString();
    if(suid.empty()) {
        LOG_ERROR("get session by uid failed, uid[%s] shouldn't be empty.", suid.c_str());
        Send2BW(result);
        return -1;
    }
    std::list<int64_t> lst_sfd;
    if (tgg_get_fdsbyuid(suid.c_str(), lst_sfd) == 0) {
        std::list<int64_t>::iterator itFd = lst_sfd.begin();
        while (itFd != lst_sfd.end()) {
            int cid = GET_CID_FDCID_MASK(*itFd);//tgg_get_cli_cid(*itFd & 0xff, *itFd >> 8);
            if(cid < 0) {
                LOG_ERROR("invalid cid[%d].", cid);
                itFd++;
                continue;
            }
            // std::string connection_id = std::to_string(cid);
            result.PushBack(cid, allocator);
            itFd++;
        }
    } else {
        LOG_ERROR("no session found for uid[%s].", suid.c_str());
    }

    Send2BW(result);
    LOG_DEBUG("GetClientIdByUid: cmd executed uid[%s] data:%s.", suid.c_str(), rapidjson_to_string(result).c_str());
    return 0;
}

int CmdBatchGetClientIdByUid::ExecCmd()
{
    rapidjson::Document result;
    result.SetArray(); // 外层数组
    rapidjson::Document::AllocatorType& allocator = result.GetAllocator();

    // 解析ext_data中的JSON数组
    std::string ext_data = jdata["ext_data"].GetString();
    rapidjson::Document juid;
    juid.Parse(ext_data.c_str());
    
    if(!juid.IsArray()) {
        LOG_ERROR("Invalid uid array format");
        return -1;
    }

    for (rapidjson::SizeType i = 0; i < juid.Size(); i++) {
        const char* uid = juid[i].GetString();
        rapidjson::Value uid_obj(rapidjson::kObjectType);
        rapidjson::Value arr(rapidjson::kArrayType);
        
        std::list<int64_t> lst_sfd;
        if (tgg_get_fdsbyuid(uid, lst_sfd) == 0) {
            for (auto fdid : lst_sfd) {
                int cid = GET_CID_FDCID_MASK(fdid);
                if(cid >= 0) {
                    arr.PushBack(cid, allocator);
                }
            }
        }
        
        uid_obj.AddMember(
            rapidjson::StringRef(uid),
            arr,
            allocator
        );
        result.PushBack(uid_obj, allocator);
    }

    Send2BW(result);
    LOG_DEBUG("BatchGetClientIdByUid: cmd executed data:%s.", rapidjson_to_string(result).c_str());
    return 0;
}

static int json_parse_body(unsigned char flag, rapidjson::Document& jdata)
{
    int cmd = 0;
    std::string result;
    rapidjson::Document obj; // 替换nlohmann::json为rapidjson::Document
    rapidjson::Document::AllocatorType& allocator = obj.GetAllocator();
    rapidjson::Document::AllocatorType& jallocator = jdata.GetAllocator();

    // 1. 获取body指针和长度
    uintptr_t body_ptr = jdata["body"].GetUint64(); // 直接获取uintptr_t
    const char* body = reinterpret_cast<char*>(body_ptr);
    int body_len = jdata["body_len"].GetInt();

    if(body_len <= 2) {
        LOG_DEBUG("invalid body length:%d.", body_len);
        return 0;
    }

    // 2. 检查body格式
    if(body_len > 2 && body[1] != 0x3a && body[0] != 0x7b) {
        std::string print_data;
        if(body[0] == 0xff && body[1] == 0xfe) {
            message_unpack(body, print_data);
        } else {
            print_data.assign(body, body_len); // 避免拷贝
        }
        LOG_DEBUG("send to cli data:%s", bin2hex(print_data).c_str());
        return 0;
    }

    // 3. JSON解析逻辑
    try {
        if(!flag) {
            // 假设Php_UnSerialize返回rapidjson::Document
            obj.CopyFrom(Php_UnSerialize(body), allocator);
        } else {
            // 直接解析body [1,4](@ref)
            obj.Parse(body, body_len);
            if(obj.HasParseError()) {
                throw std::runtime_error("Parse error");
            }
        }

        // 4. 序列化JSON并存入jdata
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        obj.Accept(writer);
        jdata["body"].SetString(buffer.GetString(), jallocator); // 替换原指针为字符串
        
        LOG_DEBUG("body: %s", buffer.GetString());

        // 5. 检查cmd字段
        if(!obj.HasMember("cmd")) {
            LOG_DEBUG("no cmd found in body");
            return 0;
        }
        cmd = obj["cmd"].GetInt();
    } catch (const std::exception& e) {
        LOG_ERROR("parse error:%s", e.what());
        return -1;
    }

    // 6. 处理cmd逻辑
    if (cmd) {
        const char* data_str = obj["data"].GetString();
        if (s_is_open_binary) {
            result = data_str;
        } else {
            std::string bin = hex2bin(data_str);
            if (bin.empty()) {
                LOG_ERROR("hex2bin failed:%s.", data_str);
                return -1;
            }
            if (message_pack(cmd, 1, 2, s_compress_flag, bin, result) < 0) {
                LOG_ERROR("message_pack failed");
            }
        }
        // 更新jdata的body字段 [1](@ref)
        jdata["body"].SetString(result.c_str(), jallocator);
    }

    // 7. 更新body_len
    jdata["body_len"] = strlen(jdata["body"].GetString());
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

int exec_cmd_processor(int prc_id, int fd, void* data)
{
    //std::string json_str = R"({"name": "Jane Smith", "age": 25, "is_student": true})";
    tgg_bw_data* bdata = (tgg_bw_data*)data;
    tgg_bw_protocal* bwdata = (tgg_bw_protocal*)bdata->data;
    if(!bwdata_frame_check(bdata, bwdata)) {
        return -1;
    }
    CmdBaseProcessor* pro = NULL;
    rapidjson::Document jdata;
    // 解析帧并生成json对象
    BwPackageHandler::decode(bwdata, jdata);

    LOG_DEBUG("jdata:%s", rapidjson_to_string(jdata).c_str());

        // 首次连接判断
    int cmd = jdata["cmd"].GetInt();
    int authorized = g_need_authorize ? tgg_get_bwfdx_authorized(prc_id, fd) : 1;
    if (!authorized && cmd != CMD_WORKER_CONNECT && cmd != CMD_GATEWAY_CLIENT_CONNECT) {
        tgg_close_bw_session(prc_id, fd);
        // close(fd);
        LOG_ERROR("command[%d] error or not authorized[%d].", cmd, authorized);
        return -1;
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
            return -1;
            break;
    }
    if(pro) {
        pro->ExecCmd();
        if(pro->NeedClose() < 0) {
            delete pro;
            pro = NULL;
            return -1;
        }
        delete pro;
        pro = NULL;
    }
    return 0;
}


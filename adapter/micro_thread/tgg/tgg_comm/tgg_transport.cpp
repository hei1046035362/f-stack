#include "comm/log.hpp"
#include <map>
#include <iostream>
#include "tgg_transport.h"
#include "comm/common.hpp"
#include "tgg_comm/tgg_bw_cache.h"
#include "tgg_bwcomm.h"
#include "tgg_comm/tgg_common.h"
#include "comm/Websocket.hpp"
#include "tgg_conf.h"

void Send2Fd(int core_id, int fd, int idx, std::string_view data, int fd_opt, int encode)
{
    // 共享数据指针（避免重复打包）
    auto shared_data = std::make_shared<const std::string>([&]{
        std::string packData;
        std::string sendData;
        if(fd_opt & FD_CLOSE) {
            sendData = Websocket::EncodeCloseFrame(data);
        } else {
            // 打包封装
            if (encode && message_pack(2, 1, 0, 1, data, packData) < 0)
            {
                LOG_ERROR("message_pack data[%s] failed.", data.data());
                return sendData;
            }
            sendData = Websocket::EncodeWebsocketMessage(BINARY_FRAME, encode ? packData : data);
        }
        return sendData;
    }());
    
    if (shared_data->empty()) return;

    LOG_DEBUG("send data coreid:%d fd:%d idx:%d, data:%s.", core_id, fd, idx, bin2hex(std::string_view(*shared_data)).c_str());
    if (enqueue_data_single_fd(core_id, shared_data, fd, idx, fd_opt) < 0) {// 函数内部会循环尝试发送10次
        LOG_ERROR("Enqueue data Failed: coreid:%d fd:%d idx:%d,opt:%d", core_id, fd, idx, fd_opt);
    }

}

void Send2Client(int cid, std::string_view data, int fd_opt, int encode)
{
    int64_t fdidcid = tgg_get_fdbycid(cid);
    if(fdidcid <= 0) {
        LOG_ERROR("client[%d] not exist.", cid);
        return;
    }
    int fd = GET_FD_FDCID_MASK(fdidcid);
    int core_id = GET_COREID_FDCID_MASK(fdidcid);
    int idx = GET_IDX_FDCID_MASK(fdidcid);// 取低24位// tgg_get_cli_idx(core_id, fd);
    Send2Fd(core_id, fd, idx, data, fd_opt, encode);
}

void BatchSend2ClientBycids(std::vector<int>& cids, std::string_view data, int fd_opt, int encode)
{
    std::vector<int64_t> lstFds;
    std::vector<int>::iterator itCid = cids.begin();
    while(itCid != cids.end()) {
        int fdidcid = tgg_get_fdbycid(*itCid);
        if(fdidcid <= 0) {
            LOG_ERROR("client[%d] not exist.", *itCid);
            continue;
        }
        lstFds.push_back(fdidcid);
        itCid++;
    }
    BatchSend2ClientByfds(lstFds, data, fd_opt, encode);
}

void BatchSend2ClientByfds(const std::vector<int64_t>& fds, 
                           std::string_view data, 
                           int fd_opt, 
                           int encode) 
{
    // ==================== 1. 输入校验与预检查 ====================
    if (fds.empty()) {
        LOG_ERROR("fd list empty");
        return;
    }
    
    // ==================== 2. 核心分组优化 ====================
    const int coreid_count = count_ones(TggConfigure::getInstance()->get_lcore_mask());
    if (coreid_count <= 0) {
        LOG_ERROR("invalid core count");
        return;
    }

    // 预分配核心分组容器（避免动态扩容）
    static thread_local std::vector<std::vector<int64_t>> core_groups;
    core_groups.resize(coreid_count);
    for (auto& group : core_groups) {
        group.clear();
    }

    // 单次遍历完成分组（O(n) 复杂度）
    for (int64_t fd_cid : fds) { 
        const int core_id = GET_COREID_FDCID_MASK(fd_cid);
        if (core_id >= 0 && core_id < coreid_count) {
            core_groups[core_id].push_back(fd_cid);
        }
    }

    // ==================== 3. 数据打包优化 ====================
    // 共享数据指针（避免重复打包）
    auto shared_data = std::make_shared<const std::string>([&]{
        std::string packData;
        std::string sendData;
        if(fd_opt & FD_CLOSE) {
            sendData = Websocket::EncodeCloseFrame(data);
        } else {
            // 打包封装
            if (encode && message_pack(2, 1, 0, 1, data, packData) < 0)
            {
                LOG_ERROR("message_pack data[%s] failed.", data.data());
                return sendData;
            }
            sendData = Websocket::EncodeWebsocketMessage(BINARY_FRAME, encode ? std::string_view(packData.data()) : data);
        }
        return sendData;
    }());
    
    if (shared_data->empty()) return;

    // ==================== 4. 异步任务投递 ====================
    for (int core_id = 0; core_id < coreid_count; ++core_id) {
        if (core_groups[core_id].empty()) continue;
                
        // 投递到对应核心的任务队列
        if (enqueue_data_batch_fd(core_id, shared_data, std::move(core_groups[core_id]), fd_opt) < 0) {// 函数内部会循环尝试发送10次
            LOG_ERROR("Enqueue failed for core: %d", core_id);
        }
    }
}

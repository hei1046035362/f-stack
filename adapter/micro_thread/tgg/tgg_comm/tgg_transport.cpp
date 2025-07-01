#include "comm/log.hpp"
#include <map>
#include <iostream>
#include "tgg_transport.h"
#include "comm/common.hpp"
#include "tgg_comm/tgg_bw_cache.h"
#include "tgg_bwcomm.h"
#include "tgg_comm/tgg_common.h"
#include "comm/Websocket.hpp"

void Send2Fd(int core_id, int fd, int idx, const std::string& data, int fd_opt, int encode)
{
    std::string packData;
    std::string sendData;
    if(fd_opt & FD_CLOSE) {
        sendData = Websocket::EncodeCloseFrame(data);
    } else {
        // 打包封装
        if (encode && message_pack(2, 1, 0, 1, data, packData) < 0)
        {
            LOG_ERROR("message_pack data[%s] failed.", data.c_str());
            return;
        }
        sendData = Websocket::EncodeWebsocketMessage(BINARY_FRAME, encode ? packData : data);
    }
    LOG_DEBUG("send data coreid:%d fd:%d idx:%d, data:%s.", core_id, fd, idx, bin2hex(sendData).c_str());
    if (enqueue_data_single_fd(core_id, sendData, fd, idx, fd_opt) < 0) {// 函数内部会循环尝试发送10次
        LOG_ERROR("Enqueue data Failed: coreid:%d fd:%d idx:%d,opt:%d", core_id, fd, idx, fd_opt);
    }

}

void Send2Client(int cid, const std::string& data, int fd_opt, int encode)
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

void BatchSend2ClientBycids(std::list<int> cids, const std::string& data, int fd_opt, int encode)
{
    std::list<int64_t> lstFds;
    std::list<int>::iterator itCid = cids.begin();
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

void BatchSend2ClientByfds(std::list<int64_t> fds, const std::string& data, int fd_opt, int encode)
{
    if(fds.size() <= 0) {
        LOG_ERROR("fd list can't be empty.");
        return;
    }
    // 不同的core_id，分到不同的组，发送的时候需要根据core_id发送到不同的队列
    std::map<int, std::list<int64_t> > mapEachcorefds;// map<coreid, fdidcid>
    std::list<int64_t>::iterator itFd = fds.begin();
    while(itFd != fds.end()) {
        mapEachcorefds[GET_COREID_FDCID_MASK(*itFd)].push_back(*itFd);
        itFd++;
    }
    for (auto coreidFds : mapEachcorefds) {
        std::map<int, int> mapFdidx;// map<fd, idx>
        std::list<int64_t>::iterator itFd = coreidFds.second.begin();
        while(itFd != coreidFds.second.end()) {
            int idx = GET_IDX_FDCID_MASK(*itFd);// tgg_get_cli_idx(coreidFds.first, *itFd);
            int fd = GET_FD_FDCID_MASK(*itFd);
            mapFdidx[fd] = idx;
            itFd++;
        }
        if(mapFdidx.size() <= 0) {
            LOG_ERROR("no live fd found for.");
            return;
        }
        std::string packData;
        std::string sendData;
        // 打包封装到
        if (encode && message_pack(2, 1, 0, 1, data, packData) < 0)
        {
            LOG_ERROR("message_pack data[%s] failed.", data.c_str());
            return;
        }
        if(fd_opt & FD_CLOSE) {
            sendData = Websocket::EncodeCloseFrame(encode ? packData : data);
        } else {
            sendData = Websocket::EncodeWebsocketMessage(BINARY_FRAME, encode ? packData : data);
        }
        LOG_DEBUG("send to batch client,coreid:%d data:%s.", coreidFds.first, bin2hex(sendData).c_str());
        if (enqueue_data_batch_fd(coreidFds.first, sendData, mapFdidx, fd_opt) < 0) {// 函数内部会循环尝试发送10次
            LOG_ERROR("Batch Enqueue data Failed.");
        }
    }
}

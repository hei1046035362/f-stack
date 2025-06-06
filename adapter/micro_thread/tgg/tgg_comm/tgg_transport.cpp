#include "comm/log.hpp"
#include <map>
#include <iostream>
#include "tgg_transport.h"
#include "comm/common.hpp"
#include "tgg_comm/tgg_bw_cache.h"
#include "tgg_bwcomm.h"
#include "tgg_comm/tgg_common.h"
#include "comm/Websocket.hpp"

void Send2Client(int cid, const std::string& data, int fd_opt, int encode)
{
    int fdidx = tgg_get_fdbycid(cid);
    if(fdidx < 0) {
        LOG_ERROR("client[%d] not exist.", cid);
        return;
    }
    int fd = fdidx >> 8;
    int core_id = fdidx & 0xf;
    int idx = tgg_get_cli_idx(core_id, fd);
    if(idx < 0) {
        LOG_ERROR("client[%d] already closed.", cid);
        return;
    }
    if(tgg_get_cli_authorized(core_id, fd) != AUTH_TYPE_HANDLESHAKED) {
        LOG_ERROR("Send data to client[%d] should check Token at first.", cid);
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
    LOG_DEBUG("send data cid[%d]:%s", cid, bin2hex(sendData).c_str());
    if (enqueue_data_single_fd(core_id, sendData, fd, idx, fd_opt) < 0) {// 函数内部会循环尝试发送10次
        LOG_ERROR("Enqueue data Failed: cid:%d,opt:%d", cid, fd_opt);
    }
}

void BatchSend2ClientBycids(std::list<int> cids, const std::string& data, int fd_opt, int encode)
{
    std::list<int> lstFds;
    std::list<int>::iterator itCid = cids.begin();
    while(itCid != cids.end()) {
        int fdidx = tgg_get_fdbycid(*itCid);
        if(fdidx < 0) {
            LOG_ERROR("client[%d] not exist.", *itCid);
            continue;
        }
        lstFds.push_back(fdidx);
        itCid++;
    }
    BatchSend2ClientByfds(lstFds, data, fd_opt, encode);
}

void BatchSend2ClientByfds(std::list<int> fds, const std::string& data, int fd_opt, int encode)
{
    if(fds.size() <= 0) {
        LOG_ERROR("fd list can't be empty.");
        return;
    }
    // 不同的core_id，分到不同的组，发送的时候需要根据core_id发送到不同的队列
    std::map<int, std::list<int> > mapEachcorefds;
    std::list<int>::iterator itFd = fds.begin();
    while(itFd != fds.end()) {
        mapEachcorefds[(*itFd) & 0xf].push_back((*itFd) >> 8);
        itFd++;
    }
    for (auto coreidFds : mapEachcorefds) {
        std::map<int, int> mapFdidx;
        std::list<int>::iterator itFd = coreidFds.second.begin();
        while(itFd != coreidFds.second.end()) {
            int idx = tgg_get_cli_idx(coreidFds.first, *itFd);
            if(idx < 0) {
                int cid = tgg_get_cli_cid(coreidFds.first, *itFd);
                LOG_ERROR("client[%d] already closed.", cid);
                itFd++;
                continue;
            }
            if(tgg_get_cli_authorized(coreidFds.first, *itFd) != AUTH_TYPE_HANDLESHAKED) {
                int cid = tgg_get_cli_cid(coreidFds.first, *itFd);
                LOG_ERROR("Send data to client[%d] should check Token at first.", cid);
                itFd++;
                continue;
            }
            mapFdidx[*itFd] = idx;
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
        LOG_DEBUG("send group data:%s.", bin2hex(sendData).c_str());
        if (enqueue_data_batch_fd(coreidFds.first, sendData, mapFdidx, fd_opt) < 0) {// 函数内部会循环尝试发送10次
            LOG_ERROR("Batch Enqueue data Failed.");
        }
    }
}
static int s_enqueued_to_server_count = 0;
int Send2Server(int core_id, int fd, const std::string& data, int fd_opt)
{
    if(tgg_get_bwfdx_count() <= 0) {
        LOG_ERROR("Send data to server Failed: no bw found.");
        return NO_BW_AVALIABLE;
    }
    if (enqueue_data_trans(core_id, fd, data, fd_opt) < 0) {// 函数内部会循环尝试发送10次
        LOG_ERROR("Send data to server Failed,[core:%d][fd:%d] current count:%d.",
         core_id, fd, s_enqueued_to_server_count);
        return SEND_FAILED;
    }
    s_enqueued_to_server_count++;
    LOG_DEBUG("send to server:%s.", bin2hex(data).c_str());
    return SEND_SUCCESS;
}
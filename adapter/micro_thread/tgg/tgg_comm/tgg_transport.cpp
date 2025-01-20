#include <rte_log.h>
#include <map>
#include "tgg_transport.h"
#include "comm/Encrypt.hpp"
#include "tgg_comm/tgg_bw_cache.h"
#include "tgg_bwcomm.h"
#include "tgg_comm/tgg_common.h"

void Send2Client(const char* cid, const std::string& data, int fd_opt)
{
    int fdidx = tgg_get_fdbycid(cid);
    if(fdidx < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d] client[%s] not exist.", __func__, __LINE__, cid);
        return;
    }
    int fd = fdidx >> 8;
    int core_id = fdidx & 0xf;
    int idx = tgg_get_cli_idx(core_id, fd);
    if(idx < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d] client[%s] already closed.", __func__, __LINE__, cid);
        return;
    }
    if(tgg_get_cli_authorized(core_id, fd) != AUTH_TYPE_TOKENCHECKED) {
        RTE_LOG(ERR, USER1, "[%s][%d] Send data to client[%s] should check Token at first.",
            __func__, __LINE__, cid);
        return;
    }
    std::string sendData;
    // 打包封装到
    if (message_pack(2, 1, 0, 1, data, sendData) < 0)
    {
        RTE_LOG(ERR, USER1, "[%s][%d] message_pack data[%s] failed.\r\n", 
            __func__, __LINE__, data.c_str());
        return;
    }

    std::cout << "send data["<< cid <<"]:" << Encrypt::bin2hex(sendData) << std::endl;
    if (enqueue_data_single_fd(core_id, sendData, fd, idx, fd_opt) < 0) {// 函数内部会循环尝试发送10次
        RTE_LOG(ERR, USER1, "[%s][%d] Enqueue data Failed: cid:%s,opt:%d",
         __func__, __LINE__, cid, fd_opt);
    }
}

void BatchSend2Client(std::list<std::string> cids, const std::string& data, int fd_opt)
{
    std::list<int> lstFds;
    std::list<std::string>::iterator itCid = cids.begin();
    while(itCid != cids.end()) {
        int fdidx = tgg_get_fdbycid((*itCid).c_str());
        if(fdidx < 0) {
            RTE_LOG(INFO, USER1, "[%s][%d] client[%s] not exist.", __func__, __LINE__, (*itCid).c_str());
            continue;
        }
        lstFds.push_back(fdidx);
        itCid++;
    }
    BatchSend2Client(lstFds, data, fd_opt);
}

void BatchSend2Client(std::list<int> fds, const std::string& data, int fd_opt)
{
    if(fds.size() <= 0) {
        RTE_LOG(ERR, USER1, "[%s][%d] fd list can't be empty.\r\n", 
            __func__, __LINE__);
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
        std::list<int>::iterator itFd = coreidFds->second.begin();
        while(itFd != coreidFds->second.end()) {
            int idx = tgg_get_cli_idx(coreidFds->first, *itFd);
            if(idx < 0) {
                std::string sCid = tgg_get_cli_cid(coreidFds->first, *itFd);
                RTE_LOG(INFO, USER1, "[%s][%d] client[%s] already closed.\n", __func__, __LINE__, sCid.c_str());
                itFd++;
                continue;
            }
            if(tgg_get_cli_authorized(coreidFds->first, *itFd) != AUTH_TYPE_TOKENCHECKED) {
                std::string sCid = tgg_get_cli_cid(coreidFds->first, *itFd);
                RTE_LOG(INFO, USER1, "[%s][%d] Send data to client[%s] should check Token at first.\n",
                    __func__, __LINE__, sCid.c_str());
                itFd++;
                continue;
            }
            mapFdidx[*itFd] = idx;
            itFd++;
        }
        if(mapFdidx.size() <= 0) {
            RTE_LOG(ERR, USER1, "[%s][%d] no live fd found for.\r\n", 
                __func__, __LINE__);
            return;
        }
        std::string sendData;
        // 打包封装
        if (message_pack(2, 1, 0, 1, data, sendData) < 0)
        {
            RTE_LOG(ERR, USER1, "[%s][%d] message_pack data[%s] failed.\r\n", 
                __func__, __LINE__, data.c_str());
            return;
        }

        std::cout << "send group data:" << Encrypt::bin2hex(sendData) << std::endl;
        if (enqueue_data_batch_fd(coreidFds->first, sendData, mapFdidx, fd_opt) < 0) {// 函数内部会循环尝试发送10次
            RTE_LOG(ERR, USER1, "[%s][%d] Batch Enqueue data Failed\n",
               __func__, __LINE__);
        }
    }
}

void Send2Server(int core_id, int fd, const std::string& data, int fd_opt)
{
    if (enqueue_data_trans(core_id, fd, data, fd_opt) < 0) {// 函数内部会循环尝试发送10次
        RTE_LOG(ERR, USER1, "[%s][%d] Send data to server Failed\n",
               __func__, __LINE__);
    }
}
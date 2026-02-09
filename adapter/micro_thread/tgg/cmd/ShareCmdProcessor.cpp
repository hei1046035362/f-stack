#include "tgg_comm/tgg_common.h"
#include "ShareCmdProcessor.h"
#include "comm/log.hpp"
#include "tgg_comm/tgg_bw_cache.h"
#include "tgg_comm/tgg_transport.h"
#include "GatewayProtocal.h"
#include <set>

int ShareCmdSelect::ExecCmd()
{
    LOG_DEBUG("ShareCmdSelect: prc[%d] cmd start.", this->prc_id);
    
    // 修复：检查任务是否被中止
    if(tgg_get_bwfx_sharecmd_halt(this->data->prc_id, this->data->fd)) {
        LOG_WARNING("ShareCmdSelect: task halted, prc[%d] fd[%d]", this->data->prc_id, this->data->fd);
        return -1;
    }
    
    tgg_vhash_list* gids = this->data->gids;
    tgg_fd_list* lst_fd = NULL;
    while(gids) {
        tgg_fd_list* lst_cur = NULL;
        if(tgg_get_fdsbygid(gids->vhash, &lst_cur) < 0) {
            LOG_ERROR("ShareCmdSelect:get fds by gid failed.");
            // 获取失败，强制终止任务
            tgg_set_bwfx_sharecmd_halt(this->data->prc_id, this->data->fd, 1);
            iter_del_list<tgg_fd_list>(lst_fd);
            return -1;
        }
        if(!lst_cur) {
            gids = gids->next;
            continue;
        }
            
        if(!lst_fd) {
            lst_fd = lst_cur;
        } else {
            tgg_fd_list* lst_tmp = lst_cur;
            while(lst_tmp->next) {
                lst_tmp = lst_tmp->next;
            }
            lst_tmp->next = lst_fd;// 把之前的列表插入到当前列表的末尾
            lst_fd = lst_cur;
        }
        gids = gids->next;
    }
    
    // 修复：返回结果前再次检查任务是否被中止
    if(tgg_get_bwfx_sharecmd_halt(this->data->prc_id, this->data->fd)) {
        LOG_WARNING("ShareCmdSelect: task halted after processing, prc[%d] fd[%d]", this->data->prc_id, this->data->fd);
        iter_del_list<tgg_fd_list>(lst_fd);
        return -1;
    }
    
    return tgg_add_bwfdx_gid_result(this->data->prc_id, this->data->fd, this->data->time, lst_fd);
}

int ShareCmdJoinGroup::ExecCmd()
{
    LOG_INFO("ShareCmdJoinGroup: prc[%d] cmd start.", this->prc_id);
    
    // 修复：检查任务是否被中止
    if(tgg_get_bwfx_sharecmd_halt(this->data->prc_id, this->data->fd)) {
        LOG_WARNING("ShareCmdJoinGroup: task halted, prc[%d] fd[%d]", this->data->prc_id, this->data->fd);
        return -1;
    }
    
    tgg_vhash_list* gid = this->data->gids;
    int64_t fdidcid = *((int64_t*)this->data->snddata);
    while(gid) {
        if(fdidcid <= 0 || tgg_add_gid(gid->vhash, fdidcid) < 0) {
            LOG_ERROR("add cid[%u] for gid[%llu] failed.", this->data->cid, gid->vhash);
            tgg_set_bwfx_sharecmd_halt(this->data->prc_id, this->data->fd, 1);
            return -1;
        }
        gid = gid->next;
    }
    return 0;
}

int ShareCmdLeaveGroup::ExecCmd()
{
    LOG_INFO("ShareCmdLeaveGroup: prc[%d] cmd start.", this->prc_id);
    
    // 修复：检查任务是否被中止
    if(tgg_get_bwfx_sharecmd_halt(this->data->prc_id, this->data->fd)) {
        LOG_WARNING("ShareCmdLeaveGroup: task halted, prc[%d] fd[%d]", this->data->prc_id, this->data->fd);
        return -1;
    }
    
    tgg_vhash_list* gids = this->data->gids;
    int64_t fdidcid = *((int64_t*)this->data->snddata);
    while(gids) {
        if(fdidcid <= 0 || tgg_del_fd4gid(gids->vhash, fdidcid) < 0) {
            LOG_ERROR("del cid[%u] fdidcid[%lld] for gid[%llu] failed.", this->data->cid, gids->vhash, fdidcid);
            tgg_set_bwfx_sharecmd_halt(this->data->prc_id, this->data->fd, 1);
            return -1;
        }
        gids = gids->next;
    }
    return 0;
}

int ShareCmdUnGroup::ExecCmd()
{
    LOG_INFO("ShareCmdUnGroup: prc[%d] cmd start.", this->prc_id);
    std::string gid((char*)this->data->snddata, this->data->snddata_len);
    if(!gid.empty()) {
        LOG_INFO("ShareCmdUnGroup: try to ungroup gid[%s].", gid.c_str());
        tgg_del_gid_cidgid(gid.c_str());// 这里顺序不能动，得先删除hash<cid,gid>中的部分，才能删除hash<gid,list<fdid>>
        tgg_del_gid(gid.c_str());
    }
    return 0;
}

int ShareCmdSendToGroup::ExecCmd()
{
    LOG_INFO("ShareCmdSendToGroup: prc[%d] cmd start.", this->prc_id);
    
    // 修复：检查任务是否被中止
    if(tgg_get_bwfx_sharecmd_halt(this->data->prc_id, this->data->fd)) {
        LOG_WARNING("ShareCmdSendToGroup: task halted, prc[%d] fd[%d]", this->data->prc_id, this->data->fd);
        return -1;
    }
    
    tgg_vhash_list* gids = this->data->gids;
    int raw = true;
    std::set<uint32_t> setExept;
    if(this->data->except_cid) {
        tgg_list_cid* exp_cids = this->data->except_cid;
        while(exp_cids->next) {
            setExept.insert(exp_cids->cid);
            exp_cids = exp_cids->next;
        }
    }
    std::vector<int64_t> lstAllFds;
    lstAllFds.reserve(RESERVED_SIZE_FOR_GID_CIDS);
    while(gids) {
        // 修复：处理每个gid前检查任务是否被中止
        if(tgg_get_bwfx_sharecmd_halt(this->data->prc_id, this->data->fd)) {
            LOG_WARNING("ShareCmdSendToGroup: task halted during processing, prc[%d] fd[%d]", this->data->prc_id, this->data->fd);
            lstAllFds.clear();
            lstAllFds.shrink_to_fit();
            return -1;
        }
        
        tgg_fd_list* lstFds = NULL;
        if(tgg_get_fdsbygid(gids->vhash, &lstFds) < 0) {
            LOG_ERROR("get fds by gid[%llu] failed.", gids->vhash);
            gids = gids->next;
            continue;
        }
        // 这里是否要考虑一个cid在多个群中，会发送多次的问题？workman的代码也存在同样的问题
        //    答：广播消息，不需要，客户端自己处理
        tgg_fd_list* tmp = lstFds;
        tgg_fd_list* node = tmp;
        while (tmp) {
            uint32_t cid = GET_CID_FDCID_MASK(node->fdidcid);
            if(setExept.size() == 0 || setExept.find(cid) == setExept.end()) {
                lstAllFds.push_back(node->fdidcid);
            }
            tmp = tmp->next;
            node = tmp;
        }
        iter_del_list<tgg_fd_list>(lstFds);
        if (!lstAllFds.empty()) {
            BatchSend2ClientByfds(lstAllFds, std::string_view(this->data->snddata, this->data->snddata_len), FD_WRITE, !raw);
        }
        LOG_INFO("SendToGroup: cmd executed for group[%llu] %d fds", gids->vhash, lstAllFds.size());
        lstAllFds.clear();
        gids = gids->next;
    }
    lstAllFds.shrink_to_fit();
    return 0;
}

int ShareCmdGetClientSessionsByGroup::ExecCmd()
{
    LOG_INFO("ShareCmdGetClientSessionsByGroup: prc[%d] cmd start.", this->prc_id);
    
    // 修复：检查任务是否被中止
    if(tgg_get_bwfx_sharecmd_halt(this->data->prc_id, this->data->fd)) {
        LOG_WARNING("ShareCmdGetClientSessionsByGroup: task halted, prc[%d] fd[%d]", this->data->prc_id, this->data->fd);
        return -1;
    }
    
    tgg_vhash_list* gid = this->data->gids;
    tgg_fd_list* lst_fd = NULL;
    if(gid) {// getcidbygid只有一个gid
        if (tgg_get_fdsbygid(gid->vhash, &lst_fd) < 0) {
            LOG_ERROR("ShareCmdGetClientSessionsByGroup: get fds by gid[%llu] failed.", gid->vhash);
            tgg_set_bwfx_sharecmd_halt(this->data->prc_id, this->data->fd, 1);
            return -1;
        }
    }
    
    // 修复：返回结果前再次检查任务是否被中止
    if(tgg_get_bwfx_sharecmd_halt(this->data->prc_id, this->data->fd)) {
        LOG_WARNING("ShareCmdGetClientSessionsByGroup: task halted after processing, prc[%d] fd[%d]", this->data->prc_id, this->data->fd);
        iter_del_list<tgg_fd_list>(lst_fd);
        return -1;
    }
    
    return tgg_add_bwfdx_gid_result(this->data->prc_id, this->data->fd, this->data->time, lst_fd);
}

int ShareCmdGetClientCountByGroup::ExecCmd()
{
    LOG_INFO("ShareCmdGetClientCountByGroup: prc[%d] cmd start.", this->prc_id);
    
    // 修复：检查任务是否被中止
    if(tgg_get_bwfx_sharecmd_halt(this->data->prc_id, this->data->fd)) {
        LOG_WARNING("ShareCmdGetClientCountByGroup: task halted, prc[%d] fd[%d]", this->data->prc_id, this->data->fd);
        return -1;
    }
    
    tgg_vhash_list* gid = this->data->gids;
    tgg_fd_list* lst_fd = (tgg_fd_list*)dpdk_rte_malloc(__FILE__, __LINE__, sizeof(tgg_fd_list));
    if(!lst_fd) {
        LOG_ERROR("GetClientCountByGroup: malloc for result failed.");
        tgg_set_bwfx_sharecmd_halt(this->data->prc_id, this->data->fd, 1);
        return -1;
    }
    lst_fd->next = NULL;
    if(gid) {// getcidbygid只有一个gid
        lst_fd->fdidcid = tgg_get_cidcount_bygid(gid->vhash);
        if (lst_fd->fdidcid < 0) {
            LOG_ERROR("GetClientCountByGroup: get fds by gid[%llu] failed, ret %d.", gid->vhash, lst_fd->fdidcid);
            tgg_set_bwfx_sharecmd_halt(this->data->prc_id, this->data->fd, 1);
            dpdk_rte_free(__FILE__, __LINE__, lst_fd);
            return -1;
        }
    }
    
    // 修复：返回结果前再次检查任务是否被中止
    if(tgg_get_bwfx_sharecmd_halt(this->data->prc_id, this->data->fd)) {
        LOG_WARNING("ShareCmdGetClientCountByGroup: task halted after processing, prc[%d] fd[%d]", this->data->prc_id, this->data->fd);
        dpdk_rte_free(__FILE__, __LINE__, lst_fd);
        return -1;
    }
    
    return tgg_add_bwfdx_gid_result(this->data->prc_id, this->data->fd, this->data->time, lst_fd);
}

 int ShareCmdPrintMemStats::ExecCmd()
{
    LOG_INFO("ShareCmdPrintMemStats: prc[%d] cmd start.", this->prc_id);
    tgg_clean_bw_share_qdata(this->prc_id, this->data);
    this->data = NULL;
    print_mem_statistics();
    // print_hash_statistics();
    return 0;
}

int exec_sharequeue_cmd_processor(int prc_id)
{
    bw_share_qdata* data = NULL;
    if(tgg_dequeue_bwshare(prc_id, &data) < 0) {
        return -1;
    }
    if(!data) {
        return -1;
    }
    ShareCmdBaseProcessor* pro = NULL;

    switch(data->cmd) {
        case CMD_SELECT:
            pro = new ShareCmdSelect(prc_id, data);
            break;
        case CMD_JOIN_GROUP:
            pro = new ShareCmdJoinGroup(prc_id, data);
            break;
        case CMD_LEAVE_GROUP:
            pro = new ShareCmdLeaveGroup(prc_id, data);
            break;
        case CMD_UNGROUP:
            pro = new ShareCmdUnGroup(prc_id, data);
            break;
        case CMD_SEND_TO_GROUP:
            pro = new ShareCmdSendToGroup(prc_id, data);
            break;
        case CMD_GET_CLIENT_SESSIONS_BY_GROUP:
            pro = new ShareCmdGetClientSessionsByGroup(prc_id, data);
            break;
        case CMD_GET_CLIENT_COUNT_BY_GROUP:
            pro = new ShareCmdGetClientCountByGroup(prc_id, data);
            break;
        case CMD_PRINT_MEM_STATS:
            pro = new ShareCmdPrintMemStats(prc_id, data);
            break;
        default :
            LOG_ERROR("Gateway inner pack err, Unknown cmd=%d.", data->cmd);
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

#include "TggCmdProcessor.h"
#include "tgg_comm/tgg_struct.h"
#include "comm/log.hpp"
#include "tgg_comm/tgg_bw_cache.h"
#include "GatewayProtocal.h"
#include "comm/common.hpp"
#include "tgg_comm/tgg_bwcomm.h"
#include "tgg_comm/tgg_conf.h"


extern int g_prc_id;



int CmdTggGateway::ReloadIpFilter()
{
    LOG_INFO("ExecCmd reload ip filter...");
    tgg_send_master_data* data = (tgg_send_master_data*)dpdk_rte_malloc(sizeof(tgg_send_master_data));
    if(!data) {
        LOG_ERROR("Enqueue master cmd failed, malloc data error.");
        return -1;
    }
    data->cmd = CMD_IP_FILTER_RELOAD;
    if(tgg_enqueue_master(data) < 0) {
        LOG_ERROR("Enqueue master cmd failed.");
        dpdk_rte_free(data);
        return -1;
    }
    LOG_INFO("reload ip filter success.");
    return 0;
}

int CmdTggGateway::UpdateRealWorkers()
{
    LOG_INFO("ExecCmd UpdateRealWorkers...");
    tgg_bwfdx_data* bwfdxdata = (tgg_bwfdx_data*)dpdk_rte_malloc(sizeof(tgg_bwfdx_data));
    if(!bwfdxdata) {
        LOG_ERROR("malloc bwfdxdata failed.");
        return -1;
    }
    bwfdxdata->bwfdx = -1;
    bwfdxdata->cmd = BWFDX_CMD_UPDATEALL;
    if(tgg_enqueue_bwfdx(bwfdxdata)) {
        LOG_ERROR("Enqueue bwfdxdata failed.");
        dpdk_rte_free(bwfdxdata);
    }
    LOG_INFO("UpdateRealWorkers success.");
    return 0;
}

int CmdTggGateway::PrintAllGids()
{
    LOG_INFO("ExecCmd PrintAllGids...");
    std::vector<std::string> lst_gid;
    if (tgg_get_allonlinegids(lst_gid) < 0) {
        return -1;
    }
    _print_path.append("_print_all_gids_");
    _print_path.append(std::to_string(g_prc_id));
    std::string header = "count: " + std::to_string(lst_gid.size());
    if (write_list_to_file(_print_path, header, lst_gid) < 0) {
        return -1;
    }

    LOG_INFO("PrintAllGids success.");
    return 0;
}

int CmdTggGateway::PrintGidCount()
{
    LOG_INFO("ExecCmd PrintGidCount...");
    std::vector<std::string> lst_gid;
    int count = tgg_get_gid_count();
    if (count < 0) {
        return -1;
    }
    _print_path.append("_print_gid_count_");
    _print_path.append(std::to_string(g_prc_id));
    std::string header = "count: " + std::to_string(count);
    if (write_list_to_file(_print_path, header, lst_gid) < 0) {
        return -1;
    }

    LOG_INFO("PrintGidCount success.");
    return 0;
}

int CmdTggGateway::PrintGidCids(rapidjson::Document& body)
{
    LOG_INFO("ExecCmd PrintGidCids...");
    std::string body_data = body["data"].GetString();
    if(body_data.size() > TGG_GID_LEN || body_data.size() <= 0) {
        LOG_ERROR("body_data[%s] not an GID", body_data.c_str());
        return -1;
    }
    std::vector<int64_t> lst_fd;
    std::vector<int> lstCids;
    if (tgg_get_fdsbygid(body_data.c_str(), lst_fd) < 0) {
        return -1;
    }
    if (lst_fd.size() > 0) {
        lstCids.reserve(lst_fd.size());
    }
    for (int64_t fdidcid : lst_fd) {
        if (fdidcid < 0) {
            LOG_WARNING("Invalid fdidcid[%lld] for gid[%s].", fdidcid, body_data.c_str());
            continue;
        }
        int cid = GET_CID_FDCID_MASK(fdidcid);
        if (cid <= 0) {
            LOG_WARNING("cid for fdidcid[%lld] gid[%s] not exist.", fdidcid, body_data.c_str());
            continue;
        }
        lstCids.push_back(cid);
    }

    _print_path.append("_print_gid_cids_");
    _print_path.append(std::to_string(g_prc_id));

    std::string header = "gid: " + body_data;
    header += "\ncount:";
    header += std::to_string(lstCids.size());

    if (write_list_to_file(_print_path, header, lstCids) < 0) {
        return -1;
    }

    LOG_INFO("PrintGidCids success.");
    return 0;
}

int CmdTggGateway::PrintAllUids()
{
    LOG_INFO("ExecCmd PrintAllUids...");
    std::vector<std::string> lst_uid;
    if (tgg_get_allonlinegids(lst_uid) < 0) {
        return -1;
    }
    _print_path.append("_print_all_uids_");
    _print_path.append(std::to_string(g_prc_id));
    std::string header = "count: " + std::to_string(lst_uid.size());
    if (write_list_to_file(_print_path, header, lst_uid) < 0) {
        return -1;
    }

    LOG_INFO("PrintAllUids success.");
    return 0;
}

int CmdTggGateway::PrintUidCount()
{
    LOG_INFO("ExecCmd PrintUidCount...");
    std::vector<std::string> lst_uid;
    int count = tgg_get_uid_count();
    if (count < 0) {
        return -1;
    }
    _print_path.append("_print_uid_count_");
    _print_path.append(std::to_string(g_prc_id));
    std::string header = "count: " + std::to_string(count);
    if (write_list_to_file(_print_path, header, lst_uid) < 0) {
        return -1;
    }

    LOG_INFO("PrintUidCount success.");
    return 0;
}

int CmdTggGateway::PrintUidCids(rapidjson::Document& body)
{
    LOG_INFO("ExecCmd PrintUidCids...");
    std::string body_data = body["data"].GetString();
    if(body_data.size() > TGG_UID_LEN || body_data.size() <= 0) {
        LOG_ERROR("body_data[%s] not an UID", body_data.c_str());
        return -1;
    }
    std::vector<int64_t> lst_fd;
    std::vector<int> lstCids;
    if (tgg_get_fdsbyuid(body_data.c_str(), lst_fd) < 0) {
        return -1;
    }
    if (lst_fd.size() > 0) {
        lstCids.reserve(lst_fd.size());
    }
    for (int64_t fdidcid : lst_fd) {
        if (fdidcid < 0) {
            LOG_WARNING("Invalid fdidcid[%lld] for uid[%s].", fdidcid, body_data.c_str());
            continue;
        }
        int cid = GET_CID_FDCID_MASK(fdidcid);
        if (cid <= 0) {
            LOG_WARNING("cid for fdidcid[%lld] uid[%s] not exist.", fdidcid, body_data.c_str());
            continue;
        }
        lstCids.push_back(cid);
    }

    _print_path.append("_print_uid_cids_");
    _print_path.append(std::to_string(g_prc_id));

    std::string header = "uid: " + body_data;
    header += "\ncount:";
    header += std::to_string(lstCids.size());

    if (write_list_to_file(_print_path, header, lstCids) < 0) {
        return -1;
    }

    LOG_INFO("PrintUidCids success.");
    return 0;
}


int CmdTggGateway::PrintAllCids()
{
    LOG_INFO("ExecCmd PrintAllCids...");
    std::vector<int64_t> lst_cid;
    if (tgg_get_allonlinecids(lst_cid) < 0) {
        return -1;
    }
    _print_path.append("_print_all_cids_");
    _print_path.append(std::to_string(g_prc_id));
    std::string header = "count: " + std::to_string(lst_cid.size());
    if (write_list_to_file(_print_path, header, lst_cid) < 0) {
        return -1;
    }

    LOG_INFO("PrintAllCids success.");
    return 0;
}

int CmdTggGateway::PrintCidCount()
{
    LOG_INFO("ExecCmd PrintCidCount...");
    std::vector<std::int64_t> lst_cid;
    int count = tgg_get_uid_count();
    if (count < 0) {
        return -1;
    }
    _print_path.append("_print_cid_count_");
    _print_path.append(std::to_string(g_prc_id));
    std::string header = "count: " + std::to_string(count);
    if (write_list_to_file(_print_path, header, lst_cid) < 0) {
        return -1;
    }

    LOG_INFO("PrintCidCount success.");
    return 0;
}

int CmdTggGateway::PrintAllIdxs()
{
    LOG_INFO("ExecCmd PrintAllIdxs...");
    int gwrcv_cnt = count_ones(TggConfigure::getInstance()->get_lcore_mask());
    _print_path.append("_print_all_idxs_core-");
    for (int i = 0; i < gwrcv_cnt; ++i)
    {
        std::string filename = _print_path + std::to_string(i);
        filename += "_";
        filename += std::to_string(g_prc_id);
        std::vector<int64_t> lst_idx;
        tgg_get_allidxs(i, lst_idx);
        std::string header = "count: " + std::to_string(lst_idx.size());
        if (write_list_to_file(_print_path, header, lst_idx) < 0) {
            continue;
        }
    }

    LOG_INFO("PrintAllIdxs success.");
    return 0;
}

int CmdTggGateway::PrintIdxCount()
{
    LOG_INFO("ExecCmd PrintIdxCount...");
    std::vector<int64_t> lst_idx;
    int gwrcv_cnt = count_ones(TggConfigure::getInstance()->get_lcore_mask());
    std::string header;
    for (int i = 0; i < gwrcv_cnt; ++i)
    {
        int count = tgg_count_idx(i);
        header += "core:";
        header += std::to_string(i);
        header +="\ncount: ";
        header += std::to_string(count);
        header += "\n";
    }
    _print_path.append("_print_idx_count_");
    _print_path.append(std::to_string(g_prc_id));
    if (write_list_to_file(_print_path, header, lst_idx) < 0) {
        return -1;
    }

    LOG_INFO("PrintIdxCount success.");
    return 0;
}

int CmdTggGateway::PrintAllWorkerKeys()
{
    LOG_INFO("ExecCmd PrintAllWorkerKeys...");
    std::vector<std::string> lst_wkkeys;
    if (tgg_get_allbwwkkeys(lst_wkkeys) < 0) {
        return -1;
    }
    _print_path.append("_print_all_workerkeys_");
    _print_path.append(std::to_string(g_prc_id));
    std::string header = "count: " + std::to_string(lst_wkkeys.size());
    if (write_list_to_file(_print_path, header, lst_wkkeys) < 0) {
        return -1;
    }

    LOG_INFO("PrintAllWorkerKeys success.");
    return 0;
}

int CmdTggGateway::PrintWorkerKeyCount()
{
    LOG_INFO("ExecCmd PrintWorkerKeyCount...");
    std::vector<std::string> lst_wkkeys;
    int count = tgg_get_bwwoker_count();
    if (count < 0) {
        return -1;
    }
    _print_path.append("_print_workerkey_count_");
    _print_path.append(std::to_string(g_prc_id));
    std::string header = "count: " + std::to_string(count);
    if (write_list_to_file(_print_path, header, lst_wkkeys) < 0) {
        return -1;
    }

    LOG_INFO("PrintWorkerKeyCount success.");
    return 0;
}

int CmdTggGateway::PrintAllWorkers()
{
    LOG_INFO("ExecCmd PrintAllWorkers...");
    // std::vector<int64_t> lst_wokers;
    std::vector<int64_t> vec_wokers;
    vec_wokers.reserve(5000);
    tgg_getall_bwfdx(vec_wokers);
    // auto it = vec_wokers.begin();
    // while(it != vec_wokers.end()) {
    //     lst_wokers.push_back(*it);
    //     it++;
    // }
    _print_path.append("_print_all_workers_");
    _print_path.append(std::to_string(g_prc_id));
    std::string header = "count: " + std::to_string(vec_wokers.size());
    if (write_list_to_file(_print_path, header, vec_wokers) < 0) {
        return -1;
    }

    LOG_INFO("PrintAllWorkers success.");
    return 0;
}

int CmdTggGateway::PrintWorkerCount()
{
    LOG_INFO("ExecCmd PrintWorkerCount...");
    std::vector<int64_t> lst_wokers;
    int count = tgg_get_bwfdx_count();
    if (count < 0) {
        return -1;
    }
    _print_path.append("_print_worker_count_");
    _print_path.append(std::to_string(g_prc_id));
    std::string header = "count: " + std::to_string(count);
    if (write_list_to_file(_print_path, header, lst_wokers) < 0) {
        return -1;
    }

    LOG_INFO("PrintWorkerCount success.");
    return 0;
}

int CmdTggGateway::PrintRealAllWorkers()
{
    LOG_INFO("ExecCmd PrintRealAllWorkers...");
    tgg_bwfdx_data* bwfdxdata = (tgg_bwfdx_data*)dpdk_rte_malloc(sizeof(tgg_bwfdx_data));
    if(!bwfdxdata) {
        LOG_ERROR("malloc bwfdxdata failed.");
        return -1;
    }
    bwfdxdata->bwfdx = -1;
    bwfdxdata->cmd = BWFDX_CMD_PRINTWORKERS;
    if(tgg_enqueue_bwfdx(bwfdxdata)) {
        LOG_ERROR("Enqueue bwfdxdata failed.");
        dpdk_rte_free(bwfdxdata);
    }
    LOG_INFO("PrintRealAllWorkers success.");
    return 0;
}

int CmdTggGateway::PrintRealWorkerCount()
{
    LOG_INFO("ExecCmd PrintRealWorkerCount...");
    tgg_bwfdx_data* bwfdxdata = (tgg_bwfdx_data*)dpdk_rte_malloc(sizeof(tgg_bwfdx_data));
    if(!bwfdxdata) {
        LOG_ERROR("malloc bwfdxdata failed.");
        return -1;
    }
    bwfdxdata->bwfdx = -1;
    bwfdxdata->cmd = BWFDX_CMD_PRINTWORKERCOUNT;
    if(tgg_enqueue_bwfdx(bwfdxdata)) {
        LOG_ERROR("Enqueue bwfdxdata failed.");
        dpdk_rte_free(bwfdxdata);
    }
    LOG_INFO("PrintRealWorkerCount success.");
    return 0;
}

int CmdTggGateway::CheckGidcidAvaliable()
{
    LOG_INFO("ExecCmd CheckGidcidAvaliable...");
    std::vector<std::string> lst_gid;
    std::vector<std::string> lst_result;
    // 获取所有在线的分组
    if (tgg_get_allonlinegids(lst_gid) < 0)
        return -1;
    if(lst_gid.size() > 0)
        lst_result.reserve(lst_gid.size());
    auto it = lst_gid.begin();
    while (it != lst_gid.end()) {
        std::vector<int64_t> lst_fd;
        if (tgg_get_fdsbygid((*it).c_str(), lst_fd) < 0)
            continue;
        auto it_fdidcid = lst_fd.begin();
        while(it_fdidcid != lst_fd.end()) {
            std::string str_result;
            int coreid = GET_COREID_FDCID_MASK(*it_fdidcid);
            int fd = GET_FD_FDCID_MASK(*it_fdidcid);
            int idx = GET_IDX_FDCID_MASK(*it_fdidcid);
            int cid = GET_CID_FDCID_MASK(*it_fdidcid);
            if(tgg_check_idx_exist(coreid, idx) < 0) {
                str_result = *it + ":";
                str_result += std::to_string(cid) + ":";
                str_result += std::to_string(*it_fdidcid);
                str_result += "_idx";
            }
            if (tgg_get_cli_idx(coreid, fd) < 0) {
                if(str_result.empty()) {
                    str_result = *it + ":";
                    str_result += std::to_string(cid) + ":";
                    str_result += std::to_string(*it_fdidcid);
                    str_result += "_fd";
                } else {
                    str_result += "_fd";
                }
            }
            if(!str_result.empty()) {
                // TODO 同时出现大量失效连接时会有内存涨爆的风险
                lst_result.push_back(str_result);
            }
            it_fdidcid++;
        }
        it++;
    }
    _print_path.append("_check_gidcid_avaliable_");
    _print_path.append(std::to_string(g_prc_id));
    std::string header = "count: " + std::to_string(lst_result.size());
    if (write_list_to_file(_print_path, header, lst_result) < 0) {
        return -1;
    }

    LOG_INFO("CheckGidcidAvaliable success.");
    return 0;
}

int CmdTggGateway::CheckUidcidAvaliable()
{
    LOG_INFO("ExecCmd CheckUidcidAvaliable...");
    std::vector<std::string> lst_uid;
    std::vector<std::string> lst_result;
    // 获取所有在线的分组
    if (tgg_get_allonlineuids(lst_uid) < 0)
        return -1;
    if(lst_uid.size() > 0)
        lst_result.reserve(lst_uid.size());
    auto it = lst_uid.begin();
    while (it != lst_uid.end()) {
        std::vector<int64_t> lst_fd;
        if (tgg_get_fdsbygid((*it).c_str(), lst_fd) < 0)
            continue;
        auto it_fdidcid = lst_fd.begin();
        while(it_fdidcid != lst_fd.end()) {
            std::string str_result;
            int coreid = GET_COREID_FDCID_MASK(*it_fdidcid);
            int fd = GET_FD_FDCID_MASK(*it_fdidcid);
            int idx = GET_IDX_FDCID_MASK(*it_fdidcid);
            int cid = GET_CID_FDCID_MASK(*it_fdidcid);
            if(tgg_check_idx_exist(coreid, idx) < 0) {
                str_result = *it + ":";
                str_result += std::to_string(cid) + ":";
                str_result += std::to_string(*it_fdidcid);
                str_result += "_idx";
            }
            if (tgg_get_cli_idx(coreid, fd) < 0) {
                if(str_result.empty()) {
                    str_result = *it + ":";
                    str_result += std::to_string(cid) + ":";
                    str_result += std::to_string(*it_fdidcid);
                    str_result += "_fd";
                } else {
                    str_result += "_fd";
                }
            }
            if(!str_result.empty()) {
                // TODO 同时出现大量失效连接时会有内存涨爆的风险
                lst_result.push_back(str_result);
            }
            it_fdidcid++;
        }
        it++;
    }
    _print_path.append("_check_uidcid_avaliable_");
    _print_path.append(std::to_string(g_prc_id));
    std::string header = "count: " + std::to_string(lst_result.size());
    if (write_list_to_file(_print_path, header, lst_result) < 0) {
        return -1;
    }

    LOG_INFO("CheckUidcidAvaliable success.");
    return 0;
}

int CmdTggGateway::CheckCidAvaliable()
{
    LOG_INFO("ExecCmd CheckCidAvaliable...");
    std::vector<std::string> lst_result;
    std::vector<int64_t> lst_fd;
    if (tgg_get_allfds(lst_fd) < 0)
        return -1;
    if(lst_fd.size() > 0)
        lst_result.reserve(lst_fd.size());
    auto it_fdidcid = lst_fd.begin();
    while(it_fdidcid != lst_fd.end()) {
        std::string str_result;
        int coreid = GET_COREID_FDCID_MASK(*it_fdidcid);
        int fd = GET_FD_FDCID_MASK(*it_fdidcid);
        int idx = GET_IDX_FDCID_MASK(*it_fdidcid);
        int cid = GET_CID_FDCID_MASK(*it_fdidcid);
        if(tgg_check_idx_exist(coreid, idx) < 0) {
            str_result += std::to_string(cid) + ":";
            str_result += std::to_string(*it_fdidcid);
            str_result += "_idx";
        }
        if (tgg_get_cli_idx(coreid, fd) < 0) {
            if(str_result.empty()) {
                str_result += std::to_string(cid) + ":";
                str_result += std::to_string(*it_fdidcid);
                str_result += "_fd";
            } else {
                str_result += "_fd";
            }
        }
        if(!str_result.empty()) {
            // TODO 同时出现大量失效连接时会有内存涨爆的风险
            lst_result.push_back(str_result);
        }
         it_fdidcid++;
    }
    _print_path.append("_check_cid_avaliable_");
    _print_path.append(std::to_string(g_prc_id));
    std::string header = "count: " + std::to_string(lst_result.size());
    if (write_list_to_file(_print_path, header, lst_result) < 0) {
        return -1;
    }

    LOG_INFO("CheckCidAvaliable success.");
    return 0;
}


int CmdTggGateway::ExecCmd()
{
    _print_path.reserve(256);
    _print_path.append(TggConfigure::getInstance()->get_health_check_path());
    _print_path.append("/");
    ensure_path_exists(_print_path);
    std::string_view body = get_body_string(jdata);
    rapidjson::Document body_info;
    body_info.Parse(body.data());
    if (body_info.HasParseError()) {
        LOG_ERROR("CmdTggGateway: JSON parse error");
        return -1;
    }
    if (!body_info.HasMember("cmd")) {
        LOG_ERROR("CmdTggGateway: no cmd key found in body.");
        return -1;
    }
    int cmd = body_info["cmd"].GetInt();
    switch (cmd) {
        case CMD_RELOAD_IP_FILTER:
            return ReloadIpFilter();
        case CMD_UPDATE_REAL_WORKERS:
            return UpdateRealWorkers();
            break;
        case CMD_PRINT_ALL_GIDS:
            return PrintAllGids();
            break;
        case CMD_PRINT_GID_COUNT:
            return PrintGidCount();
            break;
        case CMD_PRINT_GID_CIDS:
            return PrintGidCids(body_info);
            break;
        case CMD_PRINT_ALL_UIDS:
            return PrintAllUids();
            break;
        case CMD_PRINT_UID_COUNT:
            return PrintUidCount();
            break;
        case CMD_PRINT_UID_CIDS:
            return PrintUidCids(body_info);
            break;
        case CMD_PRINT_ALL_CIDS:
            return PrintAllCids();
            break;
        case CMD_PRINT_CID_COUNT:
            return PrintCidCount();
            break;
        case CMD_PRINT_ALL_IDXS:
            return PrintAllIdxs();
            break;
        case CMD_PRINT_IDX_COUNT:
            return PrintIdxCount();
            break;
        case CMD_PRINT_ALL_WORKERKEYS:
            return PrintAllWorkerKeys();
            break;
        case CMD_PRINT_WORKERKEY_COUNT:
            return PrintWorkerKeyCount();
            break;
        case CMD_PRINT_ALL_WORKERS:
            return PrintAllWorkers();
            break;
        case CMD_PRINT_WORKER_COUNT:
            return PrintWorkerCount();
            break;
        case CMD_PRINT_REAL_ALL_WORKERS:
            return PrintRealAllWorkers();
            break;
        case CMD_PRINT_REAL_WORKER_COUNT:
            return PrintRealWorkerCount();
            break;
        case CMD_CHECK_GIDCID_AVALIABLE:
            return CheckGidcidAvaliable();
            break;
        case CMD_CHECK_UIDCID_AVALIABLE:
            return CheckUidcidAvaliable();
            break;
        case CMD_CHECK_CID_AVALIABLE:
            return CheckCidAvaliable();
            break;
        default:
            LOG_WARNING("unknown cmd:%d", cmd);
            return -1;
            break;
    }
    return 0;
}
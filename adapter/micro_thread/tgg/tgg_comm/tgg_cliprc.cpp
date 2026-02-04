#include "tgg_cliprc.h"
#include "comm/log.hpp"
#include <rte_malloc.h>
#include <rte_mempool.h>
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/WsConsumer.h"
#include "tgg_comm/tgg_bw_cache.h"
#include "comm/common.hpp"
#include "tgg_transport.h"
#include "tgg_conf.h"
#include "tgg_bwcomm.h"
#include <vector>
#include <chrono>
extern int g_run;
// static  pthread_t s_bwtrans_thread;

#include <unistd.h>

extern struct rte_mempool* g_mempool_bwrcv;
#define MAX_CALC_LOAD_BALANCE_TRY 3


// 优化1：使用hash_set替代vector，add/delete从O(N)/O(N)优化到O(1)平均
#include <unordered_set>
typedef std::unordered_set<int64_t> BwfdxSet;

static void add_bwfdx(BwfdxSet& set_bwfdx, int64_t bwfdx)
{
    if(bwfdx <= 0) {
        LOG_ERROR("add bwfdx to cliprc failed, invalid bwfdx[%ld].", bwfdx);
        return;
    }
    if (set_bwfdx.insert(bwfdx).second) {
        LOG_INFO("added bwfdx[%ld] to cliprc.", bwfdx);
    } else {
        LOG_DEBUG("bwfdx[%ld] already exists.", bwfdx);
    }
}

static void delete_bwfdx(BwfdxSet& set_bwfdx, int64_t bwfdx)
{
    if(bwfdx <= 0) {
        LOG_ERROR("delete bwfdx for cliprc failed, invalid bwfdx[%ld].", bwfdx);
        return;
    }
    if (set_bwfdx.erase(bwfdx) > 0) {
        LOG_INFO("delete bwfdx[%ld] for cliprc.", bwfdx);
    } else {
        LOG_DEBUG("bwfdx[%ld] not found.", bwfdx);
    }
}

static int s_enqueued_to_server_count = 0;

// ===== deal_trans() 辅助函数 - 职责分离和消除重复逻辑 =====

/**
 * 处理已绑定的连接：直接发送到BW
 * return: 0=成功, -1=失败(需要重入队列)
 */
static inline int handle_already_bound(tgg_trans_data* tdata, int bwfdx)
{
    if (!(bwfdx > 0 && tgg_get_bwfdx_status(GET_COREID_FDID_MASK(bwfdx), GET_FD_FDID_MASK(bwfdx)))) {
        return -1;  // 绑定已失效
    }
    
    int prc_id = GET_COREID_FDID_MASK(bwfdx);
    tgg_bw_data* bdata = get_bwdata_from_transdata(prc_id, tdata);
    if (!bdata) {
        LOG_ERROR("get_bwdata_from_transdata failed for prc_id[%d]", prc_id);
        return -1;
    }
    
    bdata->bwfdx = bwfdx;
    if (tgg_enqueue_bwsnd(prc_id, bdata) < 0) {
        LOG_DEBUG("enqueue to bwsnd failed. bwfdx:%d", bwfdx);
        clean_bw_data(prc_id, bdata);
        return -1;
    }
    
    s_enqueued_to_server_count++;
    return 0;
}

/**
 * 重新绑定连接到负载最少的BW
 * return: bwfdx (>0=成功), <=0=失败
 */
static inline int try_rebind_connection(tgg_trans_data* tdata, BwfdxSet& set_bwfdx)
{
    for (int retry = 0; retry < MAX_CALC_LOAD_BALANCE_TRY; ++retry) {
        // 通过负载均衡选择BW
        std::vector<int64_t> vec_bwfdx(set_bwfdx.begin(), set_bwfdx.end());
        int bwfdx = tgg_get_load_balance(vec_bwfdx, tdata->peer_ip + tdata->peer_port);
        
        if (bwfdx <= 0) {
            LOG_DEBUG("get_load_balance attempt %d failed", retry + 1);
            usleep(10);
            continue;
        }
        
        if (tgg_get_bwfdx_status(GET_COREID_FDID_MASK(bwfdx), GET_FD_FDID_MASK(bwfdx))) {
            return bwfdx;  // 找到有效的BW
        }
    }
    
    return -1;  // 失败
}

/**
 * 绑定客户端到新的BW并发送数据
 * return: 0=成功, -1=失败(需要重入队列)
 */
static inline int handle_rebind_and_send(tgg_trans_data* tdata, BwfdxSet& set_bwfdx)
{
    int bwfdx = try_rebind_connection(tdata, set_bwfdx);
    if (bwfdx <= 0) {
        LOG_DEBUG("failed to rebind connection fd[%d] idx[%d], no available bwfdx", 
                 tdata->fd, tdata->idx);
        return -1;
    }
    
    // 绑定客户端到该BW
    tgg_set_cli_bwfdx(tdata->coreid, tdata->fd, bwfdx);
    
    int prc_id = GET_COREID_FDID_MASK(bwfdx);
    tgg_bw_data* bdata = get_bwdata_from_transdata(prc_id, tdata);
    if (!bdata) {
        LOG_ERROR("get_bwdata_from_transdata failed for prc_id[%d]", prc_id);
        return -1;
    }
    
    bdata->bwfdx = bwfdx;
    if (tgg_enqueue_bwsnd(prc_id, bdata) < 0) {
        LOG_DEBUG("enqueue to bwsnd failed after rebind. bwfdx:%d", bwfdx);
        clean_bw_data(prc_id, bdata);
        return -1;
    }
    
    s_enqueued_to_server_count++;
    return 0;
}

void clean_all_bussiness_hash()
{
    tgg_clean_cidgid();
    tgg_clean_gid();
    tgg_clean_uid();
    tgg_clean_cid();
}


static uint64_t s_last_update_time = 0;
// 定时器回调函数
void update_gwcliprc_heart_beat() {
    uint64_t now = get_system_ms();
    if(now - s_last_update_time > GW_MONITOR_HEART_BEAT_UPDATE) {
        // printf("update heart beat for [PID:%d][prc_id:%d]\n", getpid(), g_prc_id);
        s_last_update_time = now;
        tgg_update_gw_monitor(count_ones(TggConfigure::getInstance()->get_lcore_mask()), now);
    }
}

void print_current_workers(BwfdxSet& set_bwfdx)
{
    LOG_INFO("ExecCmd PrintALLRealWorkers...");
    std::string print_path;
    print_path.reserve(256);
    print_path.append(TggConfigure::getInstance()->get_health_check_path());
    print_path.append("/");
    print_path.append("_print_real_all_workers");

    // 转换为vector用于文件写入
    std::vector<int64_t> vec_bwfdx(set_bwfdx.begin(), set_bwfdx.end());
    std::string header = "count: " + std::to_string(vec_bwfdx.size());
    if (write_list_to_file(print_path, header, vec_bwfdx) < 0) {
        return;
    }

    LOG_INFO("PrintALLRealWorkers success.");

}

void print_current_worker_count(BwfdxSet& set_bwfdx)
{
    LOG_INFO("ExecCmd PrintRealWorkerCount, count:%zu.", set_bwfdx.size());
    std::string print_path;
    print_path.reserve(256);
    print_path.append(TggConfigure::getInstance()->get_health_check_path());
    print_path.append("/");
    print_path.append("_print_real_worker_count");

    std::vector<std::string> lst_workers;
    lst_workers.push_back("count: " + std::to_string(set_bwfdx.size()));
    if (write_list_to_file(print_path, "", lst_workers) < 0) {
        return;
    }

    LOG_INFO("PrintRealWorkerCount success.");
}

static void* deal_trans(void*)
{
    BwfdxSet set_bwfdx;
    set_bwfdx.reserve(5000);  // 预分配容量
    
    // 一开始就获取所有在线的bwfdx，防止因重启而丢失数据
    std::vector<int64_t> vec_bwfdx;
    tgg_getall_bwfdx(vec_bwfdx);
    for (int64_t bwfdx : vec_bwfdx) {
        set_bwfdx.insert(bwfdx);
    }
    
    while(g_run) {
        // 1. 处理心跳更新
        update_gwcliprc_heart_beat();
        
        // 2. 处理BW管理命令（add/delete/update）
        tgg_bwfdx_data* bwfdxdata = NULL;
        if(!tgg_dequeue_bwfdx(&bwfdxdata)) {
            switch(bwfdxdata->cmd) {
                case BWFDX_CMD_ADD:
                    add_bwfdx(set_bwfdx, bwfdxdata->bwfdx);
                    break;
                case BWFDX_CMD_DELETE:
                    delete_bwfdx(set_bwfdx, bwfdxdata->bwfdx);
                    break;
                case BWFDX_CMD_UPDATEALL:
                    LOG_INFO("Update Runtime bwworkers");
                    set_bwfdx.clear();
                    vec_bwfdx.clear();
                    tgg_getall_bwfdx(vec_bwfdx);
                    for (int64_t bwfdx : vec_bwfdx) {
                        set_bwfdx.insert(bwfdx);
                    }
                    break;
                case BWFDX_CMD_CLEAN_BWHASH:
                    LOG_WARNING("clean all gid,uid and cid hash tables");
                    clean_all_bussiness_hash();
                    break;
                case BWFDX_CMD_PRINTWORKERS:
                    print_current_workers(set_bwfdx);
                    break;
                case BWFDX_CMD_PRINTWORKERCOUNT:
                    print_current_worker_count(set_bwfdx);
                    break;
                default:
                    LOG_DEBUG("invalid cmd[%d].", bwfdxdata->cmd);
                    break;
            }
            dpdk_rte_free(__FILE__, __LINE__, bwfdxdata);
        }
        
        // 3. 处理客户端数据透传
        tgg_trans_data* tdata = NULL;
        if (tgg_dequeue_trans(&tdata) < 0) {
            usleep(2);  // 队列为空时短睡眠
            continue;
        }
        
        // 4. 检查是否存在可用的BW
        if (set_bwfdx.empty()) {
            // 没有bwfdx时，关闭所有连接，丢弃数据（防止满队列死锁）
            LOG_DEBUG("no bwfdx found, drop data fd[%d].", tdata->fd);
            Send2Fd(tdata->coreid, tdata->fd, tdata->idx, "", FD_WRITE|FD_CLOSE, 0);
            clean_trans_data(tdata);
            continue;
        }
        
        if (tdata->fd <= 0) {
            LOG_ERROR("invalid fd:%d, drop data.", tdata->fd);
            clean_trans_data(tdata);
            continue;
        }
        
        // 5. 尝试处理数据
        int bwfdx = tgg_get_cli_bwfdx(tdata->coreid, tdata->fd);
        
        if (bwfdx > 0 && tgg_get_bwfdx_status(GET_COREID_FDID_MASK(bwfdx), GET_FD_FDID_MASK(bwfdx))) {
            // 已绑定有效BW，直接发送
            if (handle_already_bound(tdata, bwfdx) < 0) {
                // 入队失败，重入trans队列重试
                if (tgg_enqueue_trans(tdata) < 0) {
                    LOG_FATAL("enque back trans queue failed, coreid[%d] fd[%d]", 
                             tdata->coreid, tdata->fd);
                    clean_trans_data(tdata);
                }
                continue;
            }
        } else {
            // 未绑定或绑定失效，需要重新绑定
            if (handle_rebind_and_send(tdata, set_bwfdx) < 0) {
                // 绑定失败，重入trans队列重试
                if (tgg_enqueue_trans(tdata) < 0) {
                    LOG_FATAL("enque back trans queue failed after rebind, coreid[%d] fd[%d]", 
                             tdata->coreid, tdata->fd);
                    clean_trans_data(tdata);
                }
                continue;
            }
        }
        
        clean_trans_data(tdata);
    }
    return 0;
}

void init_bwtrans()
{
    LOG_INFO("Trans thread started.");
    deal_trans(NULL);
    // pthread_create(&s_bwtrans_thread, NULL, &deal_trans, NULL);
}

void uninit_bwtrans()
{
    // void* retval = NULL;
    // if (pthread_join(s_bwtrans_thread, &retval) < 0) {
    //     LOG_ERROR("join thread failed.");
    // }
    LOG_WARNING("Trans thread ended, enqueue count:%d.", s_enqueued_to_server_count);
}
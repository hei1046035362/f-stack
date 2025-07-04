#include "tgg_cliprc.h"
#include "comm/log.hpp"
#include <rte_malloc.h>
#include <rte_mempool.h>
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/WsConsumer.h"
#include "tgg_comm/tgg_bw_cache.h"
#include "comm/common.hpp"
#include "tgg_transport.h"
#include <vector>
#include <chrono>
extern int g_run;
// static  pthread_t s_bwtrans_thread;

#include <unistd.h>

extern struct rte_mempool* g_mempool_bwrcv;
#define MAX_CALC_LOAD_BALANCE_TRY 3


static void add_bwfdx(std::vector<int64_t>& vec_bwfdx, int64_t bwfdx)
{
    if(bwfdx <= 0) {
        LOG_ERROR("add bwfdx to cliprc failed, invalid bwfdx[%ld].", bwfdx);
        return;
    }
    vec_bwfdx.push_back(bwfdx);
    LOG_INFO("added bwfdx[%ld] to cliprc.", bwfdx);
}

#include <functional>
static void delete_bwfdx(std::vector<int64_t>& vec_bwfdx, int64_t bwfdx)
{
    if(bwfdx <= 0) {
        LOG_ERROR("delete bwfdx for cliprc failed, invalid bwfdx[%ld].", bwfdx);
        return;
    }
    auto it = std::find(vec_bwfdx.begin(), vec_bwfdx.end(), std::cref(bwfdx));
    if (it != vec_bwfdx.end()) {
        std::swap(*it, vec_bwfdx.back()); // 交换目标与末尾元素
        vec_bwfdx.pop_back();             // 删除末尾
    }
    LOG_INFO("delete bwfdx[%ld] for cliprc.", bwfdx);
}

static int s_enqueued_to_server_count = 0;

void clean_all_bussiness_hash()
{
    tgg_clean_cidgid();
    tgg_clean_gid();
    tgg_clean_uid();
    tgg_clean_cid();
}

static bool try_clean_trans_data(tgg_trans_data* tdata)
{
    if (tdata->fd_opt & FD_CLOSE) {// 只要连接尚未关闭，就重入队列
        LOG_DEBUG("try enque back trans queue, coreid[%d] fd[%d] idx[%d].", tdata->coreid, tdata->fd, tdata->idx);
        if(tgg_enqueue_trans(tdata) < 0) {// 这里不需要重试，重试也不能解决问题，这里是trans队列唯一消费的地方
            LOG_FATAL("enque back trans queue failed, coreid[%d] fd[%d] idx[%d].", tdata->coreid, tdata->fd, tdata->idx);
            clean_trans_data(tdata);// 失败的话，很可能回导致内存泄漏，需要观察
            return false;
        }
    } else {
        clean_trans_data(tdata);// 失败的话，很可能回导致内存泄漏，需要观察
        LOG_WARNING("droped data, coreid[%d] fd[%d] idx[%d].", tdata->coreid, tdata->fd, tdata->idx);
    }
    return true;
}

static void* deal_trans(void*)
{
    std::vector<int64_t> vec_bwfdx;
    vec_bwfdx.reserve(5000);// 防止频繁分配赋值内存，预先分配5000个
    tgg_getall_bwfdx(vec_bwfdx);// 一开始就获取所有的在线的bwfdx，防止因重启而丢失数据
    bool clean_hash_flag = false;// 是否要清理所有的业务hash表
    while(g_run) {
        // 取可用的bw
        tgg_bwfdx_data* bwfdxdata = NULL;
        if(!tgg_dequeue_bwfdx(&bwfdxdata)) {
            switch(bwfdxdata->cmd) {
                case BWFDX_CMD_ADD:
                    add_bwfdx(vec_bwfdx, bwfdxdata->bwfdx);
                    break;
                case BWFDX_CMD_DELETE:
                    delete_bwfdx(vec_bwfdx, bwfdxdata->bwfdx);
                    break;
                case BWFDX_CMD_UPDATEALL:
                    tgg_getall_bwfdx(vec_bwfdx);
                    break;
                default:
                    LOG_INFO("invalid cmd[%d].", bwfdxdata->cmd);
                    break;
            }
            dpdk_rte_free(bwfdxdata);
        }
        // 当没有bw连接时，清理所有的业务型(cid, uid, gid, cidgid)hash表
        if(vec_bwfdx.size() <= 0) {// 防止gwbwprc内存泄漏,没有bwfdx时，证明客户端连接都失效的，可以放心清理，然后让其重连
            if(clean_hash_flag) {
                clean_all_bussiness_hash();
                clean_hash_flag = false;
            }
        } else if(!clean_hash_flag) {
            clean_hash_flag = true;
        }

        // 取数据
        tgg_trans_data* tdata = NULL;
        if (tgg_dequeue_trans(&tdata) < 0) {
            usleep(10);
            continue;
        }
        if(vec_bwfdx.size() <= 0) {
            // 防止满队列，死锁 没有bwfdx时，所有连接全部关闭，所有数据全部丢弃
            LOG_DEBUG("no bwfdx found, drop data.");
            Send2Fd(tdata->coreid, tdata->fd, tdata->idx, "", FD_WRITE|FD_CLOSE, 0);
            clean_trans_data(tdata);// 前面已经清空gwbwrcv侧的hash表了，这里不需要重入队列
            continue;
        }

        if(tdata->fd <= 0) {
            LOG_ERROR("trans error,invalid fd:%d", tdata->fd);
        }
        int bwfdx = tgg_get_cli_bwfdx(tdata->coreid, tdata->fd);
        if(bwfdx > 0 && tgg_get_bwfdx_status(GET_COREID_FDID_MASK(bwfdx), GET_FD_FDID_MASK(bwfdx))) {
            // 已经绑定服务端，正常透传
            int prc_id = GET_COREID_FDID_MASK(bwfdx);
            tgg_bw_data* bdata = get_bwdata_from_transdata(prc_id, tdata);
            bdata->bwfdx = bwfdx;
            if(!bdata) {
                if (try_clean_trans_data(tdata)) {// 这里不需要重试，重试也不能解决问题，这里是trans队列唯一消费的地方
                    LOG_ERROR("try_clean_trans_data succeed.");
                    continue;
                }
            }
            if(tgg_enqueue_bwsnd( prc_id, bdata) < 0) {
                // 重入客户端上行透传队列
                if (try_clean_trans_data(tdata)) {// 这里不需要重试，重试也不能解决问题，这里是trans队列唯一消费的地方
                    LOG_ERROR("try_clean_trans_data succeed.");
                    clean_bw_data(prc_id, bdata);
                    continue;
                }
                // 关闭客户端连接，清理
                LOG_ERROR("enque bwsnd failed, bwfdx:%d.", bwfdx);
                if(bdata->fd_opt&FD_CLOSE) {
                    Send2Fd(bdata->coreid, bdata->fd, bdata->idx, "", FD_WRITE|FD_CLOSE, 0);
                }
                clean_bw_data(prc_id, bdata);
            }
            s_enqueued_to_server_count++;
        } else {
            // 重新绑定或首次绑定，先绑定再透传
            int index = MAX_CALC_LOAD_BALANCE_TRY;
            while (--index) {
                // 随机取一个可用的服务端连接
                // int pos = bdata->fd % tgg_get_bwfdx_count();
                // bwfdx = tgg_get_bwfdx_bypos(pos);
                bwfdx = tgg_get_load_balance(vec_bwfdx);
                if(bwfdx == -1) {
                    LOG_ERROR("get load balance failed.");
                    usleep(10);
                    continue;
                }
                if(bwfdx > 0 && tgg_get_bwfdx_status(GET_COREID_FDID_MASK(bwfdx), GET_FD_FDID_MASK(bwfdx))) {
                    break;
                }
            }
            if(index < MAX_CALC_LOAD_BALANCE_TRY - 1) {
                LOG_ERROR("get bwfdx for fd[%d] idx[%d] failed, tried times:%d.", 
                    tdata->fd, tdata->idx, MAX_CALC_LOAD_BALANCE_TRY-index);
            }
            if (bwfdx <= 0) {// 入队列失败之后，清理数据，否则上行队列会满，而无法接收新数据
                // 重入客户端上行透传队列
                if (try_clean_trans_data(tdata)) {// 这里不需要重试，重试也不能解决问题，这里是trans队列唯一消费的地方
                    LOG_ERROR("try_clean_trans_data succeed.");
                    // 重入透传队列以后，这些操作都会重新执行，本次操作就直接跳过了
                    continue;
                }
                LOG_ERROR("get bwfdx failed.");
            } else {
                // 客户端连接绑定到服务端连接
                tgg_set_cli_bwfdx(tdata->coreid, tdata->fd, bwfdx);
                // 负载++
                // tgg_add_bwfdx_load(bwfdx);
                int prc_id = GET_COREID_FDID_MASK(bwfdx);
                tgg_bw_data* bdata = get_bwdata_from_transdata(prc_id, tdata);
                bdata->bwfdx = bwfdx;
                if (tgg_enqueue_bwsnd( (bwfdx & 0xff), bdata) < 0) {
                    // TODO 判断进程是否还在，不在了的话要做些什么操作
                    int prc_id = GET_COREID_FDID_MASK(bwfdx);
                    tgg_bw_data* bdata = get_bwdata_from_transdata(prc_id, tdata);
                    if(!bdata) {
                        if (!try_clean_trans_data(tdata)) {// 这里不需要重试，重试也不能解决问题，这里是trans队列唯一消费的地方
                            LOG_ERROR("try_clean_trans_data failed.");
                        }
                        continue;
                    }
                    // 重入客户端上行透传队列
                    if (try_clean_trans_data(tdata)) {// 这里不需要重试，重试也不能解决问题，这里是trans队列唯一消费的地方
                        LOG_ERROR("try_clean_trans_data succeed.");
                        clean_bw_data(prc_id, bdata);
                        // 重入透传队列以后，这些操作都会重新执行，本次操作就直接跳过了
                        continue;
                    }
                    LOG_ERROR("enque bwsnd failed, bwfdx:%d.", bwfdx);
                    if(bdata->fd_opt&FD_CLOSE) {
                        Send2Fd(bdata->coreid, bdata->fd, bdata->idx, "", FD_WRITE|FD_CLOSE, 0);
                    }
                    clean_bw_data(prc_id, bdata);
                }
                s_enqueued_to_server_count++;
            }
        }
        clean_trans_data(tdata);// 失败的话，很可能回导致内存泄漏，需要观察
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
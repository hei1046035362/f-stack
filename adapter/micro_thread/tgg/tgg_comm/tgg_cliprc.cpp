#include "tgg_cliprc.h"
#include "comm/log.hpp"
#include <rte_malloc.h>
#include <rte_mempool.h>
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/WsConsumer.h"
#include "tgg_comm/tgg_bw_cache.h"
#include "comm/common.hpp"
#include <chrono>
extern struct rte_mempool* g_mempool_read;
extern int g_run;


void tgg_process_read(int lcore_idx)
{
    while (g_run) {
        tgg_read_data* rdata = NULL;
        if (tgg_dequeue_cliprc(lcore_idx, &rdata) < 0) {
            // 队列空
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            //usleep(10);
            continue;
        }
        if (!rdata) {
            continue;
        }

        WsConsumer cons;
        cons.ConsumerData(rdata);

        memset(rdata->data, 0, rdata->data_len);
        rte_free(rdata->data);
        memset(rdata, 0, sizeof(tgg_read_data));
        rte_mempool_put(g_mempool_read, (void*)rdata);
    }
    LOG_INFO("cliprc thread exit, handle lcore_idx:%d", lcore_idx);
}


// extern int g_run;
static  pthread_t s_bwtrans_thread;
#include <unistd.h>

extern struct rte_mempool* g_mempool_bwrcv;
#define MAX_CALC_LOAD_BALANCE_TRY 3
static void* deal_trans(void*)
{
    while(g_run) {
        tgg_bw_data* bdata = NULL;
        if (tgg_dequeue_trans(&bdata) < 0) {
            usleep(10);
            continue;
        }
#if 0
        int idx = tgg_get_cli_idx(bdata->coreid, bdata->fd);
        if(bdata->data_len > 3 && !strncmp((char*)bdata->data, "GET", 3)) {// GET请求消息
            LOG_DEBUG("fd:%d idx:%d trans data:%s.", bdata->fd, 
                idx, (char*)(bdata->data));
        } else {// 其他消息
            if(bdata->data_len == 0) {
                LOG_DEBUG("fd:%d idx:%d trans without data.", bdata->fd, idx);
            } else {
                std::string hex = bin2hex(std::string((char*)(bdata->data), bdata->data_len));
                // if(!strncmp(hex.c_str(), "fffe", 4)) {
                    
                // }
                LOG_DEBUG("fd:%d idx:%d trans data:%s.", bdata->fd, idx, hex.c_str());
            }
        }
#endif
        int bwfdx = tgg_get_cli_bwfdx(bdata->coreid, bdata->fd);
        if(bwfdx && tgg_get_bwfdx_status((bwfdx & 0xf), bwfdx >> 8)) {
            // 已经绑定服务端，正常透传
            bdata->bwfdx = bwfdx;
            tgg_enqueue_bwsnd( (bwfdx & 0xf), bdata);
        } else {
            // 重新绑定或首次绑定，先绑定再透传
            int index = MAX_CALC_LOAD_BALANCE_TRY;
            while (--index) {
                // 随机取一个可用的服务端连接
                // int pos = bdata->fd % tgg_get_bwfdx_count();
                // bwfdx = tgg_get_bwfdx_bypos(pos);
                bwfdx = tgg_get_load_balance();
                if(bwfdx == -1) {
                    LOG_ERROR("get load balance failed.");
                    usleep(10);
                    continue;
                }
                if(bwfdx > 0 && tgg_get_bwfdx_status((bwfdx & 0xf), bwfdx >> 8)) {
                    break;
                }
            }
            if(index < MAX_CALC_LOAD_BALANCE_TRY - 1) {
                LOG_ERROR("get bwfdx for fd[%d] idx[%d] failed, tried times:%d.", 
                    bdata->fd, bdata->idx, MAX_CALC_LOAD_BALANCE_TRY-index);
            }
            if (bwfdx <= 0) {// 入队列失败之后，清理数据，否则上行队列会满，而无法接收新数据
                LOG_ERROR("get bwfdx failed.");
                clean_bw_data(bdata);
            } else {
                // 客户端连接绑定到服务端连接
                tgg_set_cli_bwfdx(bdata->coreid, bdata->fd, bwfdx);
                // 负载++
                tgg_add_bwfdx_load(bwfdx & 0xf, bwfdx >> 8);
                bdata->bwfdx = bwfdx;
                if (tgg_enqueue_bwsnd( (bwfdx & 0xf), bdata) < 0) {
                    // TODO 判断进程是否还在，不在了的话要做些什么操作
                    LOG_ERROR("enque bwsnd failed, bwfdx:%d.", bwfdx);
                    clean_bw_data(bdata);
                }
            }
        }
    }
    LOG_INFO("Trans thread ended.");
    return 0;
}

int init_bwtrans()
{
    LOG_INFO("Trans thread started.");
    return pthread_create(&s_bwtrans_thread, NULL, &deal_trans, NULL);
}

void uninit_bwtrans()
{
    void* retval = NULL;
    if (pthread_join(s_bwtrans_thread, &retval) < 0) {
        LOG_ERROR("join thread failed.");
    }
}
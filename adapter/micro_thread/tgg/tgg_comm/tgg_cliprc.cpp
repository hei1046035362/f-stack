#include "tgg_cliprc.h"
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/WsConsumer.h"

extern struct rte_mempool* g_mempool_read;


void tgg_process_read(int lcore_idx)
{
    while (g_run) {
        tgg_read_data* rdata = NULL;
        if (tgg_dequeue_cliprc(lcore_idx, &rdata) < 0) {
            // 队列空
            usleep(10);
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
    RTE_LOG(INFO, USER1, "[%s][%d] cliprc thread exit, handle lcore_idx:%d\n", __func__, __LINE__, lcore_idx);
}
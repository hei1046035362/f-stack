#include "tgg_common.h"
#include "tgg_bw_cache.h"
#include <rte_ring.h>
#include <rte_memzone.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include "tgg_lock.h"
#include "comm/TggLock.hpp"
#include <string.h>
#include <unistd.h>
#include <iostream>

extern int g_fd_limit;
extern struct rte_memzone* g_fd_zones[MAX_LCORE_COUNT];
extern struct rte_ring* g_ring_read;
extern struct rte_ring* g_ring_cliprcs[MAX_LCORE_COUNT];// 客户端上行
extern struct rte_ring* g_ring_writes[MAX_LCORE_COUNT];
extern struct rte_ring* g_ring_bwrcvs[MAX_LCORE_COUNT];
extern struct rte_ring* g_ring_trans;

extern struct rte_mempool* g_mempool_read;
extern struct rte_mempool* g_mempool_write;
extern struct rte_mempool* g_mempool_bwrcv;
extern char g_cid_str[21];  // 8位地址+4位端口+8位idx+1位结束符'\0'

tgg_stats g_tgg_stats = {0};


static bool s_big_endian = false;

union EndiannessTester {
    int integer;
    char bytes[sizeof(int)];
};

void init_endians()
{
    union EndiannessTester tester;
    tester.integer = 1;
    if (tester.bytes[0] == 1) {
        s_big_endian = true;
    } 
}

bool big_endian()
{
    return s_big_endian;
}



int get_valid_idx()
{
	int looptimes = 2;
	int current_id_atomic = 0;
	while (1) {
		// TODO  后续要考虑自增id超过uint32_max了怎么处理，
		rte_atomic32_inc(get_idx_lock());
		current_id_atomic = rte_atomic32_read(get_idx_lock());
		if(current_id_atomic > g_fd_limit) {
			rte_atomic32_init(get_idx_lock());
			looptimes--;
		}
		if(looptimes <= 0) {
			//rte_exit(-1, "nonIdx ");
			RTE_LOG(ERR, USER1, "[%s][%d] None idx available.", __func__, __LINE__);
			return -1;
		}
		if(tgg_check_idx_exist(current_id_atomic) < 0) {
			break;
		}
	}
	return current_id_atomic;
}

std::string get_valid_cid(int idx)
{
	std::string scid(g_cid_str, TGG_IPPORT_LEN);
    std::string rsp;
    rsp.resize(sizeof(int));
    memcpy(const_cast<char* >(rsp.data()), &idx, sizeof(int));
    return scid + rsp;
}


void tgg_close_cli(int core_id, int fd)
{
	SpinLock lock(get_cli_lock());
	tgg_cli_info* cli = &((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd];
	memset(cli->cid, 0, sizeof(cli->cid));
	memset(cli->uid, 0, sizeof(cli->uid));
	memset(cli->reserved, 0, sizeof(cli->reserved));
	cli->idx = TGG_FD_CLOSED;
	cli->authorized = 0;
	cli->status |= FD_STATUS_CLOSING | FD_STATUS_CLOSED;
}

int tgg_init_cli(int core_id, int fd, uint32_t ip, ushort port)
{
	SpinLock lock(get_cli_lock());
	tgg_cli_info* cli = &((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd];
	memset(cli->cid, 0, sizeof(cli->cid));
	memset(cli->uid, 0, sizeof(cli->uid));
	memset(cli->reserved, 0, sizeof(cli->reserved));
	cli->idx = get_valid_idx();
	if(cli->idx < 0) {
		return -1;
	}
	if (tgg_add_idx(cli->idx) < 0) {
		return -1;
	}
	cli->authorized = 0;
	cli->ip = ip;
	cli->port = port;
	cli->status = FD_STATUS_READYFORCONNECT;
	return 0;
}

int tgg_get_cli_idx(int core_id, int fd)
{
	SpinLock lock(get_cli_lock());
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].idx;	
}

int tgg_get_cli_status(int core_id, int fd)
{
	SpinLock lock(get_cli_lock());
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].status;	
}

int tgg_get_cli_authorized(int core_id, int fd)
{
	SpinLock lock(get_cli_lock());
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].authorized;	
}
uint32_t tgg_get_cli_ip(int core_id, int fd)
{
	SpinLock lock(get_cli_lock());
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].ip;
}
ushort tgg_get_cli_port(int core_id, int fd)
{
	SpinLock lock(get_cli_lock());
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].port;	
}

std::string tgg_get_cli_uid(int core_id, int fd)
{
	SpinLock lock(get_cli_lock());
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].uid;	
}

std::string tgg_get_cli_cid(int core_id, int fd)
{
	SpinLock lock(get_cli_lock());
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].cid;	
}

std::string tgg_get_cli_reserved(int core_id, int fd)
{
	SpinLock lock(get_cli_lock());
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].reserved;	
}

int tgg_set_cli_idx(int core_id, int fd, int idx)
{
	SpinLock lock(get_cli_lock());
	((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].idx = idx;
	return 0;
}

int tgg_set_cli_status(int core_id, int fd, int status)
{
	SpinLock lock(get_cli_lock());
	((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].status = status;	
	return 0;
}

int tgg_set_cli_authorized(int core_id, int fd, int authorized)
{
	SpinLock lock(get_cli_lock());
	((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].authorized = authorized;
	return 0;
}
int tgg_set_cli_ip(int core_id, int fd, uint32_t ip)
{
	SpinLock lock(get_cli_lock());
	((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].ip = ip;
	return 0;
}
int tgg_set_cli_port(int core_id, int fd, ushort port)
{
	SpinLock lock(get_cli_lock());
	((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].port = port;
	return 0;
}

int tgg_set_cli_uid(int core_id, int fd, const char* uid)
{
	SpinLock lock(get_cli_lock());
	memset(((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].uid, 0, sizeof(tgg_cli_info::uid));
	strncpy(((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].uid, uid, strlen(uid));
	return 0;
}

int tgg_set_cli_cid(int core_id, int fd, const char* cid)
{
	SpinLock lock(get_cli_lock());
	memset(((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].cid, 0, sizeof(tgg_cli_info::cid));
	strncpy(((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].cid, cid, strlen(cid));
	return 0;
}

int tgg_set_cli_reserved(int core_id, int fd, const char* reserved)
{
	SpinLock lock(get_cli_lock());
	memset(((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].reserved, 0, sizeof(tgg_cli_info::reserved));
	strncpy(((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].reserved, reserved, strlen(reserved));
	return 0;
}

int cache_ws_buffer(int core_id, int fd, void* data, int len, int pos, int iscomplete)
{
	char* buffer = (char*)dpdk_rte_malloc(len);
	if(!buffer) {
		return -1;
	}
	memcpy(buffer, data, len);
	SpinLock lock(get_cli_lock());
	tgg_ws_data* wsdata = (&((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd])->ws_data;
    if (!wsdata) {// 第一次缓存
    	wsdata = (tgg_ws_data*)dpdk_rte_malloc(sizeof(tgg_ws_data));
    	if (!wsdata) {
    		rte_free(buffer);
    		return -1;
    	}
    	wsdata->data_list = (tgg_ws_unit* )dpdk_rte_malloc(sizeof(tgg_ws_unit));
    	if (!wsdata->data_list) {
    		rte_free(buffer);
    		rte_free(wsdata);
    		return -1;
    	}
    	wsdata->data_list->data = buffer;
    	wsdata->data_list->len = len;
    	wsdata->data_list->pos = pos;
    	wsdata->data_list->next = NULL;
    	wsdata->total_len = len;
	    wsdata->head_complete = iscomplete ? 1 : 0;
	    (&((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd])->ws_data = wsdata;
	    return 0;

    }
    if (wsdata->total_len >= MAX_WSDATA_LEN) {
    	RTE_LOG(ERR, USER1, "[%s][%d] Cache buffer len[%d] beyond MAX_WSDATA_LEN.",
    		__func__, __LINE__, wsdata->total_len);
    	rte_free(buffer);
    	return -1;
    }
    tgg_ws_unit* punit = (tgg_ws_unit* )dpdk_rte_malloc(sizeof(tgg_ws_unit));
    if (!punit) {
   		rte_free(buffer);
    	return -1;
    }
    punit->data = buffer;
    punit->len = len;
    punit->pos = pos;
    wsdata->total_len += len;
    wsdata->head_complete = iscomplete ? 1 : 0;
    tgg_ws_unit* ptail = wsdata->data_list;
    if(!ptail) {
    	wsdata->data_list = punit;
    } else {
    	while(ptail->next) {
    		ptail = ptail->next;
    	}
    	ptail->next = punit;
	}

    return 0;
}
    
std::string get_one_frame_buffer(int core_id, int fd, void* data, int len)
{
	SpinLock lock(get_cli_lock());
	std::string buffer;
	tgg_ws_data* wsdata = (&((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd])->ws_data;
    if (!wsdata || wsdata->data_list) {// 没有数据
    	buffer = std::string((char*)data, len);
    	return buffer;
    }
    tgg_ws_unit* phead = wsdata->data_list;
    if (!wsdata->head_complete && phead) {
        // 数据头不完整，拿最后一个节点和当前数据拼接构成一个头，
        // 头部解析只需要两个字节，不可能存在于三个节点中，所以这里没有考虑头部分散在三个或以上节点中的情况
    	while(phead->next) {
    		phead = phead->next;
    	}
        // 取最后一个节点的数据和当前数据组成一个头
    	buffer += std::string((char*)phead->data, phead->len);
    }
    // 其他情况直接返回数据本身
    buffer += std::string((char*)data, len);
    return buffer;
}

std::string get_whole_buffer(int core_id, int fd)
{
	SpinLock lock(get_cli_lock());
	std::string buffer;
	tgg_ws_data* wsdata = (&((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd])->ws_data;
    if (!wsdata || !wsdata->data_list) {// 没有数据
        // buffer = std::string((char*)data + pos, len);
    	return buffer;
    }
    tgg_ws_unit* phead = wsdata->data_list;
    while(phead) {
    	buffer += std::string((char*)phead->data + phead->pos, phead->len - phead->pos);
    	phead = phead->next;
    }
    return buffer;
}

void clean_ws_buffer(int core_id, int fd)
{
	SpinLock lock(get_cli_lock());
    tgg_ws_data* wsdata = (&((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd])->ws_data;
    if (!wsdata) {// 没有数据
        return;
    }

    tgg_ws_unit* phead = wsdata->data_list;
    while(phead) {
        wsdata->data_list = phead->next;
        memset(phead->data, 0, phead->len);
        rte_free(phead->data);
        memset(phead, 0, sizeof(tgg_ws_unit));
        rte_free(phead);
        phead = wsdata->data_list;
    }
    memset(wsdata, 0, sizeof(tgg_ws_data));
    rte_free(wsdata);
    (&((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd])->ws_data = NULL;
}

int tgg_enqueue_read(tgg_read_data* data)
{
	return rte_ring_enqueue(g_ring_read, data);
}

int tgg_dequeue_read(tgg_read_data** data)
{
	if (rte_ring_empty(g_ring_read)) {
		return -ENOENT;
	}
	return rte_ring_dequeue(g_ring_read, (void**)data);
}

int tgg_enqueue_cliprc(int core_id, tgg_read_data* data)
{
	return rte_ring_enqueue(g_ring_cliprcs[core_id], data);
}

int tgg_dequeue_cliprc(int core_id, tgg_read_data** data)
{
	if (rte_ring_empty(g_ring_cliprcs[core_id])) {
		return -ENOENT;
	}
	return rte_ring_dequeue(g_ring_cliprcs[core_id], (void**)data);
}

int tgg_enqueue_write(int core_id, tgg_write_data* data)
{
	return rte_ring_enqueue(g_ring_writes[core_id], data);
}

int tgg_dequeue_write(int core_id, tgg_write_data** data)
{
	if (rte_ring_empty(g_ring_writes[core_id])) {
		return -ENOENT;
	}
	return rte_ring_dequeue(g_ring_writes[core_id], (void**)data);
}

int tgg_enqueue_trans(tgg_bw_data* data)
{
	return rte_ring_enqueue(g_ring_trans, data);
}

int tgg_dequeue_trans(tgg_bw_data** data)
{
	if (rte_ring_empty(g_ring_trans)) {
		return -ENOENT;
	}
	return rte_ring_dequeue(g_ring_trans, (void**)data);
}

int tgg_enqueue_bwrcv(int core_id, tgg_bw_data* data)
{
	return rte_ring_enqueue(g_ring_bwrcvs[core_id], data);
}

int tgg_dequeue_bwrcv(int core_id, tgg_bw_data** data)
{
	if (rte_ring_empty(g_ring_bwrcvs[core_id])) {
		return -ENOENT;
	}
	return rte_ring_dequeue(g_ring_bwrcvs[core_id], (void**)data);
}


void clean_bw_data(tgg_bw_data* bdata)
{
    if (bdata->data) {
    	memset(bdata->data, 0, bdata->data_len);
        rte_free(bdata->data);
    }
    memset(bdata, 0, sizeof(tgg_bw_data));
    rte_mempool_put(g_mempool_bwrcv, bdata);
}

void clean_read_data(tgg_read_data* rdata)
{
    if (rdata->data) {
    	memset(rdata->data, 0, rdata->data_len);
        rte_free(rdata->data);
    }
    memset(rdata, 0, sizeof(tgg_read_data));
    rte_mempool_put(g_mempool_read, rdata);
}

void clean_write_data(tgg_write_data* wdata)
{
    if (wdata->data) {
    	memset(wdata->data, 0, wdata->data_len);
        rte_free(wdata->data);
    }
    memset(wdata, 0, sizeof(tgg_write_data));
    rte_mempool_put(g_mempool_write, wdata);
}

tgg_write_data* format_send_data(const std::string& sdata, std::map<int, int>& mapfdidx, int fdopt)
{
	tgg_write_data* wdata = NULL;
	int ret = rte_mempool_get(g_mempool_write, (void**)&wdata);
        // TODO  建议增加循环处理，内存池不够，可以稍微等待消费端释放
	if (ret < 0) {
		RTE_LOG(ERR, USER1, "[%s][%d] get mem from write pool failed,code:%d.", 
			__func__, __LINE__, ret);
		return NULL;
	}
	tgg_fd_list* tail = NULL;
	tgg_fd_list* pcur = NULL;
	tgg_fd_list* head = NULL;
	std::map<int, int>::iterator it = mapfdidx.begin();
	while (it != mapfdidx.end()) {
		pcur = (tgg_fd_list*)dpdk_rte_malloc(sizeof(tgg_fd_list));
		if (!pcur) {
            // TODO 如果只有一个失败了，其他的是不是可以继续发送，而不是全部都不发了
			goto add_data_failed;
		}
		pcur->fdid = it->first;
		pcur->idx = it->second;
		if (!tail) {
			tail = pcur;
			head = tail;
		}
		else {
			tail->next = pcur;
			tail = tail->next;
		}
		it++;
	}
	if (head) {
		wdata->lst_fd = head;
	} else {
		goto add_data_failed;
	}
	if (sdata.length() > 0) {
		wdata->data = dpdk_rte_malloc(sdata.length());
		if (!wdata->data) {
			goto add_data_failed;
		}
		memcpy((char*)(wdata->data), sdata.c_str(), sdata.length());
		wdata->data_len = sdata.length();
	} else {
		wdata->data_len = 0;
	}
	wdata->fd_opt = fdopt;
	return wdata;

add_data_failed:
	RTE_LOG(ERR, USER1, "[%s][%d] dpdk_rte_malloc mem failed.", 
		__func__, __LINE__);
	iter_del_fdlist((void*)(wdata->lst_fd));
	memset(wdata, 0, sizeof(tgg_write_data));
	rte_mempool_put(g_mempool_write, wdata);
	return NULL;
}

int enqueue_data_batch_fd(int core_id, const std::string& data, std::map<int, int>& mapfdidx, int fdopt)
{
	if(mapfdidx.size() <= 0) {
		// fd列表为空
		RTE_LOG(ERR, USER1, "[%s][%d] mapfdidx is empty.", 
			__func__, __LINE__);
		return 0;
	}
	tgg_write_data* wdata = format_send_data(data, mapfdidx, fdopt);
	if (!wdata) {
		RTE_LOG(ERR, USER1, "[%s][%d] Format send data failed.", 
			__func__, __LINE__);
		return -1;
	}
	int idx = 10;
	while (tgg_enqueue_write(core_id, wdata) < 0 && idx-- > 0 ) {
		usleep(10);
	}
	if (idx <= 0) {
		RTE_LOG(ERR, USER1, "[%s][%d] Enqueue write data failed.", 
			__func__, __LINE__);
		return -1;
	}
	return 0;

}

int enqueue_data_single_fd(int core_id, const std::string& data, int fd, int idx, int fdopt)
{
	std::map<int, int> mapfdidx;
	mapfdidx[fd] = idx;
	return enqueue_data_batch_fd(core_id, data, mapfdidx, fdopt);
}

#include <sys/prctl.h>
void init_core(const char* dumpfile)
{
	// 设置 core 文件的路径
    prctl(PR_SET_DUMPABLE, 1);  // 确保程序可以生成 core 文件
    char core_path[256];
    snprintf(core_path, sizeof(core_path), "%s.%d", dumpfile, getpid());
    prctl(PR_SET_DUMPABLE, core_path, 0, 0, 0);
}

void* dpdk_rte_malloc(int size)
{
	void* pdata = rte_malloc("tgg_malloc", size, 0);
	if (!pdata)	{
		RTE_LOG(ERR, USER1, "malloc data failed.");
	}
	// TODO 这里需要把pdata管理起来，因dpdk的secondary进程出core而未释放时会导致大页内存泄漏
	// 		可以用链表管理起来，然后注册rte_service给master进程去管理，也可以放到定时任务管理
	return pdata;
}

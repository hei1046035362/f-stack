#include "tgg_common.h"
#include "tgg_bw_cache.h"
#include <rte_ring.h>
#include <rte_memzone.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include "tgg_lock_struct.h"
#include "comm/TggLock.hpp"
#include "comm/RedisClient.hpp"
#include <string.h>
#include <unistd.h>

extern struct rte_memzone* g_fd_zone;
extern int g_fd_limit;
extern struct rte_ring* g_ring_read;
extern struct rte_ring* g_ring_write;
extern struct rte_ring* g_ring_bwrcv;
extern struct rte_mempool* g_mempool_read;
extern struct rte_mempool* g_mempool_write;
extern struct rte_mempool* g_mempool_bwrcv;

tgg_stats g_tgg_stats = {0};

void tgg_close_cli(int fd)
{
	SpinLock lock(get_cli_lock());
	if (fd < 0 || fd >= g_fd_limit)	{
		RTE_LOG(INFO, USER1, "given fd[%d] is invalid,[0,%d]",
			fd, g_fd_limit - 1);
		return ;
	}
	tgg_cli_info* cli = &((tgg_cli_info*)g_fd_zone->addr)[fd];
	memset(cli->cid, 0, sizeof(cli->cid));
	memset(cli->uid, 0, sizeof(cli->uid));
	memset(cli->reserved, 0, sizeof(cli->reserved));
	cli->idx = -1;
	cli->authorized = 0;
	cli->status |= FD_STATUS_CLOSING | FD_STATUS_CLOSED;
}

int tgg_get_cli_idx(int fd)
{
	SpinLock lock(get_cli_lock());
	if (fd < 0 || fd >= g_fd_limit)
		return -1;
	return ((tgg_cli_info*)g_fd_zone->addr)[fd].idx;	
}

int tgg_get_cli_status(int fd)
{
	SpinLock lock(get_cli_lock());
	if (fd < 0 || fd >= g_fd_limit)
		return -1;
	return ((tgg_cli_info*)g_fd_zone->addr)[fd].status;	
}

int tgg_get_cli_authorized(int fd)
{
	SpinLock lock(get_cli_lock());
	if (fd < 0 || fd >= g_fd_limit)
		return -1;
	return ((tgg_cli_info*)g_fd_zone->addr)[fd].authorized;	
}

std::string tgg_get_cli_uid(int fd)
{
	SpinLock lock(get_cli_lock());
	if (fd < 0 || fd >= g_fd_limit)
		return "";
	return ((tgg_cli_info*)g_fd_zone->addr)[fd].uid;	
}

std::string tgg_get_cli_cid(int fd)
{
	SpinLock lock(get_cli_lock());
	if (fd < 0 || fd >= g_fd_limit)
		return "";
	return ((tgg_cli_info*)g_fd_zone->addr)[fd].cid;	
}

std::string tgg_get_cli_reserved(int fd)
{
	SpinLock lock(get_cli_lock());
	if (fd < 0 || fd >= g_fd_limit)
		return "";
	return ((tgg_cli_info*)g_fd_zone->addr)[fd].reserved;	
}

int tgg_set_cli_idx(int fd, int idx)
{
	SpinLock lock(get_cli_lock());
	if (fd < 0 || fd >= g_fd_limit)
		return -1;
	((tgg_cli_info*)g_fd_zone->addr)[fd].idx = idx;
	return 0;
}

int tgg_set_cli_status(int fd, int status)
{
	SpinLock lock(get_cli_lock());
	if (fd < 0 || fd >= g_fd_limit)
		return -1;
	((tgg_cli_info*)g_fd_zone->addr)[fd].status = status;	
	return 0;
}

int tgg_set_cli_authorized(int fd, int authorized)
{
	SpinLock lock(get_cli_lock());
	if (fd < 0 || fd >= g_fd_limit)
		return -1;
	((tgg_cli_info*)g_fd_zone->addr)[fd].authorized = authorized;
	return 0;
}

int tgg_set_cli_uid(int fd, const char* uid)
{
	SpinLock lock(get_cli_lock());
	if (fd < 0 || fd >= g_fd_limit)
		return -1;
	strncpy(((tgg_cli_info*)g_fd_zone->addr)[fd].uid, uid, strlen(uid));
	return 0;
}

int tgg_set_cli_cid(int fd, const char* cid)
{
	SpinLock lock(get_cli_lock());
	if (fd < 0 || fd >= g_fd_limit)
		return -1;
	strncpy(((tgg_cli_info*)g_fd_zone->addr)[fd].cid, cid, strlen(cid));
	return 0;
}

int tgg_set_cli_reserved(int fd, const char* reserved)
{
	SpinLock lock(get_cli_lock());
	if (fd < 0 || fd >= g_fd_limit)
		return -1;
	strncpy(((tgg_cli_info*)g_fd_zone->addr)[fd].reserved, reserved, strlen(reserved));
	return 0;
}

int cache_ws_buffer(int fd, void* data, int len, int pos, int iscomplete)
{
	SpinLock lock(get_cli_lock());
	tgg_ws_data* wsdata = (&((tgg_cli_info*)g_fd_zone->addr)[fd])->ws_data;
    if (!wsdata) {// 第一次缓存
    	wsdata = (tgg_ws_data*)dpdk_rte_malloc(sizeof(tgg_ws_data));
    	if (!wsdata)
    		return -1;
    	wsdata->data_list = (tgg_ws_unit* )dpdk_rte_malloc(sizeof(tgg_ws_unit));
    }
    if (wsdata->total_len >= MAX_WSDATA_LEN) {
    	RTE_LOG(ERR, USER1, "[%s][%d] Cache buffer len[%d] beyond MAX_WSDATA_LEN.",
    		__func__, __LINE__, wsdata->total_len);
    	return -1;
    }
    tgg_ws_unit* ptail = wsdata->data_list;
    if (!ptail) return -1;

    while(ptail->next) {
    	ptail = ptail->next;
    }
    tgg_ws_unit* punit = (tgg_ws_unit* )dpdk_rte_malloc(sizeof(tgg_ws_unit));
    if (!punit) return -1;
    punit->data = data;
    punit->len = len;
    punit->pos = pos;
    ptail->next = punit;
    wsdata->total_len += len;
    wsdata->head_complete = iscomplete ? 1 : 0;
    return 0;
}
    
std::string get_one_frame_buffer(int fd, void* data, int len)
{
	SpinLock lock(get_cli_lock());
	std::string buffer;
	tgg_ws_data* wsdata = (&((tgg_cli_info*)g_fd_zone->addr)[fd])->ws_data;
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

std::string get_whole_buffer(int fd)
{
	SpinLock lock(get_cli_lock());
	std::string buffer;
	tgg_ws_data* wsdata = (&((tgg_cli_info*)g_fd_zone->addr)[fd])->ws_data;
    if (!wsdata || wsdata->data_list) {// 没有数据
        // buffer = std::string((char*)data + pos, len);
    	return buffer;
    }
    tgg_ws_unit* phead = wsdata->data_list;
    while(phead) {
    	buffer += std::string((char*)phead->data + phead->pos, phead->len);
    	phead = phead->next;
    }
    return buffer;
}

void clean_ws_buffer(int fd)
{
	SpinLock lock(get_cli_lock());
    tgg_ws_data* wsdata = (&((tgg_cli_info*)g_fd_zone->addr)[fd])->ws_data;
    if (!wsdata) {// 没有数据
        return;
    }

    tgg_ws_unit* phead = wsdata->data_list;
    while(phead) {
        phead->len = 0;
        phead->pos = 0;
        memset(phead->data, 0, phead->len);
        rte_free(phead);
        wsdata->data_list = phead->next;
        phead = wsdata->data_list;
    }
    wsdata->head_complete = 0;
    wsdata->total_len = 0;
    rte_free(wsdata);
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

int tgg_enqueue_write(tgg_write_data* data)
{
	return rte_ring_enqueue(g_ring_write, data);
}

int tgg_dequeue_write(tgg_write_data** data)
{
	if (rte_ring_empty(g_ring_write)) {
		return -ENOENT;
	}
	return rte_ring_dequeue(g_ring_write, (void**)data);
}

int tgg_enqueue_bwrcv(tgg_bw_data* data)
{
	return rte_ring_enqueue(g_ring_bwrcv, data);
}

int tgg_dequeue_bwrcv(tgg_bw_data** data)
{
	if (rte_ring_empty(g_ring_bwrcv)) {
		return -ENOENT;
	}
	return rte_ring_dequeue(g_ring_bwrcv, (void**)data);
}

void clean_bw_data(tgg_bw_data* bdata)
{
    if (bdata->data) {
        rte_free(bdata->data);
    }
    memset(bdata, 0, sizeof(tgg_bw_data));
    rte_mempool_put(g_mempool_bwrcv, bdata);
}

void clean_read_data(tgg_read_data* rdata)
{
    if (rdata->data) {
        rte_free(rdata->data);
    }
    memset(rdata, 0, sizeof(tgg_read_data));
    rte_mempool_put(g_mempool_read, rdata);
}

void clean_write_data(tgg_write_data* wdata)
{
    if (wdata->data) {
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
		pcur->fd = it->first;
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

int enqueue_data_batch_fd(const std::string& data, std::map<int, int>& mapfdidx, int fdopt)
{
	tgg_write_data* wdata = format_send_data(data, mapfdidx, fdopt);
	if (!wdata) {
		RTE_LOG(ERR, USER1, "[%s][%d] Format send data failed.", 
			__func__, __LINE__);
		return -1;
	}
	int idx = 10;
	while (tgg_enqueue_write(wdata) < 0 && idx-- > 0 ) {
		usleep(10);
	}
	if (idx <= 0) {
		RTE_LOG(ERR, USER1, "[%s][%d] Enqueue write data failed.", 
			__func__, __LINE__);
		return -1;
	}
	return 0;

}

int enqueue_data_single_fd(const std::string& data, int fd, int idx, int fdopt)
{
	std::map<int, int> mapfdidx;
	mapfdidx[fd] = idx;
	return enqueue_data_batch_fd(data, mapfdidx, fdopt);
}

int tgg_init_uidgid(const std::vector<std::string>& clusterNodes, const std::string& password, const std::string& userName)
{
    std::map<std::string, std::set<std::string>> mapUsers;
	int ret = GetUserWithGids(clusterNodes, mapUsers, password);
	if (!ret) {
		RTE_LOG(ERR, USER1, "[%s][%d] Connect redis failed.", __func__, __LINE__);
		return ret;
	}

    std::map<std::string, std::set<std::string>>::iterator itUid = mapUsers.begin();
    while(itUid != mapUsers.end()) {
        std::set<std::string>::iterator itGid = itUid->second.begin();
        while(itGid != itUid->second.end()) {
			tgg_add_uidgid(itUid->first.c_str(), (*itGid).c_str());
            //std::cout << "userId[" << itUid->first << "]: Gid[" << *itGid << "]" << std::endl;
            itGid++;
        }
        itUid++;
    }
	return 0;
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

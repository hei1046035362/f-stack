#include "tgg_common.h"
#include "tgg_bw_cache.h"
#include "cmd/GatewayProtocal.h"
#include "tgg_conf.h"
#include <rte_ring.h>
#include <rte_memzone.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include "tgg_lock.h"
#include "comm/TggLock.hpp"
#include <string.h>
#include <unistd.h>
#include <iostream>
#include "comm/log.hpp"

extern int g_fd_limit;
extern struct rte_memzone* g_fd_zones[MAX_LCORE_COUNT];
extern int g_bwfdx_limit;
extern struct rte_memzone* g_bwfdx_zones[MAX_LCORE_COUNT];
extern struct rte_memzone* g_bwprc_zone;
extern struct rte_ring* g_ring_read;
extern struct rte_ring* g_ring_cliprcs[MAX_LCORE_COUNT];// 客户端上行
extern struct rte_ring* g_ring_writes[MAX_LCORE_COUNT];
extern struct rte_ring* g_ring_bwrcvs[MAX_LCORE_COUNT];
extern struct rte_ring* g_ring_trans;
extern struct rte_ring* g_ring_bwsnds[MAX_LCORE_COUNT];

extern struct rte_mempool* g_mempool_read;
extern struct rte_mempool* g_mempool_write;
extern struct rte_mempool* g_mempool_bwrcv;
extern struct rte_mempool* g_mempool_read_data;
extern struct rte_mempool* g_mempool_write_data;
extern struct rte_mempool* g_mempool_bwrcv_data;
extern struct rte_mempool* g_mempool_large_data;
extern struct rte_mempool* g_mempool_clifdlist_data;

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
		if(current_id_atomic >= g_fd_limit) {
			rte_atomic32_init(get_idx_lock());
			rte_atomic32_inc(get_idx_lock());// idx要从1开始  0(ready)和-1(closed)已经被用作其他功能了
			looptimes--;
		}
		if(looptimes <= 0) {
			//rte_exit(-1, "nonIdx ");
			LOG_ERROR("None idx available.");
			return -1;
		}
		if(tgg_check_idx_exist(current_id_atomic) < 0) {
			break;
		}
	}
	LOG_INFO("valid idx %d.", current_id_atomic);
	return current_id_atomic;
}

void tgg_close_cli(int core_id, int fd)
{
	SpinLock lock(get_cli_lock());
	tgg_cli_info* cli = &((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd];
	cli->cid = 0;
	memset(cli->uid, 0, sizeof(cli->uid));
	memset(cli->reserved, 0, sizeof(cli->reserved));
	cli->idx = TGG_FD_CLOSED;
	cli->authorized = AUTH_TYPE_UNKNOWN;
}

int tgg_init_cli(int core_id, int fd, char* ip_str, uint32_t ip, ushort port)
{
	SpinLock lock(get_cli_lock());
	tgg_cli_info* cli = &((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd];
	cli->cid = 0;
	memset(cli->uid, 0, sizeof(cli->uid));
	memset(cli->reserved, 0, sizeof(cli->reserved));
	cli->idx = get_valid_idx();
	if(cli->idx < 0) {
		LOG_ERROR("init client failed, invalid idx:%d core id:%d fd:%d.", cli->idx, core_id, fd);
		return -1;
	}
	if (tgg_add_idx(cli->idx) < 0) {
		LOG_ERROR("init client failed, add idx:%d failed, core id:%d fd:%d.", cli->idx, core_id, fd);
		return -1;
	}
	cli->authorized = 0;
	memcpy(cli->ip_str, ip_str, INET_ADDRSTRLEN);
	cli->ip = ip;
	cli->port = port;
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
std::string tgg_get_cli_ip_str(int core_id, int fd)
{
	SpinLock lock(get_cli_lock());
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].ip_str;
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

int tgg_get_cli_bwfdx(int core_id, int fd)
{
	SpinLock lock(get_cli_lock());
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].bwfdx;	
}

std::string tgg_get_cli_uid(int core_id, int fd)
{
	SpinLock lock(get_cli_lock());
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].uid;	
}

int tgg_get_cli_cid(int core_id, int fd)
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

int tgg_set_cli_bwfdx(int core_id, int fd, int bwfdx)
{
	SpinLock lock(get_cli_lock());
	((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].bwfdx = bwfdx;
	return 0;
}

int tgg_set_cli_uid(int core_id, int fd, const char* uid)
{
	SpinLock lock(get_cli_lock());
	memset(((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].uid, 0, sizeof(tgg_cli_info::uid));
	strncpy(((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].uid, uid, strlen(uid));
	return 0;
}

int tgg_set_cli_cid(int core_id, int fd, int cid)
{
	SpinLock lock(get_cli_lock());
	((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].cid = cid;
	return 0;
}

int tgg_set_cli_reserved(int core_id, int fd, const char* reserved)
{
	SpinLock lock(get_cli_lock());
	memset(((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].reserved, 0, sizeof(tgg_cli_info::reserved));
	strncpy(((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].reserved, reserved, strlen(reserved));
	return 0;
}

void tgg_init_bwfdx_prc(int prc_id)
{
	tgg_iter_del_bwfdx(prc_id);
	for(int i = 0; i < g_bwfdx_limit; ++i) {
		if(tgg_get_bwfdx_status(prc_id, i)) {
			tgg_close_bw_session(prc_id, i);
		}
	}
}

int tgg_get_bwfdx_status(int prc_id, int fd)
{
	SpinLock lock(get_bwfdx_lock());
	return ((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].status;
}

int tgg_get_bwfdx_load(int prc_id, int fd)
{
	SpinLock lock(get_bwfdx_lock());
	return ((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].load;
}

int tgg_get_bwfdx_cmd(int prc_id, int fd)
{
	SpinLock lock(get_bwfdx_lock());
	return ((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].cmd;
}

int tgg_get_bwfdx_idx(int prc_id, int fd)
{
	SpinLock lock(get_bwfdx_lock());
	return ((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].idx;
}
int tgg_get_bwfdx_authorized(int prc_id, int fd)
{
	SpinLock lock(get_bwfdx_lock());
	return ((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].authorized;
}
int tgg_get_bwfdx_ip(int prc_id, int fd)
{
	SpinLock lock(get_bwfdx_lock());
	return ((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].ip;
}
int tgg_get_bwfdx_port(int prc_id, int fd)
{
	SpinLock lock(get_bwfdx_lock());
	return ((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].port;
}
std::string tgg_get_bwfdx_seckey(int prc_id, int fd)
{
	SpinLock lock(get_bwfdx_lock());
	return ((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].secretkey;
}
std::string tgg_get_bwfdx_workerkey(int prc_id, int fd)
{
	SpinLock lock(get_bwfdx_lock());
	return ((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].workerkey;
}

int tgg_set_bwfdx_status(int prc_id, int fd, int status)
{
	SpinLock lock(get_bwfdx_lock());
	((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].status = status;
	return 0;
}

int tgg_set_bwfdx_load(int prc_id, int fd, int load)
{
	SpinLock lock(get_bwfdx_lock());
	((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].load = load;
	return 0;
}

int tgg_add_bwfdx_load(int prc_id, int fd)
{
	SpinLock lock(get_bwfdx_lock());
	++(((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].load);
	return 0;
}

int tgg_set_bwfdx_cmd(int prc_id, int fd, int cmd)
{
	SpinLock lock(get_bwfdx_lock());
	((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].cmd = cmd;
	return 0;
}

int tgg_set_bwfdx_idx(int prc_id, int fd, int idx)
{
	SpinLock lock(get_bwfdx_lock());
	((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].idx = idx;
	return 0;
}
int tgg_set_bwfdx_authorized(int prc_id, int fd, int authorized)
{
	SpinLock lock(get_bwfdx_lock());
	((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].authorized = authorized;
	return 0;
}
int tgg_set_bwfdx_ip(int prc_id, int fd, uint32_t ip)
{
	SpinLock lock(get_bwfdx_lock());
	((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].ip = ip;
	return 0;
}
int tgg_set_bwfdx_port(int prc_id, int fd, ushort port)
{
	SpinLock lock(get_bwfdx_lock());
	((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].port = port;
	return 0;
}
int tgg_set_bwfdx_seckey(int prc_id, int fd, const char* secretkey)
{
	SpinLock lock(get_cli_lock());
	memset(((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].secretkey, 0, sizeof(tgg_bw_info::secretkey));
	strncpy(((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].secretkey, secretkey, strlen(secretkey));
	return 0;
}

int tgg_set_bwfdx_workerkey(int prc_id, int fd, const char* workerkey)
{
	SpinLock lock(get_cli_lock());
	memset(((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].workerkey, 0, sizeof(tgg_bw_info::workerkey));
	strncpy(((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].workerkey, workerkey, strlen(workerkey));
	return 0;
}

int tgg_get_bw_prcstatus(int prc_id)
{
	return tgg_get_bwfdx_status(prc_id, 0);
}

int tgg_set_bw_prcstatus(int prc_id, int status)
{
	return tgg_set_bwfdx_status(prc_id, 0, status);
}

void tgg_new_bw_session(int prc_id, int fd, int cmd,
						const char* workerkey, uint32_t remote_ip, ushort remote_port)
{
	tgg_clean_bwfdx(prc_id, fd);
	tgg_set_bwfdx_workerkey(prc_id, fd, workerkey);
	tgg_set_bwfdx_cmd(prc_id, fd, cmd);
    tgg_set_bwfdx_ip(prc_id, fd, remote_ip);
    tgg_set_bwfdx_port(prc_id, fd, remote_port);
	tgg_set_bwfdx_authorized(prc_id, fd, 1);
	tgg_set_bwfdx_status(prc_id, fd, 1);
	// int cmd = tgg_get_bwfdx_cmd(prc_id, fd);
	if(cmd == GatewayProtocal::CMD_WORKER_CONNECT) {
		std::string workerkey = tgg_get_bwfdx_workerkey(prc_id, fd);
		tgg_add_bwwkkey(workerkey.c_str());
	}
}

void tgg_close_bw_session(int prc_id, int fd)
{
	int cmd = tgg_get_bwfdx_cmd(prc_id, fd);
	if(cmd == GatewayProtocal::CMD_WORKER_CONNECT) {
		tgg_del_bwfdx((fd << 8) | (prc_id & 0xff));
		std::string workerkey = tgg_get_bwfdx_workerkey(prc_id, fd);
		tgg_del_bwwkkey(workerkey.c_str());
	}
	LOG_INFO("close bw session prc:[%d] fd:[%d], left bw count:%d.", 
		prc_id, fd, tgg_get_bwfdx_count());
	tgg_clean_bwfdx(prc_id, fd);
}

int tgg_clean_bwfdx(int prc_id, int fd)
{
	SpinLock lock(get_bwfdx_lock());
	// tgg_bw_info* bw = &((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd];
	memset(&(((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd]), 0, sizeof(tgg_bw_info));
	// bw->idx = get_valid_idx();
	// if(bw->idx < 0) {
	// 	return -1;
	// }
	// if (tgg_add_idx(bw->idx) < 0) {
	// 	return -1;
	// }
	// bw->authorized = 0;
	// bw->ip = 0;
	// bw->port = 0;
	// bw->status = 0;
	// bw->cmd = 0;
	return 0;
}

// 获取有效的进程序号
int tgg_get_valid_bwprc(int bwcount, uint64_t now)
{
	for (int i = 0; i < bwcount; i++) {
		SpinLock lock(get_bwprc_lock());
		pid_data* prc = (pid_data*)(g_bwprc_zone->addr) + i;
		// 如果超过两倍心跳的时间都没有更新，就视为前一个进程已退出
		if (prc->heart_beat == 0 || prc->heart_beat + 2*BW_PRC_HEART_BEAT < now) {
			prc->heart_beat = now;
			prc->pid = getpid();
			return i;
		}
	}
	return -1;
}

// 更新心跳
void tgg_update_bwprc(int prc_id, uint64_t now)
{
	SpinLock lock(get_bwprc_lock());
	pid_data* prc = (pid_data*)g_bwprc_zone->addr + prc_id;
	prc->heart_beat = now;
}

// 获取指定下标的进程id
int tgg_get_bwprc_pid(int prc_id)
{
	SpinLock lock(get_bwprc_lock());
	pid_data* prc = (pid_data*)g_bwprc_zone->addr + prc_id;
	return prc->pid;
}

// 检查指定进程是否超时
int tgg_checkif_bwprc_timeout(int prc_id, uint64_t now)
{
	SpinLock lock(get_bwprc_lock());
	pid_data* prc = (pid_data*)(g_bwprc_zone->addr) + prc_id;
	// 如果超过两倍心跳的时间都没有更新，就视为前一个进程已退出
	if (now - prc->heart_beat > 2*BW_PRC_HEART_BEAT) {
		return 1;// 超时
	}
	return 0;// 没超时
}

// 进程退出前主动清理，下一个进程就能快速启动
void tgg_clean_bwprc(int prc_id)
{
	SpinLock lock(get_bwprc_lock());
	pid_data* prc = (pid_data*)g_bwprc_zone->addr + prc_id;
	memset(prc, 0, sizeof(pid_data));
}


int ringbuf_read(int core_id, int fd, std::string& dest, int len, int move_pos)
{
	// 同一个连接的数据都是串行的，同一个连接的ws的缓存只有cliprc进程处理，不需要加锁
	tgg_ws_data* wsdata = &((&((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd])->ws_data);
    if (!wsdata->data) {// 第一次缓存
    	LOG_DEBUG("get ws data failed.");
	    return 0;
    }
    int data_size = (wsdata->write_pos >= wsdata->read_pos) ? 
                     (wsdata->write_pos - wsdata->read_pos) : 
                     (wsdata->capacity - wsdata->read_pos + wsdata->write_pos);
    if (data_size < len) len = data_size;

    // 分两段读取
    int first_chunk = (wsdata->read_pos + len > wsdata->capacity) ? 
                       (wsdata->capacity - wsdata->read_pos) : len;
    
    dest.append(static_cast<const char*>(wsdata->data) + wsdata->read_pos, first_chunk);
    
    if (len > first_chunk) {
        dest.append(static_cast<const char*>(wsdata->data), len - first_chunk);
    }
    if(move_pos) {
    	wsdata->read_pos = (wsdata->read_pos + len) % wsdata->capacity;
    }
    return len;
}

int ringbuf_write(int core_id, int fd, const char* data, int len)
{
	tgg_ws_data* wsdata = &((&((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd])->ws_data);
    if (!wsdata->data) {// 第一次缓存
    	wsdata->data = dpdk_rte_malloc(DEFAULT_WSDATA_LEN);
    	if (!wsdata->data) {
    		LOG_ERROR("malloc memery failed.");
    		return -1;
    	}
    	memset(wsdata->data, 0, DEFAULT_WSDATA_LEN);
    	wsdata->capacity = DEFAULT_WSDATA_LEN;
    	wsdata->read_pos = 0;
    	wsdata->write_pos = 0;
    }
    int free_space = wsdata->capacity - ((wsdata->write_pos >= wsdata->read_pos) ? 
                      (wsdata->write_pos - wsdata->read_pos) : 
                      (wsdata->capacity - wsdata->read_pos + wsdata->write_pos));
    if (free_space < len) len = free_space;

    // 分两段写入
    int first_chunk = wsdata->capacity - wsdata->write_pos;
    if (first_chunk > len) first_chunk = len;
    
    memcpy((char*)wsdata->data + wsdata->write_pos, data, first_chunk);
    
    if (len > first_chunk) {
        memcpy(wsdata->data, data + first_chunk, len - first_chunk);
    }
    
    wsdata->write_pos = (wsdata->write_pos + len) % wsdata->capacity;
    return len;
}

// 获取缓冲区中可读数据大小
int ringbuf_size(int core_id, int fd)
{
	tgg_ws_data* wsdata = &((&((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd])->ws_data);
    if (!wsdata->data) {// 没有缓存数据
    	// LOG_DEBUG("get ws data failed.");
	    return 0;
    }
    if (wsdata->write_pos >= wsdata->read_pos) {
        return wsdata->write_pos - wsdata->read_pos;
    }
    return wsdata->capacity - wsdata->read_pos + wsdata->write_pos;
}

int ringbuf_space(int core_id, int fd)
{
	tgg_ws_data* wsdata = &((&((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd])->ws_data);
    if (!wsdata->data) {// 没有缓存数据
    	LOG_DEBUG("get ws data failed.");
	    return -1;
    }
    return wsdata->capacity - ringbuf_size(core_id, fd) - 1;
}
    
std::string get_one_frame_buffer(int core_id, int fd, void* data, int len)
{
	// SpinLock lock(get_cli_lock());
	std::string buffer;
    int reserved_len = ringbuf_size(core_id, fd);
    if (reserved_len <= 0) {// 上一次缓存没有遗留数据
    	buffer.append(static_cast<const char*>(data), len);
    	return buffer;
    }
    ringbuf_read(core_id, fd, buffer, reserved_len, 1);
    // 把当前数据附加进去
	buffer.append(static_cast<const char*>(data), len);
    return buffer;
}

std::string get_whole_buffer(int core_id, int fd)
{
	// SpinLock lock(get_cli_lock());
	std::string buffer;
    int reserved_len = ringbuf_size(core_id, fd);
    if (reserved_len <= 0) {// 上一次缓存没有遗留数据
    	return buffer;
    }
    // 取上一次剩余数据
    ringbuf_read(core_id, fd, buffer, reserved_len, 0);
    return buffer;
}

void clean_ws_buffer(int core_id, int fd)
{
    // tgg_ws_data* wsdata = &((&((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd])->ws_data);
    // if (!wsdata->data) {// 没有数据
    //     return;
    // }
    // // 释放内存
    // memset(wsdata->data, 0, );
    // wsdata->len = 0;
    // wsdata->pos = 0;
}
void release_ws_buffer(int core_id, int fd)
{
    tgg_ws_data* wsdata = &((&((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd])->ws_data);
    if (!wsdata->data) {// 没有数据
        return;
    }
    // 释放内存
    rte_free(wsdata->data);
    memset(wsdata, 0, sizeof(tgg_ws_data));
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

int tgg_enqueue_bwsnd(int queue_id, tgg_bw_data* data)
{
	return rte_ring_enqueue(g_ring_bwsnds[queue_id], data);
}

int tgg_dequeue_bwsnd(int queue_id, tgg_bw_data** data)
{
	if (rte_ring_empty(g_ring_bwsnds[queue_id])) {
		return -ENOENT;
	}
	return rte_ring_dequeue(g_ring_bwsnds[queue_id], (void**)data);
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

int tgg_enqueue_bwrcv(int prc_id, tgg_bw_data* data)
{
	return rte_ring_enqueue(g_ring_bwrcvs[prc_id], data);
}

int tgg_dequeue_bwrcv(int prc_id, tgg_bw_data** data)
{
	if (rte_ring_empty(g_ring_bwrcvs[prc_id])) {
		return -ENOENT;
	}
	return rte_ring_dequeue(g_ring_bwrcvs[prc_id], (void**)data);
}


void clean_bw_data(tgg_bw_data* bdata)
{
    if (bdata->data) {
    	memset(bdata->data, 0, bdata->data_len);
        high_freq_free(g_mempool_bwrcv_data, bdata->data, bdata->data_len);
        bdata->data = NULL;
    }
    memset(bdata, 0, sizeof(tgg_bw_data));
    rte_mempool_put(g_mempool_bwrcv, bdata);
}

void clean_read_data(tgg_read_data* rdata)
{
    if (rdata->data) {
    	memset(rdata->data, 0, rdata->data_len);
        high_freq_free(g_mempool_read_data, rdata->data, rdata->data_len);
        rdata->data = NULL;
    }
    memset(rdata, 0, sizeof(tgg_read_data));
    rte_mempool_put(g_mempool_read, rdata);
}

void clean_write_data(tgg_write_data* wdata)
{
    if (wdata->data) {
    	memset(wdata->data, 0, wdata->data_len);
        high_freq_free(g_mempool_write_data, wdata->data, wdata->data_len);
        wdata->data = NULL;
    }
    memset(wdata, 0, sizeof(tgg_write_data));
    rte_mempool_put(g_mempool_write, wdata);
}

void clean_fdidlist(tgg_fd_id_list* fdiddata)
{
    if (!fdiddata) {
        return;
    }
    tgg_fd_id_list* iter = fdiddata;// 第一个节点不存数据，先删除数据节点
    while(iter->next) {
        tgg_fd_id_list* tmp = iter->next;
        iter->next = iter->next->next;
        memset(tmp, 0, sizeof(tgg_fd_id_list));
    	high_freq_free(g_mempool_clifdlist_data, tmp, sizeof(tgg_fd_id_list));
    }
    // 删除第一个节点
    memset(fdiddata, 0, sizeof(tgg_fd_id_list));
    high_freq_free(g_mempool_clifdlist_data, fdiddata, sizeof(tgg_fd_id_list));
}


tgg_write_data* format_send_data(const std::string& sdata, std::map<int, int>& mapfdidx, int fdopt)
{
	tgg_write_data* wdata = NULL;
	int ret = rte_mempool_get(g_mempool_write, (void**)&wdata);
    // TODO  建议增加循环处理，内存池不够，可以稍微等待消费端释放
	if (ret < 0) {
		LOG_ERROR("get mem from write pool failed,code:%d.", ret);
		return NULL;
	}
	tgg_fd_id_list* tail = NULL;
	tgg_fd_id_list* pcur = NULL;
	tgg_fd_id_list* head = NULL;
	std::map<int, int>::iterator it = mapfdidx.begin();
	while (it != mapfdidx.end()) {
		ret = high_freq_malloc(g_mempool_clifdlist_data, (void**)&pcur, sizeof(tgg_fd_id_list));
		if (ret < 0) {
            // TODO 如果只有一个失败了，其他的是不是可以继续发送，而不是全部都不发了
			LOG_ERROR("get mem from clifdlist pool failed,code:%d.", ret);
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
	if (sdata.size() > 0) {
		ret = high_freq_malloc(g_mempool_write_data, &wdata->data, sdata.size());
		// wdata->data = dpdk_rte_malloc(sdata.length());
		if (ret < 0) {
			LOG_ERROR("malloc mem from write data pool failed, ret:%d.", ret);
			goto add_data_failed;
		}
		memcpy((char*)(wdata->data), sdata.c_str(), sdata.size());
	} else {
		wdata->data = NULL;
	}
		wdata->data_len = sdata.size();
	wdata->fd_opt = fdopt;
	return wdata;

add_data_failed:
	LOG_ERROR("malloc mem failed.");
	clean_fdidlist(wdata->lst_fd);
	memset(wdata, 0, sizeof(tgg_write_data));
	rte_mempool_put(g_mempool_write, wdata);
	return NULL;
}

int enqueue_data_batch_fd(int core_id, const std::string& data, std::map<int, int>& mapfdidx, int fdopt)
{
	if(mapfdidx.size() <= 0) {
		// fd列表为空
		LOG_ERROR("mapfdidx is empty.");
		return 0;
	}
	tgg_write_data* wdata = format_send_data(data, mapfdidx, fdopt);
	if (!wdata) {
		LOG_ERROR("Format send data failed.");
		return -1;
	}
	int count = 10;
	while (tgg_enqueue_write(core_id, wdata) < 0 && count-- > 0 ) {
		usleep(10);
	}
	static int loop_times_sndcli = 0;
	// TODO 前期调试要看是否经常出现重试
	if (count < 9) {
		++loop_times_sndcli;
		if(loop_times_sndcli % 100 == 0) {
			LOG_ERROR("loop times:%d.", loop_times_sndcli);
		}
	}
	if (count <= 0) {
		LOG_ERROR("Enqueue write data failed.");
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

tgg_bw_data* format_send_server_data(int core_id, int fd, const std::string& sdata, int fdopt)
{
	tgg_bw_data* bwdata = NULL;
	int ret = rte_mempool_get(g_mempool_bwrcv, (void**)&bwdata);
        // TODO  建议增加循环处理，内存池不够，可以稍微等待消费端释放
	if (ret < 0) {
		LOG_ERROR("get mem from bwrcv pool failed,code:%d.", ret);
		return NULL;
	}
	if(sdata.size() > 0) {
		ret = high_freq_malloc(g_mempool_bwrcv_data, &bwdata->data, sdata.size());
		if (ret < 0) {
			rte_mempool_put(g_mempool_bwrcv, (void*)bwdata);
			LOG_ERROR("get mem from bwrcv data pool failed,code:%d.", ret);
			return NULL;
		}
		// bwdata->data = dpdk_rte_malloc(sdata.size());
		memcpy(bwdata->data, sdata.c_str(), sdata.size());
	} else {
		bwdata->data = NULL;
	}
	bwdata->data_len = sdata.size();
	bwdata->fd_opt = fdopt;
	bwdata->fd = fd;
	bwdata->coreid = core_id;
    bwdata->peer_ip = (unsigned int)tgg_get_cli_ip(core_id, fd);
    bwdata->peer_port = (unsigned int)tgg_get_cli_port(core_id, fd);
    bwdata->cid = (unsigned int)tgg_get_cli_cid(core_id, fd);
	return bwdata;
}

int enqueue_data_trans(int core_id, int fd, const std::string& data, int fdopt)
{
	tgg_bw_data* bwdata = format_send_server_data(core_id, fd, data, fdopt);
	if (!bwdata) {
		LOG_ERROR("Format bw server data failed.");
		return -1;
	}
	int maxtry = 10;// 入队列可能会失败最多尝试10次
	while (tgg_enqueue_trans(bwdata) < 0 && maxtry-- > 0 ) {
		usleep(10);
	}
	static int loop_times_sndserver = 0;
	// TODO 前期调试要看是否经常出现重试
	if (maxtry < 9) {
		++loop_times_sndserver;
		if(loop_times_sndserver % 100 == 0) {
			LOG_ERROR("loop times:%d.", loop_times_sndserver);
		}
	}
	if (maxtry <= 0) {
		LOG_ERROR("Enqueue bw server data failed.");
		return -1;
	}
	return 0;
}

int enqueue_data_send_server(int core_id, int fd, const std::string& data, int fdopt)
{
	tgg_bw_data* bwdata = format_send_server_data(core_id, fd, data, fdopt);
	if (!bwdata) {
		LOG_ERROR("Format bw server data failed.");
		return -1;
	}
	int maxtry = 10;// 入队列可能会失败最多尝试10次
	int queue_id = fd % TggConfigure::getInstance()->get_bwsvr_count();
	while (tgg_enqueue_bwsnd(queue_id, bwdata) < 0 && maxtry-- > 0 ) {
		usleep(10);
	}
	static int loop_times_sndserver = 0;
	// TODO 前期调试要看是否经常出现重试
	if (maxtry < 9) {
		++loop_times_sndserver;
		if(loop_times_sndserver % 100 == 0) {
			LOG_ERROR("loop times:%d.", loop_times_sndserver);
		}
	}
	if (maxtry <= 0) {
		RTE_LOG(ERR, USER1, "[%s][%d] Enqueue bw server data failed.", 
			__FILE__, __LINE__);
		return -1;
	}
	return 0;
}


#include <sys/prctl.h>
static void set_core_path(const char *core_path) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "echo '%score_%%e_%%p' > /proc/sys/kernel/core_pattern", core_path);
    system(cmd);  // 需 root 权限
}

void init_core(const char* core_path)
{
	// 设置 core 文件的路径
    prctl(PR_SET_DUMPABLE, 1);  // 确保程序可以生成 core 文件
    // char core_path[256];
    // snprintf(core_path, sizeof(core_path), "%s.core_%e_%p", dumpfile, getpid());
#ifdef PR_SET_COREDUMP_FILENAME
    prctl(PR_SET_COREDUMP_FILENAME, core_path, 0, 0, 0);
#else
    // #pragma message("警告：PR_SET_COREDUMP_FILENAME 不可用，使用备用方案")
    set_core_path(core_path);  // 调用上述备用方案
#endif
}

void* dpdk_rte_malloc(int size)
{
	void* pdata = rte_malloc("tgg_malloc", size, 0);
	if (!pdata)	{
		LOG_ERROR("malloc data failed.\n");
	}
	// TODO 这里需要把pdata管理起来，因dpdk的secondary进程出core而未释放时会导致大页内存泄漏
	// 		可以用链表管理起来，然后注册rte_service给master进程去管理，也可以放到定时任务管理
	return pdata;
}

int high_freq_malloc(struct rte_mempool* pool, void** data, int size)
{
	if(size <= 0) {
		LOG_INFO("invalid size[%d] to malloc.", size);
		return -1;
	}
	if(size > COMMON_PACKET_LEN) {
		LOG_INFO("recieved an large packet, size:%d", size);
		return rte_mempool_get(g_mempool_large_data, data);
	} else {
		return rte_mempool_get(pool, data);
	}
}

void high_freq_free(struct rte_mempool* pool, void* data, int size)
{
	if(size <= 0) {
		LOG_INFO("invalid size[%d] to free.", size);
		return ;
	}
	if(size > COMMON_PACKET_LEN) {
		LOG_INFO("free an large packet, size:%d", size);
		rte_mempool_put(g_mempool_large_data, data);
	} else {
		rte_mempool_put(pool, data);
	}
}


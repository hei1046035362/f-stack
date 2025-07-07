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
#include "mt_api.h"

extern int g_fd_limit;
extern struct rte_memzone* g_fd_zones[MAX_LCORE_COUNT];
extern struct rte_memzone* g_fd_bw_zones[MAX_LCORE_COUNT];
extern int g_bwfdx_limit;
extern struct rte_memzone* g_bwfdx_zones[MAX_LCORE_COUNT];
extern struct rte_memzone* g_bwprc_zone;
extern struct rte_ring* g_ring_writes[MAX_LCORE_COUNT];
extern struct rte_ring* g_ring_trans;
extern struct rte_ring* g_ring_bwfdx;
extern struct rte_ring* g_ring_bwrcvs[MAX_LCORE_COUNT];

extern struct rte_mempool* g_mempool_trans;
extern struct rte_mempool* g_mempool_write[MAX_LCORE_COUNT];
extern struct rte_mempool* g_mempool_bwrcv[MAX_LCORE_COUNT];
extern struct rte_mempool* g_mempool_trans_data;
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

int64_t generate_fdidcid(int core_id, int fd, int cid)
{
    int64_t fdidcid = ((fd << 8) | core_id);
    fdidcid <<= 32;
    fdidcid |= cid;
    return fdidcid;
}

int generate_cid(int core_id, int idx)
{
    return ((idx << 8) | core_id);
}

int generate_bwfdx(int prc_id, int fd)
{
    return ((fd << 8) | prc_id);
}

static int s_cur_cli_idx = 0;
int get_valid_idx(int core_id)
{
	int looptimes = 2;
	while (1) {
		// TODO  后续要考虑自增id超过uint32_max了怎么处理，
		++s_cur_cli_idx;
		if(s_cur_cli_idx >= g_fd_limit) {
			s_cur_cli_idx = 1;
			looptimes--;
		}
		if(looptimes <= 0) {
			//rte_exit(-1, "nonIdx ");
			LOG_ERROR("None idx available.");
			return -1;
		}
		if(tgg_check_idx_exist(core_id, s_cur_cli_idx) < 0) {
			break;
		}
	}
	LOG_INFO("valid idx %d.", s_cur_cli_idx);
	return s_cur_cli_idx;
}

void tgg_close_cli(int core_id, int fd)
{
	// SpinLock lock(get_cli_lock());
	tgg_cli_info* cli = &((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd];
	// cli->cid = 0;
	// memset(cli->uid, 0, sizeof(cli->uid));
	// memset(cli->reserved, 0, sizeof(cli->reserved));
	cli->idx = TGG_FD_CLOSED;
	cli->authorized = AUTH_TYPE_UNKNOWN;
}

int tgg_init_cli(int core_id, int fd, char* ip_str, uint32_t ip, ushort port)
{
	// SpinLock lock(get_cli_lock());
	tgg_cli_info* cli = &((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd];
	// cli->cid = 0;
	// memset(cli->uid, 0, sizeof(cli->uid));
	// memset(cli->reserved, 0, sizeof(cli->reserved));
	cli->idx = get_valid_idx(core_id);
	if(cli->idx < 0) {
		LOG_ERROR("init client failed, invalid idx:%d core id:%d fd:%d.", cli->idx, core_id, fd);
		return -1;
	}
	if (tgg_add_idx(core_id, cli->idx) < 0) {
		LOG_ERROR("init client failed, add idx:%d failed, core id:%d fd:%d.", cli->idx, core_id, fd);
		return -1;
	}
	cli->status = 0;
	cli->authorized = 0;
	memcpy(cli->ip_str, ip_str, INET_ADDRSTRLEN);
	cli->ip = ip;
	cli->port = port;
	return 0;
}

void tgg_close_cli_bw(int core_id, int fd)
{
	// SpinLock lock(get_cli_lock());
	tgg_cli_bw_info* cli = &((tgg_cli_bw_info*)g_fd_bw_zones[core_id]->addr)[fd];
	cli->cid = -1;// 服务端侧已完成关闭
	memset(cli->uid, 0, sizeof(cli->uid));
	memset(cli->reserved, 0, sizeof(cli->reserved));
}

int tgg_init_cli_bw(int core_id, int fd, int cid)
{
	// SpinLock lock(get_cli_lock());
	tgg_cli_bw_info* cli = &((tgg_cli_bw_info*)g_fd_bw_zones[core_id]->addr)[fd];
	cli->cid = cid;// 服务端侧初始化
	memset(cli->uid, 0, sizeof(cli->uid));
	memset(cli->reserved, 0, sizeof(cli->reserved));
	return 0;
}

int tgg_get_cli_idx(int core_id, int fd)
{
	// SpinLock lock(get_cli_lock());
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].idx;	
}

int tgg_get_cli_status(int core_id, int fd)
{
	// SpinLock lock(get_cli_lock());
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].status;	
}

int tgg_get_cli_authorized(int core_id, int fd)
{
	// SpinLock lock(get_cli_lock());
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].authorized;	
}
std::string tgg_get_cli_ip_str(int core_id, int fd)
{
	// SpinLock lock(get_cli_lock());
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].ip_str;
}

uint32_t tgg_get_cli_ip(int core_id, int fd)
{
	// SpinLock lock(get_cli_lock());
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].ip;
}
ushort tgg_get_cli_port(int core_id, int fd)
{
	// SpinLock lock(get_cli_lock());
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].port;	
}

int tgg_get_cli_bwfdx(int core_id, int fd)
{
	// SpinLock lock(get_cli_lock());
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].bwfdx;	
}

std::string tgg_get_cli_uid(int core_id, int fd)
{
	// SpinLock lock(get_cli_lock());
	return ((tgg_cli_bw_info*)g_fd_bw_zones[core_id]->addr)[fd].uid;	
}

int tgg_get_cli_cid(int core_id, int fd)
{
	// SpinLock lock(get_cli_lock());
	return ((tgg_cli_bw_info*)g_fd_bw_zones[core_id]->addr)[fd].cid;	
}

std::string tgg_get_cli_reserved(int core_id, int fd)
{
	// SpinLock lock(get_cli_lock());
	return ((tgg_cli_bw_info*)g_fd_bw_zones[core_id]->addr)[fd].reserved;	
}

int tgg_set_cli_idx(int core_id, int fd, int idx)
{
	// SpinLock lock(get_cli_lock());
	((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].idx = idx;
	return 0;
}

int tgg_set_cli_status(int core_id, int fd, int status)
{
	// SpinLock lock(get_cli_lock());
	((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].status = status;	
	return 0;
}

int tgg_set_cli_authorized(int core_id, int fd, int authorized)
{
	// SpinLock lock(get_cli_lock());
	((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].authorized = authorized;
	return 0;
}
int tgg_set_cli_ip(int core_id, int fd, uint32_t ip)
{
	// SpinLock lock(get_cli_lock());
	((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].ip = ip;
	return 0;
}
int tgg_set_cli_port(int core_id, int fd, ushort port)
{
	// SpinLock lock(get_cli_lock());
	((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].port = port;
	return 0;
}

int tgg_set_cli_bwfdx(int core_id, int fd, int bwfdx)
{
	// SpinLock lock(get_cli_lock());
	((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].bwfdx = bwfdx;
	return 0;
}

int tgg_set_cli_uid(int core_id, int fd, const char* uid)
{
	// SpinLock lock(get_cli_lock());
	memset(((tgg_cli_bw_info*)g_fd_bw_zones[core_id]->addr)[fd].uid, 0, sizeof(tgg_cli_bw_info::uid));
	strncpy(((tgg_cli_bw_info*)g_fd_bw_zones[core_id]->addr)[fd].uid, uid, strlen(uid));
	return 0;
}

int tgg_set_cli_cid(int core_id, int fd, int cid)
{
	// SpinLock lock(get_cli_lock());
	((tgg_cli_bw_info*)g_fd_bw_zones[core_id]->addr)[fd].cid = cid;
	return 0;
}

int tgg_set_cli_reserved(int core_id, int fd, const char* reserved)
{
	// SpinLock lock(get_cli_lock());
	memset(((tgg_cli_bw_info*)g_fd_bw_zones[core_id]->addr)[fd].reserved, 0, sizeof(tgg_cli_bw_info::reserved));
	strncpy(((tgg_cli_bw_info*)g_fd_bw_zones[core_id]->addr)[fd].reserved, reserved, strlen(reserved));
	return 0;
}

void tgg_init_bwfdx_prc(int prc_id)
{
	for(int i = 0; i < g_bwfdx_limit; ++i) {
		if(tgg_get_bwfdx_status(prc_id, i)) {
			tgg_close_bw_session(prc_id, i);
		}
	}
	tgg_iter_del_bwfdx(prc_id);
}

int tgg_get_bwfdx_status(int prc_id, int fd)
{
	return ((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].status;
}

int tgg_get_bwfdx_load(int64_t fdid)
{
	return ((tgg_bw_info*)g_bwfdx_zones[GET_COREID_FDID_MASK(fdid)]->addr)[GET_FD_FDID_MASK(fdid)].load;
}

int tgg_get_bwfdx_cmd(int prc_id, int fd)
{
	return ((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].cmd;
}

int tgg_get_bwfdx_idx(int prc_id, int fd)
{
	return ((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].idx;
}
int tgg_get_bwfdx_authorized(int prc_id, int fd)
{
	return ((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].authorized;
}
int tgg_get_bwfdx_ip(int prc_id, int fd)
{
	return ((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].ip;
}
int tgg_get_bwfdx_port(int prc_id, int fd)
{
	return ((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].port;
}
std::string tgg_get_bwfdx_seckey(int prc_id, int fd)
{
	return ((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].secretkey;
}
std::string tgg_get_bwfdx_workerkey(int prc_id, int fd)
{
	return ((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].workerkey;
}

int tgg_set_bwfdx_status(int prc_id, int fd, int status)
{
	((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].status = status;
	return 0;
}

int tgg_set_bwfdx_load(int prc_id, int fd, int load)
{
	((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].load = load;
	return 0;
}

int tgg_add_bwfdx_load(int fdid)
{
	++(((tgg_bw_info*)g_bwfdx_zones[GET_COREID_FDID_MASK(fdid)]->addr)[GET_FD_FDID_MASK(fdid)].load);
	return 0;
}

int tgg_set_bwfdx_cmd(int prc_id, int fd, int cmd)
{
	((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].cmd = cmd;
	return 0;
}

int tgg_set_bwfdx_idx(int prc_id, int fd, int idx)
{
	((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].idx = idx;
	return 0;
}
int tgg_set_bwfdx_authorized(int prc_id, int fd, int authorized)
{
	((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].authorized = authorized;
	return 0;
}
int tgg_set_bwfdx_ip(int prc_id, int fd, uint32_t ip)
{
	((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].ip = ip;
	return 0;
}
int tgg_set_bwfdx_port(int prc_id, int fd, ushort port)
{
	((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].port = port;
	return 0;
}
int tgg_set_bwfdx_seckey(int prc_id, int fd, const char* secretkey)
{
	memset(((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].secretkey, 0, sizeof(tgg_bw_info::secretkey));
	strncpy(((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd].secretkey, secretkey, strlen(secretkey));
	return 0;
}

int tgg_set_bwfdx_workerkey(int prc_id, int fd, const char* workerkey)
{
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
	if(tgg_get_bwfdx_status(prc_id, fd) > 0) {
		LOG_WARNING("connection[prcid:%d fd:%d] prev info not cleaned, clean first.", prc_id, fd);
		tgg_close_bw_session(prc_id, fd);
	} else{
	 	tgg_clean_bwfdx(prc_id, fd);
	}
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
		tgg_bwfdx_data* bwfdxdata = (tgg_bwfdx_data*)dpdk_rte_malloc(sizeof(tgg_bwfdx_data));
		if(!bwfdxdata) {
			LOG_ERROR("Enqueue bwfdxdata failed.");
			return;
		}
		bwfdxdata->bwfdx = generate_bwfdx(prc_id, fd);
		bwfdxdata->cmd = BWFDX_CMD_ADD;
		tgg_enqueue_bwfdx(bwfdxdata);
	}
}

void tgg_close_bw_session(int prc_id, int fd)
{
	tgg_set_bwfdx_status(prc_id, fd, 0);

	int cmd = tgg_get_bwfdx_cmd(prc_id, fd);
	if(cmd == GatewayProtocal::CMD_WORKER_CONNECT) {
		// 通知透传线程不要再使用这个fd了
		tgg_bwfdx_data* bwfdxdata = (tgg_bwfdx_data*)dpdk_rte_malloc(sizeof(tgg_bwfdx_data));
		if(!bwfdxdata) {
			LOG_ERROR("Enqueue bwfdxdata failed, cannot malloc data.");
			return;
		}
		bwfdxdata->bwfdx = generate_bwfdx(prc_id, fd);
		bwfdxdata->cmd = BWFDX_CMD_DELETE;
		if(tgg_enqueue_bwfdx(bwfdxdata) < 0) {
            dpdk_rte_free(bwfdxdata);
			LOG_ERROR("Enqueue bwfdxdata failed.");
		}

		tgg_del_bwfdx(generate_bwfdx(prc_id, fd));
		std::string workerkey = tgg_get_bwfdx_workerkey(prc_id, fd);
		tgg_del_bwwkkey(workerkey.c_str());
	}
	LOG_INFO("close bw session prc:[%d] fd:[%d], left bw count:%d.", 
		prc_id, fd, tgg_get_bwfdx_count());
	tgg_clean_bwfdx(prc_id, fd);
}

int tgg_clean_bwfdx(int prc_id, int fd)
{
	memset(&(((tgg_bw_info*)g_bwfdx_zones[prc_id]->addr)[fd]), 0, sizeof(tgg_bw_info));
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

void release_ws_buffer(int core_id, int fd)
{
    tgg_ws_data* wsdata = &((&((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd])->ws_data);
    if (!wsdata->data) {// 没有数据
        return;
    }
    // 释放内存
    dpdk_rte_free(wsdata->data);
    memset(wsdata, 0, sizeof(tgg_ws_data));
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

tgg_bw_data* get_bwdata_from_transdata(int prc_id, tgg_trans_data* tdata)
{
	tgg_bw_data* bdata = NULL;
    int ret = high_freq_malloc(g_mempool_bwrcv[prc_id], (void**)&bdata, sizeof(tgg_trans_data));
    if(ret < 0) {
		LOG_ERROR("malloc bw data from trans failed, fd:%d.", tdata->fd);
    	return NULL;
    }
    memcpy(bdata, tdata, sizeof(tgg_trans_data));
    if (tdata->data) {
        ret = high_freq_malloc(g_mempool_bwrcv_data, (void**)&bdata->data, tdata->data_len);
        if(ret < 0) {
			LOG_ERROR("malloc bw data content from trans failed, fd:%d.", tdata->fd);
        	high_freq_free(g_mempool_bwrcv[prc_id], bdata, sizeof(tgg_trans_data));
        	return NULL;
        }
        memcpy(bdata->data, tdata->data, tdata->data_len);
    }
    return bdata;
}
int tgg_enqueue_bwsnd(int queue_id, tgg_bw_data* data)
{
	if(data->fd <= 0) {
		LOG_ERROR("invalid data fd:%d.", data->fd);
	}
	int ret = rte_ring_enqueue(g_ring_bwrcvs[queue_id], data);
	if(ret < 0) {
		LOG_ERROR("enqueue bwsnd ring failed, count:%d", rte_ring_count(g_ring_bwrcvs[queue_id]));
	}
	return ret;
}

int tgg_dequeue_bwsnd(int queue_id, tgg_bw_data** data)
{
	if (rte_ring_empty(g_ring_bwrcvs[queue_id])) {
		return -ENOENT;
	}
	return rte_ring_dequeue(g_ring_bwrcvs[queue_id], (void**)data);
}

int tgg_enqueue_trans(tgg_trans_data* data)
{
	if(data->fd <= 0) {
		LOG_ERROR("invalid data fd:%d.", data->fd);
	}
	return rte_ring_enqueue(g_ring_trans, data);
}

int tgg_dequeue_trans(tgg_trans_data** data)
{
	if (rte_ring_empty(g_ring_trans)) {
		return -ENOENT;
	}
	return rte_ring_dequeue(g_ring_trans, (void**)data);
}

int tgg_enqueue_bwfdx(tgg_bwfdx_data* data)
{
	return rte_ring_enqueue(g_ring_bwfdx, data);
}

int tgg_dequeue_bwfdx(tgg_bwfdx_data** data)
{
	if (rte_ring_empty(g_ring_bwfdx)) {
		return -ENOENT;
	}
	return rte_ring_dequeue(g_ring_bwfdx, (void**)data);
}

void clean_trans_data(tgg_trans_data* bdata)
{
    if (bdata->data) {
    	memset(bdata->data, 0, bdata->data_len);
        high_freq_free(g_mempool_trans_data, bdata->data, bdata->data_len);
        bdata->data = NULL;
    }
    memset(bdata, 0, sizeof(tgg_trans_data));
    high_freq_free(g_mempool_trans, bdata, sizeof(tgg_trans_data));
}

void clean_bw_data(int prc_id, tgg_bw_data* bdata)
{
    if (bdata->data) {
    	memset(bdata->data, 0, bdata->data_len);
        high_freq_free(g_mempool_bwrcv_data, bdata->data, bdata->data_len);
        bdata->data = NULL;
    }
    memset(bdata, 0, sizeof(tgg_bw_data));
    high_freq_free(g_mempool_bwrcv[prc_id], bdata, sizeof(tgg_bw_data));
}

void clean_write_data(int core_id, tgg_write_data* wdata)
{
	clean_fdidlist(wdata->lst_fd);
    if (wdata->data) {
    	memset(wdata->data, 0, wdata->data_len);
        high_freq_free(g_mempool_write_data, wdata->data, wdata->data_len);
        wdata->data = NULL;
    }
    memset(wdata, 0, sizeof(tgg_write_data));
    high_freq_free(g_mempool_write[core_id], wdata, sizeof(tgg_write_data));
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


tgg_write_data* format_send_data(int core_id, const std::string& sdata, std::map<int, int>& mapfdidx, int fdopt)
{
	tgg_write_data* wdata = NULL;
	int ret = high_freq_malloc(g_mempool_write[core_id], (void**)&wdata, sizeof(tgg_write_data));
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
	high_freq_free(g_mempool_write[core_id], wdata, sizeof(tgg_write_data));
	return NULL;
}

int enqueue_data_batch_fd(int core_id, const std::string& data, std::map<int, int>& mapfdidx, int fdopt)
{
	if(mapfdidx.size() <= 0) {
		// fd列表为空
		LOG_ERROR("mapfdidx is empty.");
		return 0;
	}
	tgg_write_data* wdata = format_send_data(core_id, data, mapfdidx, fdopt);
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


#include <sys/prctl.h>
static void set_core_path(const char *core_path) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "echo '%s%%e_%%p.core' > /proc/sys/kernel/core_pattern", core_path);
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
static int s_malloc_count;
void* dpdk_rte_malloc(int size)
{
	void* pdata = rte_malloc("tgg_malloc", size, 0);
	if (!pdata)	{
		LOG_ERROR("malloc data failed.\n");
		return NULL;
	}
	// TODO 这里需要把pdata管理起来，因dpdk的secondary进程出core而未释放时会导致大页内存泄漏
	// 		可以用链表管理起来，然后注册rte_service给master进程去管理，也可以放到定时任务管理
	s_malloc_count++;
	return pdata;
}

static int s_free_count;

void dpdk_rte_free(void* pdata)
{
	rte_free(pdata);
	s_free_count++;
	// TODO 这里需要把pdata管理起来，因dpdk的secondary进程出core而未释放时会导致大页内存泄漏
	// 		可以用链表管理起来，然后注册rte_service给master进程去管理，也可以放到定时任务管理
}

static std::map<std::string, int> s_hi_freq_malloc;
int high_freq_malloc(struct rte_mempool* pool, void** data, int size)
{
	if(size <= 0) {
		LOG_INFO("invalid size[%d] to malloc.", size);
		return -1;
	}
	s_hi_freq_malloc[pool->name]++;
	if(size > COMMON_PACKET_LEN) {
		LOG_INFO("recieved an large packet, size:%d", size);
		return rte_mempool_get(g_mempool_large_data, data);
	} else {
		return rte_mempool_get(pool, data);
	}
}

static std::map<std::string, int> s_hi_freq_free;
void high_freq_free(struct rte_mempool* pool, void* data, int size)
{
	if(size <= 0) {
		LOG_INFO("invalid size[%d] to free.", size);
		return ;
	}
	s_hi_freq_free[pool->name]++;
	if(size > COMMON_PACKET_LEN) {
		LOG_INFO("free an large packet, size:%d", size);
		rte_mempool_put(g_mempool_large_data, data);
	} else {
		rte_mempool_put(pool, data);
	}
}

void print_mem_statistics()
{
	LOG_WARNING("malloc times: %d", s_malloc_count);
	LOG_WARNING("free times: %d", s_free_count);
	for(auto iter : s_hi_freq_malloc) {
		LOG_WARNING("pool[%s] hi_malloc times: %d", iter.first.c_str(), iter.second);	
	}
	for(auto iter : s_hi_freq_free) {
		LOG_WARNING("pool[%s] hi_free times: %d", iter.first.c_str(), iter.second);	
	}
}

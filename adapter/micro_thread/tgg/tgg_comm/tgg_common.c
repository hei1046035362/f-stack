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
#include "comm/common.hpp"

#include <sys/wait.h>
#include <sys/prctl.h>
#include <limits.h>
#include <sys/stat.h>

extern int g_fd_limit;
extern struct rte_memzone* g_fd_zones[MAX_LCORE_COUNT];
extern struct rte_memzone* g_fd_bw_zones[MAX_LCORE_COUNT];
extern int g_bwfdx_limit;
extern struct rte_memzone* g_bwfdx_zones[MAX_LCORE_COUNT];
extern struct rte_memzone* g_bwprc_zone;
extern struct rte_memzone* g_gw_monitor_zone;

extern struct rte_ring* g_ring_writes[MAX_LCORE_COUNT];
extern struct rte_ring* g_ring_trans;
extern struct rte_ring* g_ring_bwfdx;
extern struct rte_ring* g_ring_bwrcvs[MAX_LCORE_COUNT];

extern struct rte_ring* g_ring_master;

extern struct rte_mempool* g_mempool_trans;
extern struct rte_mempool* g_mempool_write[MAX_LCORE_COUNT];
extern struct rte_mempool* g_mempool_bwrcv[MAX_LCORE_COUNT];
extern struct rte_mempool* g_mempool_trans_data;
extern struct rte_mempool* g_mempool_write_data;
extern struct rte_mempool* g_mempool_bwrcv_data;
extern struct rte_mempool* g_mempool_large_data;
extern struct rte_mempool* g_mempool_clifdlist_data;
extern struct rte_mempool* g_mempool_ws_buffer;
extern struct rte_mempool* g_mempool_fd_snddata[MAX_LCORE_COUNT];

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
	cli->thread = NULL;
	tgg_clean_cli_snd_data(core_id, fd);
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
	cli->send_datalist = NULL;
	cli->wdata = NULL;
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

tgg_send_data* tgg_get_cli_snd_data(int core_id, int fd)
{
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].send_datalist;
}
// static uint64_t push_times = 0;
// static uint64_t pop_times = 0;
int tgg_add_cli_snd_data(int core_id, int fd, tgg_write_data* wdata)
{
	tgg_send_data* data = NULL;
	if (high_freq_malloc(g_mempool_fd_snddata[core_id], (void**)(&data), sizeof(tgg_send_data)) < 0) {
		return -1;
	}
	if(wdata->data) {
		wdata->ref++;
	}
	data->data = wdata;
	data->next = NULL;
	tgg_send_data* snddata = ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].send_datalist;
	if(!snddata) {
		((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].send_datalist = data;
		data->tail = data;
		// push_times++;
		// LOG_INFO("addsnd:%lu", push_times);
		return 0;
	}
	snddata->tail->next = data;
	snddata->tail = data;
	// push_times++;
	// LOG_INFO("addsnd:%lu", push_times);
	return 0;
}

void tgg_clean_cli_snd_data(int core_id, int fd)
{
	if(((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].wdata) {
		tgg_send_data* snddata = (tgg_send_data*)(((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].wdata);
        ((tgg_write_data*)(snddata->data))->ref--;
        if(((tgg_write_data*)(snddata->data))->ref <= 0) {
            clean_write_data(core_id, (tgg_write_data*)(snddata->data));            
        }
		high_freq_free(g_mempool_fd_snddata[core_id], snddata, sizeof(tgg_send_data));
		((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].wdata = NULL;
	}

	tgg_send_data* snddata = ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].send_datalist;
	while(snddata) {
		tgg_send_data* tmp = snddata;
        ((tgg_write_data*)(tmp->data))->ref--;
        if(((tgg_write_data*)(tmp->data))->ref <= 0) {
            clean_write_data(core_id, (tgg_write_data*)(tmp->data));            
        }
		snddata = snddata->next;
		high_freq_free(g_mempool_fd_snddata[core_id], tmp, sizeof(tgg_send_data));
	}
	((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].send_datalist = NULL;
}
void tgg_set_write_data(int core_id, int fd, void* data)
{
	((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].wdata = data;
}
tgg_send_data* tgg_pop_cli_snd_data(int core_id, int fd)
{
	if(((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].wdata)
		return (tgg_send_data*)(((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].wdata);
	tgg_send_data* snddata = ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].send_datalist;
	if(snddata && snddata->next) {
		((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].send_datalist = snddata->next;
		snddata->next->tail = snddata->tail;
		snddata->next = NULL;
		snddata->tail = NULL;
		// pop_times++;
		// LOG_INFO("popsnd:%lu", pop_times);
	} else {
		((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].send_datalist = NULL;
	}
	return snddata;
}

void tgg_set_cli_thread(int core_id, int fd, void* pthread)
{
	((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].thread = pthread;
}
void* tgg_get_cli_thread(int core_id, int fd)
{
	return ((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd].thread;
}

void tgg_free_cli_snd_data(int core_id, tgg_send_data* data)
{
	data->data = NULL;
	data->next = NULL;
	data->tail = NULL;
	high_freq_free(g_mempool_fd_snddata[core_id], data, sizeof(tgg_send_data));
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

const char* tgg_get_cli_reserved(int core_id, int fd)
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
			LOG_ERROR("malloc bwfdxdata failed.");
			return;
		}
		bwfdxdata->bwfdx = generate_bwfdx(prc_id, fd);
		bwfdxdata->cmd = BWFDX_CMD_ADD;
		if(tgg_enqueue_bwfdx(bwfdxdata)) {
			LOG_ERROR("Enqueue bwfdxdata failed.");
			dpdk_rte_free(bwfdxdata);
		}
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
			LOG_ERROR("malloc bwfdxdata failed, cannot malloc data.");
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
		LOG_INFO("close WorkerConnect session prc:[%d] fd:[%d], left bw count:%d.", 
			prc_id, fd, tgg_get_bwfdx_count());
	} else {
		LOG_INFO("close GatewayClientConnect session prc:[%d] fd:[%d], left bw count:%d.", 
			prc_id, fd, tgg_get_bwfdx_count());
	}
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
		if (prc->heart_beat == 0 || prc->heart_beat + BW_PRC_HEART_BEAT_CHECK*2 < now) {
			if(prc->pid > 0 && kill(prc->pid, 0) == 0) {
				continue;// 进程依然存在
			}
			// prc->heart_beat = now;
			prc->pid = getpid();
			// prc->idx = 0;
			return i;
		}
	}
	return -1;
}

int tgg_get_bwprc_id(int bwcount)
{
	pid_t pid = getpid();
	for (int i = 0; i < bwcount; i++) {
		SpinLock lock(get_bwprc_lock());
		pid_data* prc = (pid_data*)(g_bwprc_zone->addr) + i;
		if (prc->heart_beat == 0 && pid == prc->pid) {
			return i;
		}
	}
	return -1;
}

// 获取有效的进程序号
int tgg_setup_bwprc_monitor(int prc_id, pid_t pid)
{
	SpinLock lock(get_bwprc_lock());
	pid_data* prc = (pid_data*)(g_bwprc_zone->addr) + prc_id;
	prc->pid = pid;
	prc->heart_beat = 0;// 心跳由被监控进程填入，为0只代表启动者以启动进程
	return 0;
}

// 更新心跳
void tgg_update_bwprc(int prc_id, uint64_t now)
{
	SpinLock lock(get_bwprc_lock());
	pid_data* prc = (pid_data*)g_bwprc_zone->addr + prc_id;
	prc->heart_beat = now;
	// if(!prc->idx) {
	// 	prc->idx = 1;
	// }
}

// 获取指定下标的进程id
pid_t tgg_get_bwprc_pid(int prc_id)
{
	SpinLock lock(get_bwprc_lock());
	pid_data* prc = (pid_data*)g_bwprc_zone->addr + prc_id;
	return prc->pid;
}

// 获取指定下标的进程id
int tgg_check_bwprc_up(int prc_id)
{
	SpinLock lock(get_bwprc_lock());
	pid_data* prc = (pid_data*)g_bwprc_zone->addr + prc_id;
	if(prc->pid > 0 && prc->heart_beat > 0) {
		return 1;
	}
	return 0;
}

// 检查指定进程是否超时
int tgg_checkif_bwprc_timeout(int prc_id, uint64_t now)
{
	SpinLock lock(get_bwprc_lock());
	pid_data* prc = (pid_data*)(g_bwprc_zone->addr) + prc_id;
	// 如果超过两倍心跳的时间都没有更新，就视为前一个进程已退出
	if (now - prc->heart_beat > BW_PRC_HEART_BEAT_CHECK) {
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

// 获取有效的进程序号
int tgg_setup_gw_monitor(int prc_id, pid_t pid)
{
	// uint64_t now = get_system_ms();
	WriteLock lock(get_gw_monitor_lock());
	pid_data* prc = (pid_data*)(g_gw_monitor_zone->addr) + prc_id;
	prc->pid = pid;
	prc->heart_beat = 0;
	// 如果超过两倍心跳的时间都没有更新，就视为前一个进程已退出
	// if (prc->heart_beat == 0 || prc->heart_beat + 2*GW_MONITOR_HEART_BEAT < now) {
	// 	if(prc->pid > 0 && kill(prc->pid, 0) == 0) {
	// 		LOG_ERROR("prev process still alive.");
	// 		return -1;// 进程依然存在
	// 	}
	// 	// prc->heart_beat = now;
	// 	prc->pid = getpid();
	// 	return 0;
	// }
	return 0;
}

// 更新心跳
void tgg_update_gw_monitor(int prc_id, uint64_t now)
{
	WriteLock lock(get_gw_monitor_lock());
	pid_data* prc = (pid_data*)g_gw_monitor_zone->addr + prc_id;
	if(prc->heart_beat != 0 && now - prc->heart_beat > GW_MONITOR_HEART_BEAT_CHECK) {// 调试代码，更新时间超过心跳时打印日志
		LOG_WARNING("update heart_beat time delayed, PID:%d, prc_id:%d heart_beat:%ld now:%ld diff:%d, curr_diff:%ld",
		 prc->pid, prc_id, prc->heart_beat, now, now - prc->heart_beat, get_system_ms() - now);
	}
	prc->heart_beat = now;
}

// 获取指定下标的进程id
pid_t tgg_get_gw_monitor_pid(int prc_id)
{
	ReadLock lock(get_gw_monitor_lock());
	pid_data* prc = (pid_data*)g_gw_monitor_zone->addr + prc_id;
	return prc->pid;
}

int tgg_check_gw_monitor_up(int prc_id)
{
	ReadLock lock(get_gw_monitor_lock());
	pid_data* prc = (pid_data*)g_gw_monitor_zone->addr + prc_id;
	if(prc->pid > 0 && prc->heart_beat > 0) {
		return 1;
	}
	return 0;
}


// 检查指定进程是否超时
int tgg_checkif_gw_monitor_timeout(int prc_id, uint64_t now)
{
	ReadLock lock(get_gw_monitor_lock());
	pid_data* prc = (pid_data*)(g_gw_monitor_zone->addr) + prc_id;
	// 如果超过两倍心跳的时间都没有更新，就视为前一个进程已退出
	// LOG_INFO("PID:%d, prc_id:%d heart_beat:%ld now:%ld", prc->pid, prc_id, prc->heart_beat, now);
	if (now > prc->heart_beat && now - prc->heart_beat > GW_MONITOR_HEART_BEAT_CHECK) {
		LOG_WARNING("PID:%d, prc_id:%d heart_beat:%ld now:%ld interval:%ld", prc->pid, prc_id, prc->heart_beat, now, now - prc->heart_beat);
		return 1;// 超时
	}
	return 0;// 没超时
}

// 进程退出前主动清理，下一个进程就能快速启动
void tgg_clean_gw_monitor(int prc_id)
{
	WriteLock lock(get_gw_monitor_lock());
	pid_data* prc = (pid_data*)g_gw_monitor_zone->addr + prc_id;
	memset(prc, 0, sizeof(pid_data));
}

int ringbuf_read(int core_id, int fd, char* dest, int len)
{
	// 同一个连接的数据都是串行的，同一个连接的ws的缓存只有cliprc进程处理，不需要加锁
    tgg_ws_data* wsdata = &((&((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd])->ws_data);
    if (!wsdata->data) {// 第一次缓存
        return 0;
    }
    int data_size = (wsdata->write_pos >= wsdata->read_pos) ? 
                     (wsdata->write_pos - wsdata->read_pos) : 
                     (BUFFER_PACKET_LEN - wsdata->read_pos + wsdata->write_pos);
	if (len > data_size) {
        LOG_WARNING("Requested len=%d exceeds available data=%d for core_id=%d, fd=%d",
                    len, data_size, core_id, fd);
        len = data_size;
    }

	// dest.reserve(dest.size() + len);

    // 分两段读取
    int first_chunk = (wsdata->read_pos + len > BUFFER_PACKET_LEN) ? 
                       (BUFFER_PACKET_LEN - wsdata->read_pos) : len;
    memcpy(dest, (char*)wsdata->data + wsdata->read_pos, first_chunk);
    
    if (len > first_chunk) {// 越过环形队列的尾部，从队列头部开始继续取数据
        memcpy(dest + first_chunk, wsdata->data, len - first_chunk);
    }
    // if(move_pos) {
    //     LOG_DEBUG("prev read_pos:%d, len:%d, write_pos:%d", wsdata->read_pos, len, wsdata->write_pos);
    // 	wsdata->read_pos = (wsdata->read_pos + len) & BUFFER_PACKET_MASK;
    // 	if (wsdata->read_pos == wsdata->write_pos) {
    // 		release_ws_buffer(core_id, fd);
    // 		LOG_DEBUG("release ws buffer for fd: %d coreid: %d after full consumption, read_pos:%d, len:%d, write_pos:%d",
    //          fd, core_id, wsdata->read_pos, len, wsdata->write_pos);
    //     }
    // }
    return len;
}

void ringbuf_move_read_pos(int core_id, int fd, int len)
{
    if(len <= 0 ) {
        return;
    }
    tgg_ws_data* wsdata = &((&((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd])->ws_data);
    if (!wsdata->data) {// 第一次缓存
        return;
    }
    // LOG_WARNING("prev read_pos:%d, write_pos:%d.", wsdata->read_pos, wsdata->write_pos);
    wsdata->read_pos = (wsdata->read_pos + len) & BUFFER_PACKET_MASK;
    if (wsdata->read_pos == wsdata->write_pos) {
        release_ws_buffer(core_id, fd);
        // LOG_DEBUG("release ws buffer for fd: %d coreid: %d after full consumption, read_pos:%d, len:%d, write_pos:%d",
        //  fd, core_id, wsdata->read_pos, len, wsdata->write_pos);
    }
    // LOG_WARNING("read_pos:%d, write_pos:%d.", wsdata->read_pos, wsdata->write_pos);
}

int64_t s_buffer_count = 0;
int ringbuf_write(int core_id, int fd, const char* data, int len)
{
	tgg_ws_data* wsdata = &((&((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd])->ws_data);
    if (!wsdata->data) {// 第一次缓存
    	if (rte_mempool_get(g_mempool_ws_buffer, &wsdata->data) < 0) {
    		LOG_ERROR("malloc memery failed.");
    		return -1;
    	}
    	s_buffer_count++;
    	wsdata->read_pos = 0;
    	wsdata->write_pos = 0;
    }
    int free_space = BUFFER_PACKET_LEN - ((wsdata->write_pos >= wsdata->read_pos) ? 
                      (wsdata->write_pos - wsdata->read_pos) : 
                      (BUFFER_PACKET_LEN - wsdata->read_pos + wsdata->write_pos));
	if (free_space == 0) {
        LOG_WARNING("Buffer full for core_id=%d, fd=%d", core_id, fd);
        return 0;
    }

    if (len > free_space) {
        LOG_WARNING("Requested write len=%d exceeds free space=%d for core_id=%d, fd=%d",
                    len, free_space, core_id, fd);
        len = free_space;
    }

    // 分两段写入
	int first_chunk = (wsdata->write_pos + len > BUFFER_PACKET_LEN)
        ? (BUFFER_PACKET_LEN - wsdata->write_pos)
        : len;
    
    memcpy((char*)wsdata->data + wsdata->write_pos, data, first_chunk);
    
    if (len > first_chunk) {
        memcpy(wsdata->data, data + first_chunk, len - first_chunk);
    }
    
    wsdata->write_pos = (wsdata->write_pos + len) & BUFFER_PACKET_MASK;
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
    return BUFFER_PACKET_LEN - wsdata->read_pos + wsdata->write_pos;
}

int ringbuf_space(int core_id, int fd)
{
	tgg_ws_data* wsdata = &((&((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd])->ws_data);
    if (!wsdata->data) {// 没有缓存数据
    	LOG_DEBUG("get ws data failed.");
	    return -1;
    }
    return BUFFER_PACKET_LEN - ringbuf_size(core_id, fd) - 1;
}
    
int get_one_frame_buffer(int core_id, int fd, void* data, int len, char* buffer)
{
	// SpinLock lock(get_cli_lock());
	// char buffer[4096];
    int cached_len = ringbuf_size(core_id, fd);// 获取之前缓存数据的大小
    if (cached_len <= 0) {// 上一次缓存没有遗留数据
    	memcpy(buffer, (char*)data, len);
    	return len;
    }
    if(cached_len + len > BUFFER_PACKET_LEN) {// 数据超出了最大允许缓存包的大小
        LOG_ERROR("buffer len[%d] exceed packet len:%d.", cached_len+len, BUFFER_PACKET_LEN);
        return -1;
    }
    int read_len = ringbuf_read(core_id, fd, buffer, cached_len);
    if(read_len != cached_len) {
        LOG_ERROR("read_len:%d < cached_len:%d.", read_len, cached_len);
        return -1;
    }
    // 把当前数据附加进去
	memcpy(buffer + read_len, data, len);
    // LOG_INFO("get cached buffer len:%d read_len:%d, buffer:%s.", len, read_len, bin2hex(std::string_view(buffer, read_len+len)).data());
    return read_len + len;
}

int get_whole_buffer(int core_id, int fd, char* buffer)
{
	// SpinLock lock(get_cli_lock());
	// std::string buffer;
    int cached_len = ringbuf_size(core_id, fd);
    if (cached_len <= 0) {// 上一次缓存没有遗留数据
    	return 0;
    }
    // 取上一次剩余数据
    return ringbuf_read(core_id, fd, buffer, cached_len);;
}

void release_ws_buffer(int core_id, int fd)
{
    tgg_ws_data* wsdata = &((&((tgg_cli_info*)g_fd_zones[core_id]->addr)[fd])->ws_data);
    if (!wsdata->data) {// 没有数据
    	LOG_DEBUG("No buffer to release for core_id=%d, fd=%d", core_id, fd);
        return;
    }
    // 释放内存
    rte_mempool_put(g_mempool_ws_buffer, wsdata->data);
    s_buffer_count--;
	wsdata->data = nullptr; // 防止悬垂指针
    wsdata->read_pos = 0;
    wsdata->write_pos = 0;
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

int tgg_enqueue_master(tgg_send_master_data* data)
{
	return rte_ring_enqueue(g_ring_master, data);
}

int tgg_dequeue_master(tgg_send_master_data** data)
{
	if (rte_ring_empty(g_ring_master)) {
		return -ENOENT;
	}
	return rte_ring_dequeue(g_ring_master, (void**)data);
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
    if(wdata->ref < 0) {
    	return;
    }
	clean_fdidlist(wdata->lst_fd);
    if (wdata->data) {
    	memset(wdata->data, 0, wdata->data_len);
        high_freq_free(g_mempool_write_data, wdata->data, wdata->data_len);
        wdata->data = NULL;
    }
    memset(wdata, 0, sizeof(tgg_write_data));
    high_freq_free(g_mempool_write[core_id], wdata, sizeof(tgg_write_data));
    wdata->ref = -1;
}

void clean_fdidnode(tgg_fd_id_list* fdiddata)
{
    if (!fdiddata) {
        return;
    }
    memset(fdiddata, 0, sizeof(tgg_fd_id_list));
    high_freq_free(g_mempool_clifdlist_data, fdiddata, sizeof(tgg_fd_id_list));	
}

void clean_fdidlist(tgg_fd_id_list* fdiddata)
{
    tgg_fd_id_list* iter = fdiddata;// 第一个节点不存数据，先删除数据节点
    while(iter) {
        tgg_fd_id_list* tmp = iter;
        iter = iter->next;
        memset(tmp, 0, sizeof(tgg_fd_id_list));
    	high_freq_free(g_mempool_clifdlist_data, tmp, sizeof(tgg_fd_id_list));
    }
}


tgg_write_data* format_send_data(int core_id, const std::shared_ptr<const std::string>& sdata, std::vector<int64_t>& vecfdidx, int fdopt)
{
	int try_times = ENQUEUE_TRY_TIMES;
    unsigned int attempt_size = vecfdidx.size();  // Change to *1 for less conservatism; revert if needed for concurrency buffer
    while(rte_mempool_avail_count(g_mempool_clifdlist_data) < attempt_size && try_times-- > 0) {
        usleep(10);
    }
    if(try_times <= 0) {
        LOG_ERROR("no enough[%u] avail unit in mempool, tried times:%d.", attempt_size, ENQUEUE_TRY_TIMES - try_times);
        return NULL;
    }
	tgg_write_data* wdata = NULL;
	int ret = 0;
	tgg_fd_id_list* tail = NULL;
	tgg_fd_id_list* pcur = NULL;
	tgg_fd_id_list* head = NULL;
	for (auto fdidx : vecfdidx) {
		// ret = high_freq_malloc(g_mempool_clifdlist_data, (void**)&pcur, sizeof(tgg_fd_id_list));
		try_times = ENQUEUE_TRY_TIMES;
		while ((ret = high_freq_malloc(g_mempool_clifdlist_data, (void**)&pcur, sizeof(tgg_fd_id_list))) < 0 && try_times-- > 0) {
			usleep(10);
		}
		if (ret < 0) {
			LOG_ERROR("malloc fdiddata node failed, ret:%d.", ret);
			goto add_data_failed;
		}
		pcur->fdid = GET_FD_FDCID_MASK(fdidx);
		pcur->idx = GET_IDX_FDCID_MASK(fdidx);
		if (!tail) {
			tail = pcur;
			head = tail;
		}
		else {
			tail->next = pcur;
			tail = tail->next;
		}
	}
	try_times = ENQUEUE_TRY_TIMES;
    while ((ret = high_freq_malloc(g_mempool_write[core_id], (void**)&wdata, sizeof(tgg_write_data))) < 0 && try_times-- > 0) {
        usleep(10);
    }
    if (ret < 0) {
        LOG_ERROR("get mem from write pool failed,code:%d.", ret);
        goto add_data_failed;
    }
	wdata->ref = 0;
	if (sdata->size() > 0) {
		try_times = ENQUEUE_TRY_TIMES;
		while ((ret = high_freq_malloc(g_mempool_write_data, &wdata->data, sdata->size())) < 0 && try_times-- > 0) {
			usleep(10);
		}
		// wdata->data = dpdk_rte_malloc(sdata.length());
		if (ret < 0) {
			LOG_ERROR("malloc mem from write data pool failed, ret:%d.", ret);
			goto add_data_failed;
		}
		memcpy((char*)(wdata->data), sdata->data(), sdata->size());
	} else {
		wdata->data = NULL;
	}
	wdata->lst_fd = head;
	wdata->data_len = sdata->size();
	wdata->fd_opt = fdopt;
	return wdata;

add_data_failed:
	LOG_ERROR("format write data failed.");
	if(wdata) {
        wdata->lst_fd = head;
        clean_write_data(core_id, wdata);
    } else {
        clean_fdidlist(head);
        head = NULL;
    }
	return NULL;
}

int enqueue_data_batch_fd(int core_id, const std::shared_ptr<const std::string>& data, std::vector<int64_t> vecfdidx, int fdopt)
{
	if(vecfdidx.size() <= 0) {
		// fd列表为空
		LOG_ERROR("vecfdidx is empty.");
		return 0;
	}
	tgg_write_data* wdata = format_send_data(core_id, data, vecfdidx, fdopt);
	if (!wdata) {
		LOG_ERROR("Format send data failed.");
		return -1;
	}
	int count = ENQUEUE_TRY_TIMES;
	int ret = 0;
	while ((ret = tgg_enqueue_write(core_id, wdata)) < 0 && count-- > 0 ) {
		usleep(10);
	}
	if (ret < 0) {
		clean_write_data(core_id, wdata);
		LOG_ERROR("Enqueue write data failed, ret:%d.", ret);
		return -1;
	}
	return 0;

}

int enqueue_data_single_fd(int core_id, const std::shared_ptr<const std::string>& data, int fd, int idx, int fdopt)
{
	std::vector<int64_t> vecfdidx;
	int64_t fdidcid = ((int64_t)fd << 40) | (idx << 8);// 这里后续流程不需要core_id和prc_id，因此我们只赋值了fd和idx
	vecfdidx.push_back(fdidcid);
	return enqueue_data_batch_fd(core_id, data, vecfdidx, fdopt);
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

// 调试是否有内存泄漏，但是高频操作在正式环境不合适，map查询非常耗性能
#ifdef DEBUG_MEMPOOL_STATS
static std::map<uintptr_t, int> s_hi_freq_malloc;
static std::map<uintptr_t, int> s_hi_freq_free;
#endif

int high_freq_malloc(struct rte_mempool* pool, void** data, int size)
{
	if(size <= 0) {
		LOG_INFO("invalid size[%d] to malloc.", size);
		return -1;
	}
	int ret = -1;
	if(size > COMMON_PACKET_LEN) {
		LOG_INFO("recieved an large packet, name:%s size:%d", pool->name, size);
		ret = rte_mempool_get(g_mempool_large_data, data);
#ifdef DEBUG_MEMPOOL_STATS
		if(!ret)
			s_hi_freq_malloc[reinterpret_cast<uintptr_t>(g_mempool_large_data)]++;
#endif
	} else {
		ret = rte_mempool_get(pool, data);
#ifdef DEBUG_MEMPOOL_STATS
		if(!ret)
			s_hi_freq_malloc[reinterpret_cast<uintptr_t>(pool)]++;
#endif
	}
	return ret;
}

void high_freq_free(struct rte_mempool* pool, void* data, int size)
{
	if(size <= 0) {
		LOG_INFO("invalid size[%d] to free.", size);
		return ;
	}
	if(size > COMMON_PACKET_LEN) {
		LOG_DEBUG("free an large packet, name:%s size:%d", pool->name, size);
		rte_mempool_put(g_mempool_large_data, data);
#ifdef DEBUG_MEMPOOL_STATS
		s_hi_freq_free[reinterpret_cast<uintptr_t>(g_mempool_large_data)]++;
#endif
	} else {
		rte_mempool_put(pool, data);
#ifdef DEBUG_MEMPOOL_STATS
		s_hi_freq_free[reinterpret_cast<uintptr_t>(pool)]++;
#endif
	}
}

// #include <stdio.h>
void print_mem_statistics()
{
	LOG_WARNING("malloc times: %d", s_malloc_count);
	LOG_WARNING("free times: %d", s_free_count);
#ifdef DEBUG_MEMPOOL_STATS
	for(auto iter : s_hi_freq_malloc) {
		LOG_WARNING("pool[%s] hi_malloc times: %d", (reinterpret_cast<struct rte_mempool*>(iter.first))->name, iter.second);	
	}
	for(auto iter : s_hi_freq_free) {
		LOG_WARNING("pool[%s] hi_free times: %d", (reinterpret_cast<struct rte_mempool*>(iter.first))->name, iter.second);	
	}
#endif
	LOG_WARNING("ws buffer left count:%ld", s_buffer_count);
    if(rte_eal_process_type() != RTE_PROC_PRIMARY) {
    	return;
    }

	LOG_WARNING("****************rte_ring stats*****************");
	for (int i = 0; i < MAX_LCORE_COUNT; ++i)
	{
		if(g_ring_writes[i])
			LOG_WARNING("%s cur count:%ld", g_ring_writes[i]->name, rte_ring_count(g_ring_writes[i]));
		if(g_ring_bwrcvs[i])
			LOG_WARNING("%s cur count:%ld", g_ring_bwrcvs[i]->name, rte_ring_count(g_ring_bwrcvs[i]));
	}
	LOG_WARNING("%s cur count:%ld", g_ring_trans->name, rte_ring_count(g_ring_trans));
	LOG_WARNING("%s cur count:%ld", g_ring_bwfdx->name, rte_ring_count(g_ring_bwfdx));
	LOG_WARNING("%s cur count:%ld", g_ring_master->name, rte_ring_count(g_ring_master));

	LOG_WARNING("****************rte_mempool stats*****************");
	for (int i = 0; i < MAX_LCORE_COUNT; ++i)
	{
		if(g_mempool_write[i])
			LOG_WARNING("%s available count:%ld used count:%u", g_mempool_write[i]->name, rte_mempool_avail_count(g_mempool_write[i]), rte_mempool_in_use_count(g_mempool_write[i]));
		if(g_mempool_bwrcv[i])
			LOG_WARNING("%s available count:%ld used count:%u", g_mempool_bwrcv[i]->name, rte_mempool_avail_count(g_mempool_bwrcv[i]), rte_mempool_in_use_count(g_mempool_bwrcv[i]));
		if(g_mempool_fd_snddata[i])
			LOG_WARNING("%s available count:%ld used count:%u", g_mempool_fd_snddata[i]->name, rte_mempool_avail_count(g_mempool_fd_snddata[i]), rte_mempool_in_use_count(g_mempool_fd_snddata[i]));
	}
	LOG_WARNING("%s available count:%ld used count:%u", g_mempool_trans->name, rte_mempool_avail_count(g_mempool_trans), rte_mempool_in_use_count(g_mempool_trans));
	LOG_WARNING("%s available count:%ld used count:%u", g_mempool_trans_data->name, rte_mempool_avail_count(g_mempool_trans_data), rte_mempool_in_use_count(g_mempool_trans_data));
	LOG_WARNING("%s available count:%ld used count:%u", g_mempool_write_data->name, rte_mempool_avail_count(g_mempool_write_data), rte_mempool_in_use_count(g_mempool_write_data));
	LOG_WARNING("%s available count:%ld used count:%u", g_mempool_bwrcv_data->name, rte_mempool_avail_count(g_mempool_bwrcv_data), rte_mempool_in_use_count(g_mempool_bwrcv_data));
	LOG_WARNING("%s available count:%ld used count:%u", g_mempool_large_data->name, rte_mempool_avail_count(g_mempool_large_data), rte_mempool_in_use_count(g_mempool_large_data));
	LOG_WARNING("%s available count:%ld used count:%u", g_mempool_clifdlist_data->name, rte_mempool_avail_count(g_mempool_clifdlist_data), rte_mempool_in_use_count(g_mempool_clifdlist_data));
	LOG_WARNING("%s available count:%ld used count:%u", g_mempool_ws_buffer->name, rte_mempool_avail_count(g_mempool_ws_buffer), rte_mempool_in_use_count(g_mempool_ws_buffer));

	// const char* dump_mem = "/var/log/tgg_gateway/mem_stat.log"
	// FILE* file = open(dump_mem, "w+");
	// if(!file) {
	// 	LOG_WARNING("open dump_mem:%s failed.", dump_mem);
	// 	return;
	// }
	// rte_mempool_dump(stdout, g_mempool_clifdlist_data);
}


static int get_exec_path(char* exe_path, const char* exec_name)
{
    ssize_t len = readlink("/proc/self/exe", exe_path, PATH_MAX - 1);
    if (len == -1) {
        LOG_ERROR("readlink failed: %s", strerror(errno));
        return -1;
    }
    exe_path[len] = '\0';
    
    // 查找最后一个斜杠位置
    char *last_slash = strrchr(exe_path, '/');
    if (!last_slash) {
        LOG_ERROR("Invalid path format: %s", exe_path);
        return -1;
    }
    
    // 安全拼接新文件名
    size_t name_len = strlen(exec_name);
    if (last_slash - exe_path + name_len + 1 >= PATH_MAX) {
        LOG_ERROR("Path too long: %s + %s", exe_path, exec_name);
        return -1;
    }
    
    // 直接覆盖原文件名部分
    strcpy(last_slash + 1, exec_name);
    LOG_DEBUG("Final exec path: %s", exe_path);
    return 0;
}

static pid_t custom_fork(const char* exec_name, int prc_id, char* args[])
{
    char exe_path[PATH_MAX] = {0};
    if (get_exec_path(exe_path, exec_name) < 0) {
        return -1;
    }
    
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("fork failed: %s", strerror(errno));
        return -1;
    }
    
    if (pid == 0) {  // 子进程
        // 验证可执行文件
        if (access(exe_path, X_OK) != 0) {
            fprintf(stderr, "ERROR: Cannot execute %s: %s\n", 
                    exe_path, strerror(errno));
            _exit(EXIT_FAILURE);
        }
        
        // 执行程序
        execv(exe_path, args);
        
        // 如果execv返回，说明执行失败
        fprintf(stderr, "FATAL: execv failed for %s: %s\n", 
                exe_path, strerror(errno));
        _exit(EXIT_FAILURE);
    }
    
    return pid;
}

pid_t start_gwrcv_sendary(int lcore_id)
{
    char proc_id[24]; // 栈上分配
    snprintf(proc_id, sizeof(proc_id), "--proc-id=%d", lcore_id);
    
    // 参数数组（栈上分配）
    char* args[] = {
        const_cast<char*>("gwrcv"),    // 程序名
        proc_id,    // 参数
        NULL        // 结束标记
    };
    
    return custom_fork("gwrcv", lcore_id, args);
}

pid_t start_gwrcv_reactor_sendary(int lcore_id)
{
    char proc_id[24]; // 栈上分配
    snprintf(proc_id, sizeof(proc_id), "--proc-id=%d", lcore_id);
    
    // 参数数组（栈上分配）
    char* args[] = {
        const_cast<char*>("gwrcv"),    // 程序名
        proc_id,    // 参数
        NULL        // 结束标记
    };
    
    return custom_fork("gwrcv", lcore_id, args);
}

pid_t start_gwcliprc(int lcore_id)
{
    // 参数数组（栈上分配）
    char* args[] = {
        const_cast<char*>("gwcliprc"), // 程序名
        NULL        // 结束标记
    };
    
    return custom_fork("gwcliprc", lcore_id, args);
}

pid_t start_register(int lcore_id)
{
    // 参数数组（栈上分配）
    char* args[] = {
        const_cast<char*>("gwregister"), // 程序名
        NULL          // 结束标记
    };
    
    return custom_fork("gwregister", lcore_id, args);
}

static uint32_t get_mask_value(uint32_t mask, int index)
{
    // 使用位操作高效查找第index个置位
    int count = 0;
    for (int pos = 0; pos < 32; pos++) {
        if (mask & (1U << pos)) {
            if (count == index) {
                return (1U << pos);
            }
            count++;
        }
    }
    return 0; // 未找到
}

pid_t start_gwbwprc(int prc_id)
{
    uint32_t mask_val = get_mask_value(
        TggConfigure::getInstance()->get_bcore_mask(), 
        prc_id
    );
    
    char proc_mask[16]; // 栈上分配
    snprintf(proc_mask, sizeof(proc_mask), "-c%x", mask_val);
    
    // 参数数组（栈上分配）
    char* args[] = {
        const_cast<char*>("gwbwprc"),  // 程序名
        proc_mask,   // 掩码参数
        NULL         // 结束标记
    };
    
    return custom_fork("gwbwprc", prc_id, args);
}
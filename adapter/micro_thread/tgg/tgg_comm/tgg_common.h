#ifndef _TGG_COMMON_H_
#define _TGG_COMMON_H_

#include <string>
#include <map>
#include "tgg_struct.h"

#define BW_PRC_HEART_BEAT 5000  // bwprc进程心跳最大5s
#define GW_MONITOR_HEART_BEAT 5000  // gw进程心跳最大5s

void init_endians();

bool big_endian();

// 生成fdidcid
int64_t generate_fdidcid(int core_id, int fd, int cid);

// 生成cid
int generate_cid(int core_id, int idx);

// 生成bwfdx
int generate_bwfdx(int prc_id, int fd);


int tgg_get_cli_idx(int core_id, int fd);
int tgg_get_cli_status(int core_id, int fd);
int tgg_get_cli_authorized(int core_id, int fd);
std::string tgg_get_cli_ip_str(int core_id, int fd);
uint32_t tgg_get_cli_ip(int core_id, int fd);
ushort tgg_get_cli_port(int core_id, int fd);
int tgg_get_cli_bwfdx(int core_id, int fd);
// 不能返回引用，内部加锁的
std::string tgg_get_cli_uid(int core_id, int fd);
int tgg_get_cli_cid(int core_id, int fd);
std::string tgg_get_cli_reserved(int core_id, int fd);
int tgg_set_cli_idx(int core_id, int fd, int idx);
int tgg_set_cli_status(int core_id, int fd, int status);
int tgg_set_cli_authorized(int core_id, int fd, int authorized);
int tgg_set_cli_ip(int core_id, int fd, uint32_t ip);
int tgg_set_cli_port(int core_id, int fd, ushort port);
int tgg_set_cli_bwfdx(int core_id, int fd, int bwfdx);
int tgg_set_cli_uid(int core_id, int fd, const char* uid);
int tgg_set_cli_cid(int core_id, int fd, int cid);
int tgg_set_cli_reserved(int core_id, int fd, const char* reserved);
void tgg_close_cli(int core_id, int fd);
int tgg_init_cli(int core_id, int fd, char* ip_str, uint32_t ip, ushort port);

int tgg_init_cli_bw(int core_id, int fd, int cid);
void tgg_close_cli_bw(int core_id, int fd);


int tgg_get_bwfdx_status(int prc_id, int fd);
int tgg_get_bwfdx_cmd(int prc_id, int fd);
int tgg_get_bwfdx_idx(int prc_id, int fd);
int tgg_get_bwfdx_authorized(int prc_id, int fd);
int tgg_get_bwfdx_ip(int prc_id, int fd);
int tgg_get_bwfdx_port(int prc_id, int fd);
std::string tgg_get_bwfdx_seckey(int prc_id, int fd);
std::string tgg_get_bwfdx_workerkey(int prc_id, int fd);

int tgg_set_bwfdx_status(int prc_id, int fd, int status);
int tgg_set_bwfdx_load(int prc_id, int fd, int load);
int tgg_set_bwfdx_cmd(int prc_id, int fd, int cmd);
int tgg_set_bwfdx_idx(int prc_id, int fd, int idx);
int tgg_set_bwfdx_authorized(int prc_id, int fd, int authorized);
int tgg_set_bwfdx_ip(int prc_id, int fd, uint32_t ip);
int tgg_set_bwfdx_port(int prc_id, int fd, ushort port);
int tgg_set_bwfdx_seckey(int prc_id, int fd, const char* seckey);
int tgg_set_bwfdx_workerkey(int prc_id, int fd, const char* workerkey);

int tgg_get_bwfdx_load(int64_t fdid);
int tgg_add_bwfdx_load(int fdid);

// 重置进程对应的所有fd状态，子进程宕机的情况，父进程要对这些fd进行重置，防止后续的cli继续使用这些无效的fd
void tgg_init_bwfdx_prc(int prc_id);
// 使用fd为0的位置来记录进程自身的状态,暂时不用
int tgg_get_bw_prcstatus(int prc_id);
int tgg_set_bw_prcstatus(int prc_id, int status);
int tgg_clean_bwfdx(int prc_id, int fd);

// 获取一个有效的bw进程编号
int tgg_get_valid_bwprc(int bwcount, uint64_t now);
// 更新进程编号对应的时间戳
void tgg_update_bwprc(int prc_id, uint64_t now);
// 获取指定编号的进程的进程id
int tgg_get_bwprc_pid(int prc_id);
// 检查指定进程是否超时了
int tgg_checkif_bwprc_timeout(int prc_id, uint64_t now);
// 清理指定进程相关数据
void tgg_clean_bwprc(int prc_id);

int tgg_setup_gw_monitor(int prc_id);
// 更新进程编号对应的时间戳
void tgg_update_gw_monitor(int prc_id, uint64_t now);
// 获取指定编号的进程的进程id
int tgg_get_gw_monitor_pid(int prc_id);
// 检查指定进程是否超时了
int tgg_checkif_gw_monitor_timeout(int prc_id, uint64_t now);
// 清理指定进程相关数据
void tgg_clean_gw_monitor(int prc_id);


// 给ws操作缓存的函数  
// int cache_ws_buffer(int core_id, int fd, void* data, int len, int pos = 0, int iscomplete = 1);
std::string get_one_frame_buffer(int core_id, int fd, void* data, int len);
std::string get_whole_buffer(int core_id, int fd);
void release_ws_buffer(int core_id, int fd);

// move_pos 是否要移动读指针
int ringbuf_read(int core_id, int fd, std::string& dest, int len, int move_pos);
int ringbuf_write(int core_id, int fd, const char* data, int len);
int ringbuf_size(int core_id, int fd);
int ringbuf_space(int core_id, int fd);
const char* ringbuf_memmem(tgg_ws_data* rb, const char* needle, int needle_len);

int tgg_enqueue_trans(tgg_bw_data* data);
int tgg_dequeue_trans(tgg_bw_data** data);
int tgg_enqueue_bwfdx(tgg_bwfdx_data* data);
int tgg_dequeue_bwfdx(tgg_bwfdx_data** data);

int tgg_enqueue_write(int core_id, tgg_write_data* data);
int tgg_dequeue_write(int core_id, tgg_write_data** data);

tgg_bw_data* get_bwdata_from_transdata(int prc_id, tgg_trans_data* tdata);

int tgg_enqueue_bwsnd(int queue_id, tgg_bw_data* data);
int tgg_dequeue_bwsnd(int queue_id, tgg_bw_data** data);


void init_core(const char* dumpfile);

void* dpdk_rte_malloc(int size);
void dpdk_rte_free(void* pdata);


int high_freq_malloc(struct rte_mempool* pool, void** data, int size);
void high_freq_free(struct rte_mempool* pool, void* data, int size);
void print_mem_statistics();


// bw侧接口
void tgg_new_bw_session(int prc_id, int fd, int cmd, 
    const char* workerkey, uint32_t remote_ip = 0, ushort remote_port = 0);
void tgg_close_bw_session(int prc_id, int fd);

// 业务侧接口

// cid绑定uid
int tgg_bind_session(const char* uid, int cid);
// cid和uid解绑
int tgg_unbind_session(int cid);
// 连接建立
int tgg_init_session(int core_id, int fd, int idx);

// 连接断开
int tgg_free_session(int core_id, int fd, int cid);
// 加入组
int tgg_join_group(const char* gid, int cid);
// 退出组
int tgg_exit_group(const char* gid, int cid);

// 获取可用的idx
int get_valid_idx();

// 清理队列
void clean_trans_data(tgg_trans_data* bdata);
void clean_bw_data(int prc_id, tgg_bw_data* bdata);
void clean_write_data(int core_id, tgg_write_data* wdata);
void clean_fdidlist(tgg_fd_id_list* fdiddata);


// 发送给客户端
tgg_write_data* format_send_data(int core_id, const std::string& sdata, std::map<int, int>& mapfdidx, int fdopt);
int enqueue_data_batch_fd(int core_id, const std::string& data, std::map<int, int>& mapfdidx, int fdopt);
int enqueue_data_single_fd(int core_id, const std::string& data, int fd, int idx, int fdopt);


void custom_fork(const char* exec_name, char** args);

#endif  // _TGG_COMMON_H_
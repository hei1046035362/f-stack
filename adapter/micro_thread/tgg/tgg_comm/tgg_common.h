#ifndef _TGG_COMMON_H_
#define _TGG_COMMON_H_

#include <string>
#include <map>
#include "tgg_struct.h"

void init_endians();

bool big_endian();

int tgg_get_cli_idx(int core_id, int fd);
int tgg_get_cli_status(int core_id, int fd);
int tgg_get_cli_authorized(int core_id, int fd);
uint32_t tgg_get_cli_ip(int core_id, int fd);
ushort tgg_get_cli_port(int core_id, int fd);
int tgg_get_cli_bwfdx(int core_id, int fd);
// 不能返回引用，内部加锁的
std::string tgg_get_cli_uid(int core_id, int fd);
std::string tgg_get_cli_cid(int core_id, int fd);
std::string tgg_get_cli_reserved(int core_id, int fd);
int tgg_set_cli_idx(int core_id, int fd, int idx);
int tgg_set_cli_status(int core_id, int fd, int status);
int tgg_set_cli_authorized(int core_id, int fd, int authorized);
int tgg_set_cli_ip(int core_id, int fd, uint32_t ip);
int tgg_set_cli_port(int core_id, int fd, ushort port);
int tgg_set_cli_bwfdx(int core_id, int fd, int bwfdx);
int tgg_set_cli_uid(int core_id, int fd, const char* uid);
int tgg_set_cli_cid(int core_id, int fd, const char* cid);
int tgg_set_cli_reserved(int core_id, int fd, const char* reserved);
void tgg_close_cli(int core_id, int fd);
int tgg_init_cli(int core_id, int fd, uint32_t ip, ushort port);




int tgg_get_bwfdx_status(int prc_id, int fd);
int tgg_get_bwfdx_load(int prc_id, int fd);
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

int tgg_add_bwfdx_load(int prc_id, int fd);

// 重置进程对应的所有fd状态，子进程宕机的情况，父进程要对这些fd进行重置，防止后续的cli继续使用这些无效的fd
void tgg_init_bwfdx_prc(int prc_id);
// 使用fd为0的位置来记录进程自身的状态,暂时不用
int tgg_get_bw_prcstatus(int prc_id);
int tgg_set_bw_prcstatus(int prc_id, int status);
int tgg_clean_bwfdx(int prc_id, int fd);




// 给ws操作缓存的函数  
int cache_ws_buffer(int core_id, int fd, void* data, int len, int pos = 0, int iscomplete = 1);
std::string get_one_frame_buffer(int core_id, int fd, void* data, int len);
std::string get_whole_buffer(int core_id, int fd);
void clean_ws_buffer(int core_id, int fd);




int tgg_enqueue_read(tgg_read_data* data);
int tgg_dequeue_read(tgg_read_data** data);
int tgg_enqueue_cliprc(int core_id, tgg_read_data* data);
int tgg_dequeue_cliprc(int core_id, tgg_read_data** data);

int tgg_enqueue_trans(tgg_bw_data* data);
int tgg_dequeue_trans(tgg_bw_data** data);
int tgg_enqueue_bwrcv(int prc_id, tgg_bw_data* data);
int tgg_dequeue_bwrcv(int prc_id, tgg_bw_data** data);

int tgg_enqueue_write(int core_id, tgg_write_data* data);
int tgg_dequeue_write(int core_id, tgg_write_data** data);

int tgg_enqueue_bwsnd(int queue_id, tgg_bw_data* data);
int tgg_dequeue_bwsnd(int queue_id, tgg_bw_data** data);


void init_core(const char* dumpfile);

void* dpdk_rte_malloc(int size);

// bw侧接口
void tgg_new_bw_session(int prc_id, int fd, int cmd, 
    uint32_t remote_ip = 0, ushort remote_port = 0);
void tgg_close_bw_session(int prc_id, int fd);

// 业务侧接口
// 新接入连接
int tgg_bind_session(int core_id, int fd, const char* uid, const char* cid);
// 连接断开
int tgg_free_session(int core_id, int fd);
// 加入组
int tgg_join_group(const char* gid, const char* cid);
// 退出组
int tgg_exit_group(const char* gid, const char* cid);

// 获取可用的idx
int get_valid_idx();

// 通过idx生成cid
std::string get_valid_cid(int idx);

// 清理队列
void clean_bw_data(tgg_bw_data* bdata);
void clean_read_data(tgg_read_data* rdata);
void clean_write_data(tgg_write_data* wdata);


// 发送给客户端
tgg_write_data* format_send_data(const std::string& sdata, std::map<int, int>& mapfdidx, int fdopt);
int enqueue_data_batch_fd(int core_id, const std::string& data, std::map<int, int>& mapfdidx, int fdopt);
int enqueue_data_single_fd(int core_id, const std::string& data, int fd, int idx, int fdopt);

// 发送给服务端
tgg_read_data* format_send_server_data(int core_id, int fd, const std::string& sdata, int fdopt);
int enqueue_data_send_server(int core_id, int fd, const std::string& data, int fdopt);

#endif  // _TGG_COMMON_H_
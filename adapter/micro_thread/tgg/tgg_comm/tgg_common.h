#ifndef _TGG_COMMON_H_
#define _TGG_COMMON_H_

#include <string>
#include <map>
#include "tgg_struct.h"

void init_endians();

bool big_endian();

int tgg_get_cli_idx(int fd);
int tgg_get_cli_status(int fd);
int tgg_get_cli_authorized(int fd);
std::string tgg_get_cli_uid(int fd);
std::string tgg_get_cli_cid(int fd);
std::string tgg_get_cli_reserved(int fd);
int tgg_set_cli_idx(int fd, int idx);
int tgg_set_cli_status(int fd, int status);
int tgg_set_cli_authorized(int fd, int authorized);
int tgg_set_cli_uid(int fd, const char* uid);
int tgg_set_cli_cid(int fd, const char* cid);
int tgg_set_cli_reserved(int fd, const char* reserved);
void tgg_close_cli(int fd);
int tgg_init_cli(int fd);

// 给ws操作缓存的函数  
int cache_ws_buffer(int fd, void* data, int len, int pos = 0, int iscomplete = 1);
std::string get_one_frame_buffer(int fd, void* data, int len);
std::string get_whole_buffer(int fd);
void clean_ws_buffer(int fd);




int tgg_enqueue_read(tgg_read_data* data);
int tgg_enqueue_write(tgg_write_data* data);
int tgg_dequeue_read(tgg_read_data** data);
int tgg_dequeue_write(tgg_write_data** data);
int tgg_enqueue_bwrcv(tgg_bw_data* data);
int tgg_dequeue_bwrcv(tgg_bw_data** data);


void init_core(const char* dumpfile);

void* dpdk_rte_malloc(int size);


// 业务侧接口
// 新接入连接
int tgg_bind_session(int fd, const char* uid, const char* cid);
// 连接断开
int tgg_free_session(int fd);
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
int enqueue_data_batch_fd(const std::string& data, std::map<int, int>& mapfdidx, int fdopt);
int enqueue_data_single_fd(const std::string& data, int fd, int idx, int fdopt);

#endif  // _TGG_COMMON_H_
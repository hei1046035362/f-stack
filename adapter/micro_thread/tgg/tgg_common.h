#ifndef _TGG_COMMON_H_
#define _TGG_COMMON_H_

#include "tgg_struct.h"

// 生成gid  ip4地址+idx
const char* generate_cid();


tgg_cli_info* tgg_get_cli_data(int fd);
int tgg_enqueue_read(tgg_read_data* data);
int tgg_enqueue_write(tgg_write_data* data);
int tgg_dequeue_read(tgg_read_data** data);
int tgg_dequeue_write(tgg_write_data** data);
int get_fd_by_uid(const char* uid);
void init_core(const char* dumpfile);

void* dpdk_rte_malloc(int size);

/// 增删查  gid
int tgg_add_gid(char* gid, tgg_gid_data* data);
int tgg_del_gid(char* gid);
tgg_gid_data* tgg_get_gid(char* gid);

/// 增删查  uid
int tgg_add_uid(char* uid, tgg_uid_data* data);
int tgg_del_uid(char* uid);
tgg_uid_data* tgg_get_uid(char* uid);

/// 增删查  cid
int tgg_add_cid(char* cid, tgg_cid_data* data);
int tgg_del_cid(char* cid);
tgg_cid_data* tgg_get_cid(char* cid);


// 业务侧接口
int tgg_bind_session(int fd);
int tgg_free_session(int fd);
int tgg_join_group(const char* cid);

// 生成一个可用的cid
const char* get_valid_cid();

#endif  // _TGG_COMMON_H_
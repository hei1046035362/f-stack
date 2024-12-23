#ifndef __TGG_BWCOMM_H__
#define __TGG_BWCOMM_H__
#include <vector>
#include <string>
#include "tgg_comm/tgg_struct.h"

int get_connection_info(int fd, const char* ip, unsigned short* port);

tgg_bw_info* lookup_bwinfo(int fd);

tgg_bw_info* get_valid_bwinfo_by_fd(int fd);

int message_pack(uint16_t command, uint32_t seq, uint8_t protocol,
            uint8_t compressFormat, const std::string& body, std::string& result);

int message_unpack(const std::string& packedData, std::string& result);

int tgg_init_uidgid(const std::vector<std::string>& clusterNodes, const std::string& password, const std::string& userName = "");

// 或缺一个可用的idx
int get_valid_bw_idx();

// 用于标记bwidx是否已经存在的set<idx>
bool tgg_exist_bw_idx(int bwidx);
bool tgg_add_bw_idx(int bwidx);
bool tgg_delete_bw_idx(int bwidx);

// 操作map<fd, bw_info*>
int tgg_set_bw_idx(int bwidx);
int tgg_get_bw_idx(int bwidx);

int tgg_set_bw_seckey(int fd, const std::string& seckey);
int tgg_get_bw_seckey(int fd, std::string& seckey);

void tgg_set_bw_authorized(int fd, int authorized);
int tgg_get_bw_authorized(int fd);

void tgg_set_bw_load(int fd, int load);
int tgg_get_bw_load(int fd);

int new_bw_session(int fd);
void free_bw_session(int fd);

#endif  // __TGG_BWCOMM_H__
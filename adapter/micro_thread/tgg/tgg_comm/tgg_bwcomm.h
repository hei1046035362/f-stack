#ifndef __TGG_BWCOMM_H__
#define __TGG_BWCOMM_H__
#include <vector>
#include <string>
#include "tgg_comm/tgg_struct.h"

int get_connection_info(int fd, const char* ip, unsigned short* port);

tgg_bw_info* lookup_bwinfo(int fd);

tgg_bw_info* get_valid_bwinfo_by_fd(int fd);

std::string message_pack(uint16_t command, uint32_t seq, uint8_t protocol,
            uint8_t compressFormat, const std::string& body);

std::string message_unpack(const std::string& packedData);

void init_endians();
bool big_endian();

int tgg_init_uidgid(const std::vector<std::string>& clusterNodes, const std::string& password, const std::string& userName = "");

#endif  // __TGG_BWCOMM_H__
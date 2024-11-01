#ifndef __TGG_BWCOMM_H__
#define __TGG_BWCOMM_H__
#include <vector>
#include <string>
#include "tgg_struct.h"

int get_connection_info(int fd, char[] ip, unsigned short* port);

tgg_bw_info* lookup_bwinfo(int fd);

tgg_bw_info* get_valid_bwinfo_by_fd(int fd);

std::vector<uint8_t> message_pack(uint16_t command, uint32_t seq, uint8_t protocol,
            uint8_t compressFormat, const std::string& body);

std::vector<std::string> message_unpack(const std::string& packedData);

std::string hex2bin(const std::string& hex);

#endif  // __TGG_BWCOMM_H__
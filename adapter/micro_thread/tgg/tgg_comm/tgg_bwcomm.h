#ifndef __TGG_BWCOMM_H__
#define __TGG_BWCOMM_H__
#include <vector>
#include <list>
#include <string>
#include "tgg_comm/tgg_struct.h"

int get_connection_info(int fd, char* ip_str, unsigned int* ip, unsigned short* port);

tgg_bw_info* lookup_bwinfo(int fd);

tgg_bw_info* get_valid_bwinfo_by_fd(int fd);

int message_pack(uint16_t command, uint32_t seq, uint8_t protocol,
            uint8_t compressFormat, std::string_view body, std::string& result);

int message_unpack(const char* packedData, size_t sdata_len, std::string& result);

template <typename T>
int write_list_to_file(const std::string& filename, 
                     const std::string& header,
                     const std::vector<T>& dataList,
                     size_t buffer_kb = 8);

template <>
int write_list_to_file<int>(const std::string& filename, 
                     const std::string& header,
                     const std::vector<int>& dataList,
                     size_t buffer_kb);

template <>
int write_list_to_file<int64_t>(const std::string& filename, 
                     const std::string& header,
                     const std::vector<int64_t>& dataList,
                     size_t buffer_kb);

template <>
int write_list_to_file<uint64_t>(const std::string& filename, 
                     const std::string& header,
                     const std::vector<uint64_t>& dataList,
                     size_t buffer_kb);

template <>
int write_list_to_file<std::string>(const std::string& filename, 
                     const std::string& header,
                     const std::vector<std::string>& dataList,
                     size_t buffer_kb);

#endif  // __TGG_BWCOMM_H__
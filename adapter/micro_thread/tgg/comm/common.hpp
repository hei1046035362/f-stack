#ifndef __COMMON_HPP__
#define __COMMON_HPP__

#include <string>
#include <vector>

std::string tgg_trim(const std::string& str);

bool is_ipv4(const std::string& str);

bool is_ipport_format(const std::string& str);


void split_string(const std::string& str, char delimiter, std::vector<std::string>& result);

std::string hex2bin(const std::string& hex);

std::string bin2hex(const std::string& input, bool bForLog = true, bool uppercase = false);

// 函数：将无符号整数转换为十六进制字符串
std::string uint32_to_hex(uint32_t ip);

uint64_t get_system_ms(void);

int count_ones(unsigned int n);

int wait_all_child_exit();

#endif // __COMMON_HPP__
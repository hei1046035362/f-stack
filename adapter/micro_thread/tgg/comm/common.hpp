#ifndef __COMMON_HPP__
#define __COMMON_HPP__

#include <string>
#include <vector>
#include <cstdint>

std::string tgg_trim(const std::string& str);

bool is_ipv4(const std::string& str);

bool is_ipport_format(const std::string& str);


void split_string(const std::string& str, char delimiter, std::vector<std::string>& result);

std::string hex2bin(const std::string& hex);

std::string bin2hex(std::string_view input, bool bForLog = true, bool uppercase = false);

// 函数：将无符号整数转换为十六进制字符串
std::string uint32_to_hex(uint32_t ip);

uint64_t get_system_ms(void);

int count_ones(unsigned int n);

int wait_all_child_exit();

#include <map>
struct HttpRequest {
    std::string method;
    std::string uri;
    std::string protocol;
    std::string host;
    std::string content_type;
    std::vector<std::pair<std::string, std::string>> headers; // 改用vector减少红黑树开销
    std::vector<std::pair<std::string, std::string>> query;
    std::vector<std::pair<std::string, std::string>> cookies;
};
void parse_http_request(const char* data, size_t len, HttpRequest& req, bool parse_cookies = true);

bool ensure_path_exists(const std::string& path, bool writelog = true);

#endif // __COMMON_HPP__
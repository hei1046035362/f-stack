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
#include "picohttpparser.h"
struct http_str_t {
    const char* data;
    size_t len;
};
struct http_request_t {
    http_str_t method;
    http_str_t uri;
    int minor_version;
    http_str_t host;
    http_str_t content_type;
    // http_str_t path;
    struct phr_header headers[50]; // 改用vector减少红黑树开销
    size_t num_headers;
    struct phr_header query_params[50];
    size_t num_query_params;
    struct phr_header cookies[20];
    size_t num_cookies;
    int error;
};
int parse_http_request(const char* data, size_t len,
                           http_request_t* req, int parse_cookies_flag = 0);
int parse_cookies(const char* str, size_t len,
                        struct phr_header* cookies, int max);
bool ensure_path_exists(const std::string& path, bool writelog = true);

#endif // __COMMON_HPP__
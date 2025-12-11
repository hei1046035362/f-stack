#include <iostream>
#include <regex>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include "common.hpp"
#include <iomanip>
#include <string.h>
#include "log.hpp"
#include <array>
#include <sys/stat.h>
#include <unistd.h>

std::string tgg_trim(const std::string& str) {
    auto start = str.begin();
    while (start != str.end() && std::isspace(*start)) {
        ++start;
    }

    auto end = str.end();
    do {
        --end;
    } while (end != start && std::isspace(*end));

    return std::string(start, end + 1);
}

bool is_ipv4(const std::string& str) {
    std::regex ipv4_pattern("^(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})$");
    std::smatch match;
    if (std::regex_match(str, match, ipv4_pattern)) {
        for (size_t i = 1; i < match.size(); ++i) {
            int num = std::stoi(match[i].str());
            if (num < 0 || num > 255) {
                return false;
            }
        }
        return true;
    }
    return false;
}

bool is_ipport_format(const std::string& str) {
    std::regex pattern("^((\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})):(\\d{1,5})$");
    std::smatch match;
    if (std::regex_match(str, match, pattern)) {
        // 检查IP部分的每个数字段是否在合法范围（0-255）
        for (size_t i = 2; i < 6; ++i) {
            int num = std::stoi(match[i].str());
            if (num < 0 || num > 255) {
                return false;
            }
        }
        // 检查端口部分是否在合法范围（0-65535）
        int port = std::stoi(match[6].str());
        if (port < 0 || port > 65535) {
            return false;
        }
        return true;
    }
    return false;
}


void split_string(const std::string& str, char delimiter, std::vector<std::string>& result) {
    size_t start = 0;
    size_t end = str.find(delimiter);
    while (end!= std::string::npos) {
        result.push_back(str.substr(start, end - start));
        start = end + 1;
        end = str.find(delimiter, start);
    }
    result.push_back(str.substr(start));
}

// 函数：将无符号整数转换为十六进制字符串
std::string uint32_to_hex(uint32_t ip) {
    char buffer[9];
    memset(buffer, 0, sizeof(buffer));
    sprintf(buffer, "%08X", ip);
    return std::string(buffer);
}

uint64_t get_system_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL);
};

static constexpr unsigned char hexCharToValue(unsigned char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

std::string hex2bin(const std::string& hex) {
    if (hex.length() % 2 != 0) {
        LOG_ERROR("Hex string must have even length");
        return "";
    }

    std::string binary;
    binary.resize(hex.size() / 2);

    // 修复方案1：使用静态局部数组（线程安全）[6](@ref)
    static const auto hexTable = [] {
        std::array<unsigned char, 256> table{}; // 使用std::array
        for (int i = 0; i < 256; ++i) {
            table[i] = hexCharToValue(static_cast<unsigned char>(i));
        }
        return table; // 安全返回（std::array可拷贝）
    }();

    for (size_t i = 0, j = 0; i < hex.length(); i += 2, ++j) {
        const auto c1 = static_cast<unsigned char>(hex[i]);
        const auto c2 = static_cast<unsigned char>(hex[i+1]);
        const auto high = hexTable[c1];
        const auto low = hexTable[c2];

        if (high > 15 || low > 15) {
            LOG_ERROR("Invalid hex char: %c%c", c1, c2);
            return "";
        }

        binary[j] = static_cast<char>((high << 4) | low);
    }
    return binary;
}

std::string bin2hex(std::string_view input, bool bForLog, bool uppercase) {
    if(bForLog && AsyncLogger::getInstance().getloglevel() != LogLevel::DEBUG) {
        return "";
    }
    // 预分配目标字符串内存（避免动态扩容）
    std::string output;
    output.resize(input.size() * 2);
    
    // 十六进制字符表（小写/大写双版本）
    static const char hex_table_lower[] = "0123456789abcdef";
    static const char hex_table_upper[] = "0123456789ABCDEF";
    const char* hex_table = uppercase ? hex_table_upper : hex_table_lower;

    // 核心转换逻辑
    for (size_t i = 0; i < input.size(); ++i) {
        const auto c = static_cast<unsigned char>(input[i]);
        output[i * 2]     = hex_table[c >> 4];   // 高4位
        output[i * 2 + 1] = hex_table[c & 0x0F];  // 低4位
    }
    return output;
}

int count_ones(unsigned int n) {
    int count = 0;
    while (n) {
        n &= (n - 1);  // 每次清除一个1
        count++;
    }
    return count;
}

int wait_all_child_exit()
{
    int status;
    pid_t child_pid;
    while ((child_pid = wait(&status)) != -1) { // 阻塞等待任意子进程
        if (WIFEXITED(status)) {
            LOG_INFO("child[pid:%d] exit, ret: %d.", child_pid, WEXITSTATUS(status));
        }
    }

    if (errno != ECHILD) { // 确保因无子进程而退出
        LOG_ERROR("wait error");
        return 1;
    }
    LOG_INFO("all child exited");
    return 0;
}

// 解析查询字符串
static int parse_query_string(const char* q, size_t qlen, 
                             struct phr_header* params, int max) {
    int cnt = 0;
    const char* end = q + qlen;
    const char* p = q;
    
    while (p < end && cnt < max) {
        if (*p == '&') { p++; continue; }
        
        const char* kstart = p;
        while (p < end && *p != '=' && *p != '&') p++;
        size_t klen = p - kstart;
        
        const char* vstart = NULL;
        size_t vlen = 0;
        
        if (p < end && *p == '=') {
            p++;
            vstart = p;
            while (p < end && *p != '&') p++;
            vlen = p - vstart;
        } else {
            vstart = p;
            vlen = 0;
        }
        
        if (klen > 0) {
            params[cnt].name = kstart;
            params[cnt].name_len = klen;
            params[cnt].value = vstart;
            params[cnt].value_len = vlen;
            cnt++;
        }
        
        if (p < end && *p == '&') p++;
    }
    
    return cnt;
}

// 解析 Cookie
int parse_cookies(const char* str, size_t len,
                        struct phr_header* cookies, int max) {
    int cnt = 0;
    const char* end = str + len;
    const char* p = str;
    char* nstart;
    char* vstart;
    size_t nlen;
    
    while (p < end && cnt < max) {
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end) break;
        
        nstart = (char*)p;
        
        while (p < end && *p != '=') {
            if (*p == ';') {
                cookies[cnt].name = nstart;
                cookies[cnt].name_len = p - nstart;
                cookies[cnt].value = p;
                cookies[cnt].value_len = 0;
                cnt++;
                p++;
                goto next;
            }
            p++;
        }
        
        if (p >= end) break;
        
        nlen = p - nstart;
        p++;
        
        vstart = (char*)p;
        while (p < end && *p != ';') p++;
        
        cookies[cnt].name = nstart;
        cookies[cnt].name_len = nlen;
        cookies[cnt].value = vstart;
        cookies[cnt].value_len = p - vstart;
        cnt++;
        
        if (p < end && *p == ';') p++;
        
    next:
        continue;
    }
    
    return cnt;
}
static inline int is_space(char c) {
    return c == ' ' || c == '\t';
}
int parse_http_request(const char* data, size_t len,
                           http_request_t* req, int parse_cookies_flag) {
    memset(req, 0, sizeof(*req));
    req->num_headers = 50;
    
    // 解析 HTTP 请求
    int ret = phr_parse_request(data, len,
                               &req->method.data, &req->method.len,
                               &req->uri.data, &req->uri.len,
                               &req->minor_version,
                               req->headers, &req->num_headers, 0);
    
    if (ret <= 0) {
        LOG_ERROR("parse request failed,data:%s, len:%d, num_headers:%d.", data, len, req->num_headers);
        req->error = ret;
        return 0;
    }
    for (size_t i = 0; i < req->num_headers; i++) {
        if(req->headers[i].name_len == 4 && 
            0 == strncasecmp(req->headers[i].name, "host", req->headers[i].name_len)) {
            req->host.data = req->headers[i].value;
            req->host.len = req->headers[i].value_len;
        }
    }
    // 解析查询参数
    for (size_t i = 0; i < req->uri.len; i++) {
        if (req->uri.data[i] == '?') {
            const char* qstr = req->uri.data + i + 1;
            size_t qlen = req->uri.len - (i + 1);
            req->num_query_params = parse_query_string(qstr, qlen,
                                                      req->query_params, 50);
            break;
        }
    }
    
    // 解析 Cookie
    if (parse_cookies_flag) {
        for (size_t i = 0; i < req->num_headers; i++) {
            if (strncasecmp("cookie", req->headers[i].name, 
                           req->headers[i].name_len) == 0) {
                req->num_cookies = parse_cookies(req->headers[i].value,
                                                req->headers[i].value_len,
                                                req->cookies, 20);
                break;
            }
        }
    }
    
    // 记录请求体位置
    // if ((size_t)ret < len) {
    //     req->body = data + ret;
    //     req->body_len = len - ret;
    // }
    
    return 1;
}

bool ensure_path_exists(const std::string& path, bool writelog)
{
    struct stat info;
    if (stat(path.c_str(), &info) != 0) {      // 路径不存在
        if (mkdir(path.c_str(), 0755) == 0) {  // 创建单级目录
            if (writelog) LOG_INFO("create path:%s", path.c_str());
            return true;
        }
        if (writelog) LOG_ERROR("create path:%s failed", path.c_str());
        return false;
    } else if (S_ISDIR(info.st_mode)) {        // 已存在且是目录
        return true;
    }
    if (writelog) LOG_ERROR("path:%s is not a directory", path.c_str());
    return false;
}
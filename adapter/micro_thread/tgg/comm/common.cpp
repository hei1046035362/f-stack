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

std::string bin2hex(const std::string& input, bool bForLog, bool uppercase) {
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


// 1. URL解码优化：预分配内存+避免子串复制
inline void url_decode_inplace(std::string& src) {
    size_t src_idx = 0, dst_idx = 0;
    for (; src_idx < src.size(); ++src_idx) {
        if (src[src_idx] == '%' && src_idx + 2 < src.size()) {
            char c1 = std::tolower(src[src_idx + 1]);
            char c2 = std::tolower(src[src_idx + 2]);
            uint8_t val = (c1 >= 'a' ? c1 - 'a' + 10 : c1 - '0') * 16 +
                          (c2 >= 'a' ? c2 - 'a' + 10 : c2 - '0');
            src[dst_idx++] = static_cast<char>(val);
            src_idx += 2;
        } else if (src[src_idx] == '+') {
            src[dst_idx++] = ' ';
        } else {
            if (dst_idx != src_idx) src[dst_idx] = src[src_idx];
            dst_idx++;
        }
    }
    src.resize(dst_idx);
}

// 2. 高效解析HTTP请求
void parse_http_request(const char* data, size_t len, HttpRequest& req, bool parse_cookies) {
    const char* end = data + len;
    const char* ptr = data;

    // 解析请求行（避免字符串流）
    while (ptr < end && *ptr != ' ') req.method += *ptr++;
    while (ptr < end && *ptr == ' ') ptr++; // 跳过空格
    while (ptr < end && *ptr != ' ') req.uri += *ptr++;
    while (ptr < end && *ptr == ' ') ptr++;
    while (ptr < end && *ptr != '\r') req.protocol += *ptr++;
    if (!req.protocol.empty() && req.protocol.size() > 5) {
        req.protocol.erase(0, 5); // 原地移除"HTTP/"
    }

    // 解析头部（零拷贝+预分配）
    ptr += 2; // 跳过"\r\n"
    req.headers.reserve(20); // 预分配典型头部数量
    while (ptr < end - 2) {
        const char* colon = std::find(ptr, end, ':');
        if (colon == end) break;

        std::string key(ptr, colon);
        std::transform(key.begin(), key.end(), key.begin(), 
                       [](char c) { return std::tolower(c); });

        const char* val_start = colon + 1;
        while (val_start < end && (*val_start == ' ' || *val_start == '\t')) val_start++;
        const char* val_end = std::find(val_start, end, '\r');
        std::string value(val_start, val_end);
        if(key == "host") {
            req.host = value;
        }
        else if(key == "content-type") {
            req.content_type = value;
        }
        req.headers.emplace_back(key, value);
        ptr = val_end + 2; // 跳过"\r\n"
        if (ptr < end && *ptr == '\r') break; // 空行检测
    }

    // 3. 查询参数解析（批量解码+避免流）
    size_t query_start = 0;
    while (query_start < req.uri.size() && req.uri[query_start] != '?') query_start++;
    if (query_start++ < req.uri.size()) {
        const char* query_str = req.uri.data() + query_start;
        size_t query_len = req.uri.size() - query_start;
        std::string query_buf(query_str, query_len);
        url_decode_inplace(query_buf); // 批量解码整段查询字符串

        const char* qptr = query_buf.data();
        const char* qend = qptr + query_buf.size();
        while (qptr < qend) {
            const char* amp = std::find(qptr, qend, '&');
            const char* eq = std::find(qptr, amp, '=');
            std::string key(qptr, eq);
            std::string val(eq + 1, amp);
            req.query.emplace_back(std::move(key), std::move(val));
            qptr = amp + (amp != qend);
        }
    }

    // 4. Cookie解析（按需触发）
    if (parse_cookies) {
        for (const auto& [k, v] : req.headers) {
            if (k == "cookie") {
                std::string cookie_buf = v;
                url_decode_inplace(cookie_buf); // 批量解码

                const char* cptr = cookie_buf.data();
                const char* cend = cptr + cookie_buf.size();
                while (cptr < cend) {
                    while (cptr < cend && std::isspace(*cptr)) cptr++;
                    const char* semi = std::find(cptr, cend, ';');
                    const char* eq = std::find(cptr, semi, '=');
                    if (eq != semi) {
                        req.cookies.emplace_back(
                            std::string(cptr, eq),
                            std::string(eq + 1, semi)
                        );
                    }
                    cptr = semi + (semi != cend);
                }
                break;
            }
        }
    }
}

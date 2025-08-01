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


// URL解码函数（参考网页[9][10]）
std::string url_decode(const std::string &src) {
    std::string decoded;
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '%' && i + 2 < src.size()) {
            int hex_val;
            std::istringstream hex_stream(src.substr(i+1, 2));
            if (hex_stream >> std::hex >> hex_val) {
                decoded += static_cast<char>(hex_val);
                i += 2;
            }
        } else if (src[i] == '+') {
            decoded += ' ';
        } else {
            decoded += src[i];
        }
    }
    return decoded;
}

// 解析HTTP请求（参考网页[7][11]的握手处理）
void parse_http_request(const std::string &raw_request, HttpRequest& req, bool parse_cookies)
{
    std::istringstream stream(raw_request);
    std::string line;

    // 解析请求行
    if (std::getline(stream, line)) {
        std::istringstream line_stream(line);
        line_stream >> req.method >> req.uri >> req.protocol;
        req.protocol = req.protocol.substr(5); // 去除"HTTP/"
    }

    // 解析请求头
    while (std::getline(stream, line) && line != "\r") {
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            std::string value = line.substr(colon_pos + 2); // 跳过": "
            value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
            req.headers[key] = value;
        }
    }

    // 解析QUERY_STRING（参考网页[9]的URL参数处理）
    size_t query_start = req.uri.find('?');
    if (query_start != std::string::npos) {
        std::string query_str = req.uri.substr(query_start + 1);
        std::istringstream query_stream(query_str);
        std::string pair;
        while (std::getline(query_stream, pair, '&')) {
            size_t eq_pos = pair.find('=');
            std::string key = (eq_pos != std::string::npos) ? 
                url_decode(pair.substr(0, eq_pos)) : url_decode(pair);
            std::string value = (eq_pos != std::string::npos) ? 
                url_decode(pair.substr(eq_pos + 1)) : "";
            req.query[key] = value;
        }
    }

    // 新增：解析 Cookies（需在请求头解析完成后添加）
    if (req.headers.find("cookie") != req.headers.end()) {
        std::string cookieStr = req.headers["cookie"];
        std::istringstream cookieStream(cookieStr);
        std::string cookiePair;

        while (std::getline(cookieStream, cookiePair, ';')) {
            // 去除首尾空格（网页4提到的清理逻辑）
            cookiePair.erase(cookiePair.begin(), 
                std::find_if(cookiePair.begin(), cookiePair.end(), 
                    [](int ch) { return !std::isspace(ch); }));
            cookiePair.erase(std::find_if(cookiePair.rbegin(), cookiePair.rend(),
                [](int ch) { return !std::isspace(ch); }).base(), cookiePair.end());

            // 分割键值对（类似查询参数处理）
            size_t eqPos = cookiePair.find('=');
            if (eqPos != std::string::npos) {
                std::string key = url_decode(cookiePair.substr(0, eqPos));
                std::string value = url_decode(
                    cookiePair.substr(eqPos + 1)
                );
                req.cookies[key] = value;  // 需在 HttpRequest 结构体中定义 cookies 成员
            }
        }
    }
}

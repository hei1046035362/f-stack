#include <iostream>
#include <regex>
#include <sys/time.h>
#include "common.hpp"
#include <iomanip>
#include <string.h>
#include "log.hpp"

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
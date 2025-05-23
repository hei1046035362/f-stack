#include <iostream>
#include <regex>
#include <sys/time.h>
#include "common.hpp"
#include <iomanip>

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

std::string hex2bin(const std::string& hex)
{
    if (hex.length() % 2 != 0) {
        printf("[%s][%d] Hex string must have an even length.", 
            __FILE__, __LINE__);
        return "";
    }
    std::string binary;
    for (size_t i = 0; i < hex.length(); i += 2) {
        // 提取两个字符
        std::string byteString = hex.substr(i, 2);
        // 转换成整数
        char byte = static_cast<char>(strtol(byteString.c_str(), nullptr, 16));
        binary.push_back(byte); // 添加到结果字符串
    }

    return binary;
}

std::string bin2hex(const std::string& input)
{
    std::stringstream ss;
    for (unsigned char c : input) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    return ss.str();
}

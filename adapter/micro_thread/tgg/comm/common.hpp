#pragma once
#include <iostream>
#include <string>
#include <vector>
static std::string tgg_trim(const std::string& str) {
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
        int port = std::stoi(match[7].str());
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
    std::memset(buffer, 0, sizeof(buffer));
    sprintf(buffer, "%08X", ip);
    return std::string(buffer);
}
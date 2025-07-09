#pragma once
#include <iostream>
#include <string>
#include <cstring>
#include <arpa/inet.h>
#include "tgg_struct.h"
#include "cmd/GatewayProtocal.h"
#include "comm/Serialize.hpp"
#include <type_traits>
#include "comm/common.hpp"

template<typename T, typename = typename std::enable_if<
    std::is_same<T, bool>::value ||
    std::is_integral<T>::value ||
    std::is_floating_point<T>::value ||
    std::is_convertible<T, std::string>::value
    >::type>
constexpr bool is_scalar()
{
    return true;
}

template<typename T>
constexpr bool is_scalar()
{
    return false;
}

class BwPackageHandler {
public:
    // 获取整个包的buffer，对应encode函数   外部需要填充bwdata->flag和bwdata->cmd 两个字段，ip和port默认都是不填的，可选
    static void encode(std::string &result, tgg_bw_protocal* bwdata, 
        const std::string& body, const std::string& extend_data = "")
     {
        bwdata->ext_len = extend_data.length() > 0 ? extend_data.length() : 0;
        bwdata->pack_len = sizeof(tgg_bw_protocal) + bwdata->ext_len + body.size();

        result.resize(bwdata->pack_len);
        // 拼接ext_data和body
        if (bwdata->ext_len > 0) {
            result.replace(sizeof(tgg_bw_protocal), bwdata->ext_len, extend_data);
        }
        result.replace(sizeof(tgg_bw_protocal) + bwdata->ext_len, body.size(), body);

        if(big_endian()) {
            bwdata->pack_len = htonl(bwdata->pack_len);
            bwdata->local_port = htons(bwdata->local_port);// 本地端口是从文件中读取的，需要转换成网络字节序
            bwdata->connection_id = htonl(bwdata->connection_id);
            bwdata->gateway_port = htons(bwdata->gateway_port);
            bwdata->ext_len = htonl(bwdata->ext_len);
        }
        std::memcpy(&result[0], bwdata, sizeof(tgg_bw_protocal));
    }

    // 从二进制数据转换为数组，对应decode函数
static void decode(tgg_bw_protocal* bwdata, rapidjson::Document& bwjdata) {
    // 确保bwjdata是对象类型
    bwjdata.SetObject();
    rapidjson::Document::AllocatorType& allocator = bwjdata.GetAllocator();
    
    // 1. 处理flag字段
    bwjdata.AddMember("flag", static_cast<int>(bwdata->flag), allocator);
    
    // 2. 字节序转换（保持原逻辑）
    if(big_endian()) {
        bwdata->ext_len = htonl(bwdata->ext_len);
        bwdata->pack_len = htonl(bwdata->pack_len);
    }
    
    // 3. 处理body字段（存储指针地址）
    int body_len = bwdata->pack_len - bwdata->ext_len - sizeof(tgg_bw_protocal);
    if(body_len > 0) {
        // 存储二进制数据的指针地址（替代nlohmann的指针存储）
        bwjdata.AddMember("body", 
                         reinterpret_cast<uintptr_t>(bwdata->data + bwdata->ext_len), 
                         allocator);
        bwjdata.AddMember("body_len", body_len, allocator);
    } else {
        bwjdata.AddMember("body", reinterpret_cast<uintptr_t>(nullptr), allocator);
        bwjdata.AddMember("body_len", 0, allocator);
    }
    
    // 4. 处理ext_data字段（深拷贝字符串）
    if(bwdata->ext_len > 0) {
        // std::string ext_data(bwdata->data, bwdata->ext_len);
        // 使用深拷贝避免悬空指针
        bwjdata.AddMember("ext_data", rapidjson::Value().SetString(bwdata->data, bwdata->ext_len, allocator), allocator);
    } else {
        bwjdata.AddMember("ext_data", "", allocator);
    }
    
    // 5. 添加基础字段
    bwjdata.AddMember("pack_len", static_cast<unsigned int>(bwdata->pack_len), allocator);
    bwjdata.AddMember("cmd", static_cast<int>(bwdata->cmd), allocator);
    
    // 6. IP地址转换（保持原逻辑）
    unsigned int local_ip = bwdata->local_ip;
    unsigned int client_ip = bwdata->client_ip;
    bwjdata.AddMember("local_ip", 
                     rapidjson::StringRef(inet_ntoa(*reinterpret_cast<in_addr*>(&local_ip))),
                     allocator);
    bwjdata.AddMember("client_ip", 
                     rapidjson::StringRef(inet_ntoa(*reinterpret_cast<in_addr*>(&client_ip))),
                     allocator);
    
    // 7. 端口处理（字节序转换）
    if(big_endian()) {
        bwjdata.AddMember("local_port", ntohs(bwdata->local_port), allocator);
        bwjdata.AddMember("client_port", ntohs(bwdata->client_port), allocator);
        bwjdata.AddMember("connection_id", ntohl(bwdata->connection_id), allocator);
        bwjdata.AddMember("gateway_port", ntohs(bwdata->gateway_port), allocator);
    } else {
        bwjdata.AddMember("local_port", static_cast<unsigned short>(bwdata->local_port), allocator);
        bwjdata.AddMember("client_port", static_cast<unsigned short>(bwdata->client_port), allocator);
        bwjdata.AddMember("connection_id", static_cast<unsigned int>(bwdata->connection_id), allocator);
        bwjdata.AddMember("gateway_port", static_cast<unsigned int>(bwdata->gateway_port), allocator);
    }
}

private:
    // 简单模拟序列化，这里只是将字符串包裹在特定格式中，实际可能需要更完善的序列化逻辑
    static std::string serialize(const std::string& str) {
        return "S:" + std::to_string(str.size()) + ":\"" + str + "\";";
    }
    // 简单模拟反序列化，去除模拟序列化时添加的格式字符
    static std::string deserialize(const std::string& str) {
        size_t start = str.find(':') + 1;
        size_t end = str.find(':', start);
        size_t len = std::stoi(str.substr(start, end - start));
        return str.substr(end + 2, len);
    }
};
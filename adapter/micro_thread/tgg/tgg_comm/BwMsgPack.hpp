#include <iostream>
#include <string>
#include <cstring>
#include <arpa/inet.h>
#include "tgg_struct.h"
#include "cmd/GatewayProtocal.h"
#include "comm/Serialize.hpp"
#include <type_traits>

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
    static void encode(std::string &result, const tgg_bw_protocal* bwdata, 
        const std::string& body, const std::string& extend_data = "")
     {
        // bwdata->flag = (body.find_first_not_of("0123456789") == std::string::npos);
        // std::string encode_body = body;
        // if (!bwdata->flag) {
        //     // 这里简单模拟序列化，实际可能需要更复杂处理
        //     encode_body = serialize(body);
        // }
        bwdata->ext_len = extend_data.length() > 0 ? extend_data.length() : 0;
        bwdata->pack_len = sizeof(tgg_bw_protocal) + bwdata->ext_len + body.size();

        result.resize(bwdata->pack_len);
        // 拼接ext_data和body
        if (bwdata->ext_len > 0) {
            result.replace(sizeof(tgg_bw_protocal), bwdata->ext_len, extend_data.c_str());
        }
        result.replace(sizeof(tgg_bw_protocal) + bwdata->ext_len, body.size(), body.c_str());

        // bwdata->local_ip = inet_addr(local_ip.c_str());
        // bwdata->client_ip = inet_addr(client_ip.c_str());
        if(big_endian()) {
            bwdata->local_ip = htonl(bwdata->local_ip);
            bwdata->client_ip = htonl(bwdata->client_ip);
            bwdata->pack_len = htonl(bwdata->pack_len);
            bwdata->local_port = htons(bwdata->local_port);
            bwdata->client_port = htons(bwdata->client_port);
            bwdata->connection_id = htonl(bwdata->connection_id);
            bwdata->gateway_port = htons(bwdata->gateway_port);
            bwdata->ext_len = htonl(bwdata->ext_len);
        }
        std::memcpy(&result[0], bwdata, sizeof(tgg_bw_protocal));
        return result;
    }

    // 从二进制数据转换为数组，对应decode函数
    static void decode(tgg_bw_protocal* bwdata, nlohmann::json& bwjdata) {
        bwjdata["flag"] = (int)(bwdata->flag);
        if(big_endian()) {
            bwdata->ext_len = htonl(bwdata->ext_len);
            bwdata->pack_len = htonl(bwdata->pack_len);
        }
        // bwjdata = nlohmann::json::parse(body)
        int body_len = bwdata->pack_len - bwdata->ext_len - sizeof(tgg_bw_protocal);
        if(body_len > 0) {
            std::string body(bwdata->data + bwdata->ext_len, // body的起始位置
                             body_len); // body的长度
            if (bwdata->flag & FLAG_BODY_IS_SCALAR) {
                bwjdata["body"] = body.c_str();
            } else {
                // 这里简单模拟反序列化，实际可能需要更安全可靠的处理
                bwjdata["body"] = deserialize(body).c_str();
            }
        } else {
            bwjdata["body"] = "";
        }
        if(bwdata->ext_len > 0) {
            std::string ext_data(bwdata->data, bwdata->ext_len);
            bwjdata["ext_data"] = ext_data.c_str();
        } else {
            bwjdata["ext_data"] = "";
        }

        bwjdata["pack_len"] = (unsigned int)bwdata->pack_len;
        bwjdata["cmd"] = (int)bwdata->cmd;
        unsigned int local_ip = bwdata->local_ip;
        unsigned int client_ip = bwdata->client_ip;
        bwjdata["local_ip"] = inet_ntoa(*reinterpret_cast<in_addr*>(&local_ip));
        bwjdata["client_ip"] = inet_ntoa(*reinterpret_cast<in_addr*>(&client_ip));
        if(big_endian()) {
            bwjdata["local_port"] = htons(bwdata->local_port);
            bwjdata["client_port"] = htons(bwdata->client_port);
            bwjdata["connection_id"] = htonl(bwdata->connection_id);
            bwjdata["gateway_port"] = htons(bwdata->gateway_port);
        } else {
            unsigned short local_port = bwdata->local_port;
            unsigned short client_port = bwdata->client_port;
            unsigned int connection_id = bwdata->connection_id;
            unsigned int gateway_port = bwdata->gateway_port;
            bwjdata["local_port"] = local_port;
            bwjdata["client_port"] = client_port;
            bwjdata["connection_id"] = connection_id;
            bwjdata["gateway_port"] = gateway_port;
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
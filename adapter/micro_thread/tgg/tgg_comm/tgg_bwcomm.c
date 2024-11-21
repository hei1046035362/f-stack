#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <zlib.h>
#include <rte_log.h>
#include <rte_mempool.h>
#include "tgg_bwcomm.h"
#include <map>
#include <iostream>
#include "string.h"
#include "nlohmann/json.hpp"
#include "comm/Encrypt.hpp"
#include "comm/RedisClient.hpp"
#include "tgg_comm/tgg_bw_cache.h"

std::map<int, tgg_bw_info*> g_map_bwinfo;

static bool s_big_endian = false;

union EndiannessTester {
    int integer;
    char bytes[sizeof(int)];
};

void init_endians()
{
    union EndiannessTester tester;
    tester.integer = 1;
    if (tester.bytes[0] == 1) {
        s_big_endian = true;
    } 
}

bool big_endian()
{
    return s_big_endian;
}


int get_connection_info(int fd, const char* ip, unsigned short* port)
{
     // 获取IP地址信息
     struct sockaddr_in remote_addr;
     socklen_t addrlen = sizeof(remote_addr);
     if (getpeername(fd, (struct sockaddr *)&remote_addr, &addrlen) == -1) {
         perror("getpeername");
         close(fd);
         return -1;
     }
     inet_ntop(AF_INET, &(remote_addr.sin_addr), (char*)ip, INET_ADDRSTRLEN);
     *port = ntohs(remote_addr.sin_port);
     RTE_LOG(INFO, USER1, "[%s][%d] Cmd from [%s]:[%d]", __func__, __LINE__, ip, *port);
     return 0;
}

tgg_bw_info* lookup_bwinfo(int fd)
{
    tgg_bw_info* bwinfo = NULL;
    // 如果之前已经存在了这个fd，说明可能fd被重用了，之前的信息要重置
    std::map<int, tgg_bw_info*>::iterator it = g_map_bwinfo.find(fd);
    if (it != g_map_bwinfo.end()) {
        bwinfo = it->second;
    } 
    return bwinfo;
}

tgg_bw_info* get_valid_bwinfo_by_fd(int fd)
{
    tgg_bw_info* bwinfo = lookup_bwinfo(fd);
    if (!bwinfo) {
        bwinfo = new(tgg_bw_info);
    }
    return bwinfo;
}




// 将输入字符串使用 DEFLATE 压缩
std::string gzdeflate(const std::string& input) {
    // 预计最大的压缩后缓冲区大小，这里简单地设置为输入字符串大小的两倍（可根据实际情况调整）
    uLongf outBufferSize = input.length() * 2;

    // 分配压缩后的输出缓冲区
    std::string outBuffer;
    outBuffer.resize(outBufferSize);

    // 调用deflate函数进行压缩
    z_stream deflateStream;
    deflateStream.zalloc = Z_NULL;
    deflateStream.zfree = Z_NULL;
    deflateStream.opaque = Z_NULL;

    // 初始化deflate流
    if (deflateInit2(&deflateStream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY)!= Z_OK) {
        RTE_LOG(ERR, USER1, "[%s][%d] deflate init Error.", __func__, __LINE__);
        return "";
    }

    deflateStream.avail_in = input.length();
    deflateStream.next_in = (Bytef*)input.c_str();
    deflateStream.avail_out = outBufferSize;
    deflateStream.next_out = (Bytef*)outBuffer.c_str();

    // 执行压缩操作
    if (deflate(&deflateStream, Z_FINISH)!= Z_STREAM_END) {
        deflateEnd(&deflateStream);
        RTE_LOG(ERR, USER1, "[%s][%d] deflate failed.", __func__, __LINE__);
        return "";
    }

    // 获取实际压缩后的大小
    uLongf actualOutSize = deflateStream.total_out;

    if(outBufferSize < deflateStream.total_out) {
        RTE_LOG(ERR, USER1, "[%s][%d] Less of reserved buffer length:%lu, total:%lu.", __func__, __LINE__, 
            outBufferSize, deflateStream.total_out);
        return "";
    }
    outBuffer.resize(actualOutSize);
    // 结束deflate流并释放相关资源
    deflateEnd(&deflateStream);

    // 将压缩后的数据转换为字符串并返回
    return outBuffer;
}

std::string gzinflate(const std::string& input)
{
    // 预计最大的解压后缓冲区大小，这里设置为输入字符串大小的10倍（可根据实际情况调整）
    uLongf outBufferSize = input.length() * 2;

    // 分配解压后的输出缓冲区
    std::string outBuffer;
    outBuffer.resize(outBufferSize);

    // 调用inflate函数进行解压
    z_stream inflateStream;
    inflateStream.zalloc = Z_NULL;
    inflateStream.zfree = Z_NULL;
    inflateStream.opaque = Z_NULL;

    // 初始化inflate流
    if (inflateInit2(&inflateStream, -MAX_WBITS)!= Z_OK) {
        RTE_LOG(ERR, USER1, "[%s][%d] inflate init Error.", __func__, __LINE__);
        return "";
    }

    inflateStream.avail_in = input.length();
    inflateStream.next_in = (Bytef*)input.c_str();
    inflateStream.avail_out = outBufferSize;
    inflateStream.next_out = (Bytef*)outBuffer.c_str();

    // 执行解压操作
    if (inflate(&inflateStream, Z_FINISH)!= Z_STREAM_END) {
        inflateEnd(&inflateStream);
        RTE_LOG(ERR, USER1, "[%s][%d] inflate failed.", __func__, __LINE__);
        return "";
    }

    // 获取实际解压后的大小
    uLongf actualOutSize = inflateStream.total_out;
    if(outBufferSize < inflateStream.total_out) {
        RTE_LOG(ERR, USER1, "[%s][%d] Less of reserved buffer length:%lu, total:%lu.", __func__, __LINE__, 
            outBufferSize, inflateStream.total_out);
        return "";
    }
    // 结束inflate流并释放相关资源
    inflateEnd(&inflateStream);
    outBuffer.resize(actualOutSize);
    // 将解压后的数据转换为字符串并返回
    return outBuffer;
}

std::string compress(int compressFormat, const std::string& body) {
    // 具体的压缩逻辑应根据需求实现
    if (!compressFormat) {
        return body;
    }
    return gzdeflate(body);
}

std::string decompress(int compressFormat, const std::string& body)
{
    // 具体的压缩逻辑应根据需求实现
    if (!compressFormat) {
        return body;
    }
    return gzinflate(body);
}

std::string message_pack(uint16_t command, uint32_t seq, uint8_t protocol,
            uint8_t compressFormat, const std::string& body)
{    
    const uint16_t PACKAGE_SEPARATOR = 65534; // 假设的包分隔符
    std::vector<uint8_t> output;

    // 添加包分隔符
    output.push_back(PACKAGE_SEPARATOR >> 8);
    output.push_back(PACKAGE_SEPARATOR & 0xFF);

    // 添加未使用
    output.push_back(0); 
    output.push_back(0); 

    // 包长度（稍后填充）
    output.push_back(0);
    output.push_back(0);
    output.push_back(0);
    output.push_back(0);

    // 额外大小
    output.push_back(0);
    output.push_back(0);

    // 添加命令、序号和版本信息
    output.push_back(command >> 8);
    output.push_back(command & 0xFF);

    output.push_back(seq >> 24);
    output.push_back((seq >> 16) & 0xFF);
    output.push_back((seq >> 8) & 0xFF);
    output.push_back(seq & 0xFF);

    output.push_back(0);
    output.push_back(1); // 版本

    output.push_back(protocol);
    output.push_back(compressFormat);

    // 压缩主体
    auto compressedBody = compress(compressFormat, body);
    if (compressedBody.empty()) {
        return ""; // 假设压缩失败返回空
    }
    
    output.insert(output.end(), compressedBody.begin(), compressedBody.end());

    // 计算并设置包长度
    uint32_t packageLength = output.size();
    output[4] = (packageLength >> 24) & 0xFF;
    output[5] = (packageLength >> 16) & 0xFF;
    output[6] = (packageLength >> 8) & 0xFF;
    output[7] = packageLength & 0xFF;
    return std::string(output.begin(), output.end());
}

std::string message_unpack(const std::string& packedData)
{
    std::string result;
    const unsigned int PACKAGE_SEPARATOR = 65534;

    // 验证包分隔符
    unsigned short package_separator_check;
    memcpy(&package_separator_check, packedData.c_str(), 2);
    if(big_endian()) {
        package_separator_check = ntohs(package_separator_check);
    }
    if (package_separator_check!= PACKAGE_SEPARATOR) {
        return result; // 包分隔符不匹配，返回空结果向量
    }

    // 读取包长度
    unsigned int packageLength;
    memcpy(&packageLength, packedData.c_str() + 4, 4);
    if(big_endian()) {
        packageLength = ntohl(packageLength);
    }
    if (packedData.length()!= packageLength) {
        return result; // 包长度不匹配，返回空结果向量
    }

    // 读取并解析剩余部分
    size_t offset = 10; // 跳过包分隔符、未使用字段、包长度和额外大小

    unsigned short command;
    memcpy(&command, packedData.c_str() + offset, 2);
    if(big_endian()) {
        command = ntohs(command);
    }
    offset += 2;

    unsigned int seq;
    memcpy(&seq, packedData.c_str() + offset, 4);
    if(big_endian()) {
        seq = ntohl(seq);
    }
    offset += 4;

    unsigned short version;
    memcpy(&version, packedData.c_str() + offset, 2);
    if(big_endian()) {
        version = ntohs(version);
    }
    offset += 2;

    unsigned char protocol;
    memcpy(&protocol, packedData.c_str() + offset, 1);
    offset += 1;

    unsigned char compressFormat;
    memcpy(&compressFormat, packedData.c_str() + offset, 1);
    offset += 1;

    // 读取压缩后的正文
    std::string compressedBody = packedData.substr(offset);

    // 解压缩正文（调用假设存在的decompress函数）
    std::string body = decompress(compressFormat, compressedBody);
    if (body.empty()) {
        return result; // 解压缩失败，返回空结果向量
    }
    nlohmann::json jdata; 

    // 将解析结果整理到结果向量中
    jdata["cmd"] = command;
    jdata["seq"] = seq;
    jdata["version"] = version;
    jdata["compressFormat"] = compressFormat;
    jdata["body"] = body;
    result = jdata.dump();

    return result;
}

int tgg_init_uidgid(const std::vector<std::string>& clusterNodes, const std::string& password, const std::string& userName)
{
    std::map<std::string, std::set<std::string>> mapUsers;
    int ret = GetUserWithGids(clusterNodes, mapUsers, password);
    if (ret < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d] Connect redis failed.", __func__, __LINE__);
        return ret;
    }

    std::map<std::string, std::set<std::string>>::iterator itUid = mapUsers.begin();
    while(itUid != mapUsers.end()) {
        std::set<std::string>::iterator itGid = itUid->second.begin();
        while(itGid != itUid->second.end()) {
            std::cout << "start add userId[" << itUid->first << "]: Gid[" << *itGid << "]" << std::endl;
            tgg_add_uidgid(itUid->first.c_str(), (*itGid).c_str());
            std::cout << "end add userId[" << itUid->first << "]: Gid[" << *itGid << "]" << std::endl;
            itGid++;
        }
        itUid++;
    }
    return 0;
}

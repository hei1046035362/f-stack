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
#include <set>
#include <iostream>
#include <mutex>
#include <atomic>
#include "string.h"
#include "comm/Encrypt.hpp"
#include "tgg_comm/tgg_bw_cache.h"
#include "tgg_comm/tgg_common.h"
#include "comm/log.hpp"
#include "comm/common.hpp"

#ifndef MAX_FD_COUNT
#define MAX_FD_COUNT 100000
#endif

int get_connection_info(int fd, char* ip_str, unsigned int* ip, unsigned short* port)
{
     // 获取IP地址信息
     struct sockaddr_in remote_addr;
     socklen_t addrlen = sizeof(remote_addr);
     if (getpeername(fd, (struct sockaddr *)&remote_addr, &addrlen) == -1) {
         perror("getsockname");
         // close(fd);
         return -1;
     }
     inet_ntop(AF_INET, &(remote_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
     *port = remote_addr.sin_port;// 网络字节序
     *ip = remote_addr.sin_addr.s_addr;// 网络字节序
     return 0;
}

// 将输入字符串使用 DEFLATE 压缩
int gzdeflate(std::string_view input, std::string& outBuffer) {
    if(!input.length()) {
        // 输入为空时，deflate会失败，但是php那边是可以压缩的，值为 \x03\x00
        outBuffer = "\x03\x00";
        outBuffer.resize(2);// 不resize,会导致外部取数据的时候把\x00忽略掉
        return 0;
    }
    // 预计最大的压缩后缓冲区大小，这里简单地设置为输入字符串大小的两倍（可根据实际情况调整）
    uLongf outBufferSize = input.length() * 2;

    // 分配压缩后的输出缓冲区
    outBuffer.resize(outBufferSize);

    // 调用deflate函数进行压缩
    z_stream deflateStream;
    deflateStream.zalloc = Z_NULL;
    deflateStream.zfree = Z_NULL;
    deflateStream.opaque = Z_NULL;

    // 初始化deflate流
    if (deflateInit2(&deflateStream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY)!= Z_OK) {
        LOG_ERROR("deflate init Error.");
        return -1;
    }

    deflateStream.avail_in = input.length();
    deflateStream.next_in = (Bytef*)input.data();
    deflateStream.avail_out = outBufferSize;
    deflateStream.next_out = (Bytef*)outBuffer.c_str();

    // 执行压缩操作
    if (deflate(&deflateStream, Z_FINISH)!= Z_STREAM_END) {
        deflateEnd(&deflateStream);
        LOG_ERROR("deflate failed.");
        return -1;
    }

    // 获取实际压缩后的大小
    uLongf actualOutSize = deflateStream.total_out;

    if(outBufferSize < deflateStream.total_out) {
        LOG_ERROR("Less of reserved buffer length:%lu, total:%lu.", outBufferSize, deflateStream.total_out);
        return -1;
    }
    outBuffer.resize(actualOutSize);
    // 结束deflate流并释放相关资源
    deflateEnd(&deflateStream);

    // 将压缩后的数据转换为字符串并返回
    return 0;
}

int gzinflate(const char* input, size_t input_len, char* outBuffer)
{
    // 预计最大的解压后缓冲区大小，这里设置为输入字符串大小的10倍（可根据实际情况调整）
    uLongf outBufferSize = input_len * 2;

    // 分配解压后的输出缓冲区
    // std::string outBuffer;
    // outBuffer.resize(outBufferSize);

    // 调用inflate函数进行解压
    z_stream inflateStream;
    inflateStream.zalloc = Z_NULL;
    inflateStream.zfree = Z_NULL;
    inflateStream.opaque = Z_NULL;

    // 初始化inflate流
    if (inflateInit2(&inflateStream, -MAX_WBITS)!= Z_OK) {
        LOG_ERROR("inflate init Error.");
        return -1;
    }

    inflateStream.avail_in = input_len;
    inflateStream.next_in = (z_const Bytef*)input;
    inflateStream.avail_out = outBufferSize;
    inflateStream.next_out = (Bytef*)outBuffer;

    // 执行解压操作
    if (inflate(&inflateStream, Z_FINISH)!= Z_STREAM_END) {
        inflateEnd(&inflateStream);
        LOG_ERROR("inflate failed, input:%s.", bin2hex(std::string_view(input, input_len)).c_str());
        return -1;
    }

    // 获取实际解压后的大小
    uLongf actualOutSize = inflateStream.total_out;
    if(outBufferSize < inflateStream.total_out) {
        LOG_ERROR("Less of reserved buffer length:%lu, total:%lu.", outBufferSize, inflateStream.total_out);
        return -1;
    }
    // 结束inflate流并释放相关资源
    inflateEnd(&inflateStream);
    outBuffer[actualOutSize] = '\0';
    // 将解压后的数据转换为字符串并返回
    return 0;
}

int message_pack(uint16_t command, uint32_t seq, uint8_t protocol,
            uint8_t compressFormat, std::string_view body, std::string& result)
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

    std::string compressedBody;
    // 压缩主体
    if (compressFormat) {
        if (gzdeflate(body, compressedBody) < 0) {
            return -1;
        }
    } else {
        compressedBody = body;
    }
    
    output.insert(output.end(), compressedBody.begin(), compressedBody.end());

    // 计算并设置包长度
    uint32_t packageLength = output.size();
    output[4] = (packageLength >> 24) & 0xFF;
    output[5] = (packageLength >> 16) & 0xFF;
    output[6] = (packageLength >> 8) & 0xFF;
    output[7] = packageLength & 0xFF;
    result = std::string(output.begin(), output.end());
    return 0;
}

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

// 辅助函数：将 rapidjson::Value 转换为字符串
static std::string rapidjson_to_string(const rapidjson::Value& val)
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    val.Accept(writer);
    return buffer.GetString();
}

int message_unpack(const char* packedData, size_t packedData_len, std::string& result)
{
    const unsigned int PACKAGE_SEPARATOR = 65534;

    // 验证包分隔符
    unsigned short package_separator_check;
    memcpy(&package_separator_check, packedData, 2);
    if(big_endian()) {
        package_separator_check = ntohs(package_separator_check);
    }
    if (package_separator_check!= PACKAGE_SEPARATOR) {
        return -1; // 包分隔符不匹配，返回空结果向量
    }

    // 读取包长度
    unsigned int packageLength;
    memcpy(&packageLength, packedData + 4, 4);
    if(big_endian()) {
        packageLength = ntohl(packageLength);
    }
    if (packedData_len!= packageLength) {
        return -1; // 包长度不匹配，返回空结果向量
    }

    // 读取并解析剩余部分
    size_t offset = 10; // 跳过包分隔符、未使用字段、包长度和额外大小

    unsigned short command;
    memcpy(&command, packedData + offset, 2);
    if(big_endian()) {
        command = ntohs(command);
    }
    offset += 2;

    unsigned int seq;
    memcpy(&seq, packedData + offset, 4);
    if(big_endian()) {
        seq = ntohl(seq);
    }
    offset += 4;

    unsigned short version;
    memcpy(&version, packedData + offset, 2);
    if(big_endian()) {
        version = ntohs(version);
    }
    offset += 2;

    unsigned char protocol;
    memcpy(&protocol, packedData + offset, 1);
    offset += 1;

    unsigned char compressFormat;
    memcpy(&compressFormat, packedData + offset, 1);
    offset += 1;

    // 读取压缩后的正文
    const char* compressedBody = packedData + offset;
    char body[4096] = {0};
    // 解压缩正文（调用假设存在的decompress函数）
    if (compressFormat) {
        if (gzinflate(compressedBody, packedData_len - offset, body) < 0) {
            return -1; // 解压缩失败，返回空结果向量
        }
    }
    // } else {
    //     memcpy(body, compressedBody, packedData_len - offset);
    // }

    // 结果存储成json
    rapidjson::Document jdata;
    jdata.SetObject();
    rapidjson::Document::AllocatorType& allocator = jdata.GetAllocator();
    
    // 添加字段（需显式管理内存）
    jdata.AddMember("cmd", 
                   rapidjson::Value().SetInt(command), 
                   allocator);
    jdata.AddMember("seq", 
                   rapidjson::Value().SetUint(seq), 
                   allocator);
    jdata.AddMember("version", 
                   rapidjson::Value().SetInt(version), 
                   allocator);
    jdata.AddMember("compressFormat", 
                   rapidjson::Value().SetInt(compressFormat), 
                   allocator);
    
    // 处理二进制数据（假设bin2hex返回std::string）
    std::string hexBody = bin2hex(compressFormat ? body : compressedBody, false);
    jdata.AddMember("body", 
                   rapidjson::Value().SetString(hexBody.c_str(), hexBody.size(), allocator).Move(), 
                   allocator);
    result = rapidjson_to_string(jdata);
    return 0;
}

#include <fstream>
#include <vector>
#include <sstream>
#include <stdexcept>
#include <type_traits>

template <typename T>
int write_list_to_file_inline(const std::string& filename, 
                     const std::string& header,
                     const std::vector<T>& dataList,
                     size_t buffer_kb) 
{
    // 1. 创建输出流并设置缓冲区
    std::ofstream out_file;
    std::string buffer;
    buffer.reserve(buffer_kb * 1024);  // 预分配缓冲区
    if(!header.empty()) {
        buffer.append(header);
        buffer.append("\n");
    }
    try {
        // 2. 打开文件（异常安全）
        out_file.open(filename);
        if (!out_file.is_open()) {
            LOG_ERROR("Failed to open file: %s", filename.c_str());
            return -1;
        }

        // 3. 遍历列表并构建缓冲区
        for (const auto& item : dataList) {
            std::ostringstream oss;
            oss << item;
            // 类型特化处理（避免额外开销）
            // if constexpr (std::is_integral_v<T>) {
            //     oss << item;  // 直接写入整型
            // } else {
            //     oss << item;  // 依赖类型重载的<<操作符
            // }
            
            // 添加换行符
            oss << '\n';
            
            // 检查缓冲区容量
            if (buffer.size() + oss.str().size() > buffer.capacity()) {
                out_file << buffer;  // 批量写入
                buffer.clear();
            }
            buffer += oss.str();  // 添加到缓冲区
        }

        // 4. 写入剩余数据
        if (!buffer.empty()) {
            out_file << buffer;
        }
    } catch (...) {
        if (out_file.is_open()) out_file.close();  // 异常时关闭文件
        LOG_ERROR("writeListToFile [%s] failed, catched an exception.", filename.c_str());
        return -1;
    }
    // 5. RAII自动关闭文件
    return 0;
}

template <typename T>
int write_list_to_file(const std::string& filename, 
                     const std::string& header,
                     const std::vector<T>& dataList,
                     size_t buffer_kb)
{
    return write_list_to_file_inline(filename, header, dataList, buffer_kb);
}

template <>
int write_list_to_file<int>(const std::string& filename, 
                     const std::string& header,
                     const std::vector<int>& dataList,
                     size_t buffer_kb)
{
    return write_list_to_file_inline(filename, header, dataList, buffer_kb);
}

template <>
int write_list_to_file<int64_t>(const std::string& filename, 
                     const std::string& header,
                     const std::vector<int64_t>& dataList,
                     size_t buffer_kb)
{
    return write_list_to_file_inline(filename, header, dataList, buffer_kb);
}

template <>
int write_list_to_file<uint64_t>(const std::string& filename, 
                     const std::string& header,
                     const std::vector<uint64_t>& dataList,
                     size_t buffer_kb)
{
    return write_list_to_file_inline(filename, header, dataList, buffer_kb);
}

template<>
int write_list_to_file<std::string>(const std::string& filename, 
                     const std::string& header,
                     const std::vector<std::string>& dataList,
                     size_t buffer_kb)
{
    return write_list_to_file_inline(filename, header, dataList, buffer_kb);
}

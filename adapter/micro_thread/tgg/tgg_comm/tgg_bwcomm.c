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
#include "nlohmann/json.hpp"
#include "comm/Encrypt.hpp"
#include "comm/RedisClient.hpp"
#include "tgg_comm/tgg_bw_cache.h"
#include "tgg_comm/tgg_common.h"

#ifndef MAX_FD_COUNT
#define MAX_FD_COUNT 100000
#endif

// std::map<int, tgg_bw_info*> g_map_bwinfo;
// std::set<int> g_set_bwidx;
// std::mutex g_map_bwinfo_mtx;
// std::mutex g_set_bwidx_mtx;
// std::atomic<int> s_atomic_idx(0);

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
     *port = big_endian() ? ntohs(remote_addr.sin_port) : remote_addr.sin_port;
     RTE_LOG(INFO, USER1, "[%s][%d] Cmd from [%s]:[%d]\n", __func__, __LINE__, ip, *port);
     return 0;
}

// int get_valid_bw_idx()
// {
//     int idx = 0;
//     int times = 2;// 最多轮训两次
//     while (times > 0) {
//         if (idx > MAX_FD_COUNT || idx < 0) {
//             s_atomic_idx.store(0);
//             idx = 0;
//             times--;
//         } else {
//             idx = s_atomic_idx.fetch_add(1);
//         }
//         if(tgg_exist_bw_idx(idx)) {
//             continue;
//         } else {
//             return idx;
//         }
//     }
//     return -1;
// }

static void init_bwinfo(tgg_bw_info* bwinfo)
{
    memset(bwinfo, 0, sizeof(tgg_bw_info));
    bwinfo->idx = TGG_FD_CLOSED;
}

//tgg_bw_info* lookup_bwinfo(int fd)
// bool tgg_exist_bw_idx(int bwidx)
// {
//     std::lock_guard<std::mutex> gurad(g_set_bwidx_mtx);
//     std::set<int>::iterator it = g_set_bwidx.find(bwidx);
//     if (it != g_set_bwidx.end()) {
//         return true;
//     } 
//     return false;
// }

// bool tgg_add_bw_idx(int bwidx)
// {
//     std::lock_guard<std::mutex> gurad(g_set_bwidx_mtx);
//     std::set<int>::iterator it = g_set_bwidx.find(bwidx);
//     if (it != g_set_bwidx.end()) {
//         return false;
//     } 
//     if (g_set_bwidx.insert(bwidx).second)
//         return true;
//     return false;
// }

// bool tgg_delete_bw_idx(int bwidx)
// {
//     std::lock_guard<std::mutex> gurad(g_set_bwidx_mtx);
//     std::set<int>::iterator it = g_set_bwidx.find(bwidx);
//     if (it != g_set_bwidx.end()) {
//         g_set_bwidx.erase(it);
//         return true;
//     } 
//     return false;
// }



// int tgg_get_bw_idx(int fd)
// {
//     std::lock_guard<std::mutex> gurad(g_map_bwinfo_mtx);
//     std::map<int, tgg_bw_info*>::iterator it = g_map_bwinfo.find(fd);
//     if (it != g_map_bwinfo.end()) {
//         return it->second->idx;
//     } 
//     return TGG_FD_NOTEXIST;
// }

// int tgg_set_bw_idx(int fd, int idx)
// {
//     std::lock_guard<std::mutex> gurad(g_map_bwinfo_mtx);
//     std::map<int, tgg_bw_info*>::iterator it = g_map_bwinfo.find(fd);
//     if (it != g_map_bwinfo.end()) {
//         it->second->idx = idx;
//         return 0;
//     } 
//     return -1;
// }

// int tgg_set_bw_seckey(int fd, const std::string& seckey)
// {
//     if(seckey.size() <= 0) {
//         // 如果是空的，就不需要设置了，目前抓包看服务端填充的都是空的
//         return 0;
//     }
//     std::lock_guard<std::mutex> gurad(g_map_bwinfo_mtx);
//     std::map<int, tgg_bw_info*>::iterator it = g_map_bwinfo.find(fd);
//     if (it != g_map_bwinfo.end()) {
//         int len = seckey.size();
//         if(len > SECRET_KEY_LEN) {
//             len = SECRET_KEY_LEN;
//         }
//         memcpy(it->second->secretKey, seckey.c_str(), len);
//         return 0;
//     } 
//     return -1;
// }

// int tgg_get_bw_seckey(int fd, std::string& seckey)
// {
//     std::lock_guard<std::mutex> gurad(g_map_bwinfo_mtx);
//     std::map<int, tgg_bw_info*>::iterator it = g_map_bwinfo.find(fd);
//     if (it != g_map_bwinfo.end()) {
//         int len = strlen(it->second->secretKey);
//         if(len > SECRET_KEY_LEN) {
//             len = SECRET_KEY_LEN;
//         }
//         seckey = std::string(it->second->secretKey, len);
//         return 0;
//     } 
//     return -1;
// }

// int new_bw_session(int fd)
// {
//     std::map<int, tgg_bw_info*>::iterator it = g_map_bwinfo.find(fd);
//     if (it != g_map_bwinfo.end()) {
//         if (it->second->idx != TGG_FD_CLOSED) {
//             // 连接还没有关闭
//             return -1;
//         } else {
//             it->second->idx = get_valid_bw_idx();
//             if(it->second->idx < 0 || !tgg_add_bw_idx(it->second->idx)) {// 生成idx失败
//                 return -1;
//             }
//         }
//         return 0;
//     }
//     // 全新的fd
//     tgg_bw_info* bwinfo = new(tgg_bw_info);
//     if(!bwinfo) {
//         return -1;
//     }
//     init_bwinfo(bwinfo);
//     int bwidx = get_valid_bw_idx();
//     if(bwidx < 0 || !tgg_add_bw_idx(bwidx)) {// 生成idx失败
//         delete bwinfo;
//         return -1;
//     }
//     bwinfo->idx = bwidx;
//     g_map_bwinfo[fd] = bwinfo;
//     return 0;
// }

// void free_bw_session(int fd)
// {// 不释放空间，只清理，fd是有限的且会重复利用，反复新建和删除影响效率
//     int idx = -1;
//     {
//         std::lock_guard<std::mutex> gurad(g_map_bwinfo_mtx);
//         std::map<int, tgg_bw_info*>::iterator it = g_map_bwinfo.find(fd);
//         if (it != g_map_bwinfo.end()) {
//             idx = it->second->idx;
//             init_bwinfo(it->second);
//         }
//     }
//     close(fd);
//     // 先关闭，后删除
//     if(idx > 0) {
//         tgg_delete_bw_idx(idx);
//     }
// }

// void tgg_set_bw_authorized(int fd, int authorized)
// {
//     std::lock_guard<std::mutex> gurad(g_map_bwinfo_mtx);
//     std::map<int, tgg_bw_info*>::iterator it = g_map_bwinfo.find(fd);
//     if (it != g_map_bwinfo.end()) {
//         it->second->authorized = authorized;
//     }
// }
// int tgg_get_bw_authorized(int fd)
// {
//     std::lock_guard<std::mutex> gurad(g_map_bwinfo_mtx);
//     std::map<int, tgg_bw_info*>::iterator it = g_map_bwinfo.find(fd);
//     if (it != g_map_bwinfo.end()) {
//         return it->second->authorized;
//     }
//     return 0;
// }


// 将输入字符串使用 DEFLATE 压缩
int gzdeflate(const std::string& input, std::string& outBuffer) {
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
        RTE_LOG(ERR, USER1, "[%s][%d] deflate init Error.\n", __func__, __LINE__);
        return -1;
    }

    deflateStream.avail_in = input.length();
    deflateStream.next_in = (Bytef*)input.c_str();
    deflateStream.avail_out = outBufferSize;
    deflateStream.next_out = (Bytef*)outBuffer.c_str();

    // 执行压缩操作
    if (deflate(&deflateStream, Z_FINISH)!= Z_STREAM_END) {
        deflateEnd(&deflateStream);
        RTE_LOG(ERR, USER1, "[%s][%d] deflate failed.\n", __func__, __LINE__);
        return -1;
    }

    // 获取实际压缩后的大小
    uLongf actualOutSize = deflateStream.total_out;

    if(outBufferSize < deflateStream.total_out) {
        RTE_LOG(ERR, USER1, "[%s][%d] Less of reserved buffer length:%lu, total:%lu.\n", __func__, __LINE__, 
            outBufferSize, deflateStream.total_out);
        return -1;
    }
    outBuffer.resize(actualOutSize);
    // 结束deflate流并释放相关资源
    deflateEnd(&deflateStream);

    // 将压缩后的数据转换为字符串并返回
    return 0;
}

int gzinflate(const std::string& input, std::string& outBuffer)
{
    // 预计最大的解压后缓冲区大小，这里设置为输入字符串大小的10倍（可根据实际情况调整）
    uLongf outBufferSize = input.length() * 2;

    // 分配解压后的输出缓冲区
    // std::string outBuffer;
    outBuffer.resize(outBufferSize);

    // 调用inflate函数进行解压
    z_stream inflateStream;
    inflateStream.zalloc = Z_NULL;
    inflateStream.zfree = Z_NULL;
    inflateStream.opaque = Z_NULL;

    // 初始化inflate流
    if (inflateInit2(&inflateStream, -MAX_WBITS)!= Z_OK) {
        RTE_LOG(ERR, USER1, "[%s][%d] inflate init Error.\n", __func__, __LINE__);
        return -1;
    }

    inflateStream.avail_in = input.length();
    inflateStream.next_in = (Bytef*)input.c_str();
    inflateStream.avail_out = outBufferSize;
    inflateStream.next_out = (Bytef*)outBuffer.c_str();

    // 执行解压操作
    if (inflate(&inflateStream, Z_FINISH)!= Z_STREAM_END) {
        inflateEnd(&inflateStream);
        RTE_LOG(ERR, USER1, "[%s][%d] inflate failed.\n", __func__, __LINE__);
        return -1;
    }

    // 获取实际解压后的大小
    uLongf actualOutSize = inflateStream.total_out;
    if(outBufferSize < inflateStream.total_out) {
        RTE_LOG(ERR, USER1, "[%s][%d] Less of reserved buffer length:%lu, total:%lu.\n", __func__, __LINE__, 
            outBufferSize, inflateStream.total_out);
        return -1;
    }
    // 结束inflate流并释放相关资源
    inflateEnd(&inflateStream);
    outBuffer.resize(actualOutSize);
    // 将解压后的数据转换为字符串并返回
    return 0;
}

int message_pack(uint16_t command, uint32_t seq, uint8_t protocol,
            uint8_t compressFormat, const std::string& body, std::string& result)
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

int message_unpack(const std::string& packedData, std::string& result)
{
    const unsigned int PACKAGE_SEPARATOR = 65534;

    // 验证包分隔符
    unsigned short package_separator_check;
    memcpy(&package_separator_check, packedData.c_str(), 2);
    if(big_endian()) {
        package_separator_check = ntohs(package_separator_check);
    }
    if (package_separator_check!= PACKAGE_SEPARATOR) {
        return -1; // 包分隔符不匹配，返回空结果向量
    }

    // 读取包长度
    unsigned int packageLength;
    memcpy(&packageLength, packedData.c_str() + 4, 4);
    if(big_endian()) {
        packageLength = ntohl(packageLength);
    }
    if (packedData.length()!= packageLength) {
        return -1; // 包长度不匹配，返回空结果向量
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
    std::string body = "";
    // 解压缩正文（调用假设存在的decompress函数）
    if (compressFormat) {
        if (gzinflate(compressedBody, body) < 0) {
            return -1; // 解压缩失败，返回空结果向量
        }
    } else {
        body = compressedBody;
    }

    nlohmann::json jdata; 

    // 将解析结果整理到结果向量中
    jdata["cmd"] = command;
    jdata["seq"] = seq;
    jdata["version"] = version;
    jdata["compressFormat"] = compressFormat;
    jdata["body"] = body;
    result = jdata.dump();

    return 0;
}

int tgg_init_uidgid(const std::vector<std::string>& clusterNodes, const std::string& password, const std::string& userName)
{
    std::map<std::string, std::set<std::string>> mapUsers;
    int ret = GetUserWithGids(clusterNodes, mapUsers, password);
    if (ret < 0) {
        RTE_LOG(ERR, USER1, "[%s][%d] Connect redis failed.\n", __func__, __LINE__);
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

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <zlib.h>
#include <rte_log.h>
#include <rte_mempool.h>
#include "tgg_bwcomm.h"

std::map<int, tgg_bw_info*> g_map_bwinfo;

int get_connection_info(int fd, char[] ip, unsigned short* port)
{
     // 获取IP地址信息
     struct sockaddr_in remote_addr;
     socklen_t addrlen = sizeof(remote_addr);
     if (getpeername(fd, (struct sockaddr *)&local_addr, &addrlen) == -1) {
         perror("getpeername");
         close(sockfd);
         return -1;
     }
     inet_ntop(AF_INET, &(local_addr.sin_addr), ip, INET_ADDRSTRLEN);
     port = ntohs(addr.sin_port);
     RTE_LOG(INFO, USER1, "[%s][%d] Cmd from [%s]:[%d]", __func__, __LINE__, ip, port);
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
std::vector<uint8_t> gzdeflate(const std::string& input) {
    // 输出缓冲区
    std::vector<uint8_t> output(compressBound(input.size()));

    uLongf outputSize = output.size();
    int result = compress(output.data(), &outputSize, reinterpret_cast<const Bytef*>(input.data()), input.size());

    if (result != Z_OK) {
        throw std::runtime_error("Failed to compress data");
    }

    // Resize the vector to the actual compressed size
    output.resize(outputSize);
    return output;
}

std::string gzinflate(const std::string& compressedData)
{
    // 分配足够大的缓冲区用于存储解压缩后的数据
    const size_t bufferSize = compressedData.size() * 2;
    std::string uncompressedData;
    uncompressedData.resize(bufferSize);

    // zlib解压缩结构体
    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = compressedData.size();
    strm.next_in = (Bytef*)compressedData.c_str();
    strm.avail_out = bufferSize;
    strm.next_out = (Bytef*)uncompressedData.c_str();

    // 初始化解压缩流
    int ret = inflateInit2(&strm, -MAX_WBITS);
    if (ret!= Z_OK) {
        std::cerr << "inflateInit2 failed with error code: " << ret << std::endl;
        return "";
    }

    // 进行解压缩操作
    ret = inflate(&strm, Z_FINISH);
    if (ret!= Z_STREAM_END && ret!= Z_OK) {
        std::cerr << "inflate failed with error code: " << ret << std::endl;
        inflateEnd(&strm);
        return "";
    }

    // 结束解压缩并清理资源
    inflateEnd(&strm);

    // 调整解压缩后数据的大小，去除多余的缓冲区空间
    uncompressedData.resize(strm.total_out);

    return uncompressedData;
}

std::vector<uint8_t> compress(int compressFormat, const std::string& body) {
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

std::vector<uint8_t> message_pack(uint16_t command, uint32_t seq, uint8_t protocol,
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

    output.push_back(1); // 版本

    output.push_back(protocol);
    output.push_back(compressFormat);

    // 压缩主体
    auto compressedBody = compress(compressFormat, body);
    if (compressedBody.empty()) {
        return {}; // 假设压缩失败返回空
    }
    
    output.insert(output.end(), compressedBody.begin(), compressedBody.end());

    // 计算并设置包长度
    uint32_t packageLength = output.size();
    output[4] = (packageLength >> 24) & 0xFF;
    output[5] = (packageLength >> 16) & 0xFF;
    output[6] = (packageLength >> 8) & 0xFF;
    output[7] = packageLength & 0xFF;

    return output;
}

std::string hex2bin(const std::string& hex)
{
    if (hex.length() % 2 != 0) {
        throw std::invalid_argument("Hex string must have an even length.");
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

std::string message_unpack(const std::string& packedData)
{
    std::string result;
    const int PACKAGE_SEPARATOR = 65534;

    // 验证包分隔符
    short package_separator_check;
    std::memcpy(&package_separator_check, packedData.c_str(), 2);
    if (package_separator_check!= PACKAGE_SEPARATOR) {
        return result; // 包分隔符不匹配，返回空结果向量
    }

    // 读取包长度
    int packageLength;
    std::memcpy(&packageLength, packedData.c_str() + 4, 4);
    if (packedData.length()!= packageLength) {
        return result; // 包长度不匹配，返回空结果向量
    }

    // 读取并解析剩余部分
    size_t offset = 10; // 跳过包分隔符、未使用字段、包长度和额外大小

    short command;
    std::memcpy(&command, packedData.c_str() + offset, 2);
    offset += 2;

    int seq;
    std::memcpy(&seq, packedData.c_str() + offset, 4);
    offset += 4;

    short version;
    std::memcpy(&version, packedData.c_str() + offset, 2);
    offset += 2;

    unsigned char protocol;
    std::memcpy(&protocol, packedData.c_str() + offset, 1);
    offset += 1;

    unsigned char compressFormat;
    std::memcpy(&compressFormat, packedData.c_str() + offset, 1);
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
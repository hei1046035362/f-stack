#pragma once
#include <map>
#include <set>
#include <list>
#include <vector>
#include <string>
#include <unordered_map>
#include <hiredis/hiredis.h>

// 用于存储集群节点信息和连接上下文的结构体
struct ClusterNodeInfo {
    std::string node;
    redisContext* context;
};

class RedisClient {
public:
    RedisClient(const std::vector<std::string>& nodes) : cluster_nodes(nodes) {}
    ~RedisClient() {Disconnect();}
    // 连接到Redis集群
    bool Connect(const std::string& redis_password = "", const std::string& redis_user = "");

    // 从Redis集群中读取指定key的hash数据（类似map）
    std::unordered_map<std::string, std::string> ReadMap(const char* key);

    // 从Redis或Redis集群中读取指定key的list数据
    std::list<std::string> ReadList(const char* key);

    // 从Redis或Redis集群中读取指定key的set数据
    std::set<std::string> ReadSet(const char* key);

    // 从Redis或Redis集群中根据指定规则找匹配的keys
    std::list<std::string> GetKeys(const char* patterns);

    std::string GetKeyType(const char* key_name);

    // 关闭与Redis集群的连接
    void Disconnect();

private:
    std::vector<std::string> cluster_nodes;
    std::vector<ClusterNodeInfo> node_contexts;
};


std::string ExtractStringBetweenColons(const std::string& input);

int GetUserWithGids(const std::vector<std::string>& clusterNodes, 
                    std::map<std::string, std::set<std::string> >& result, 
                    const std::string& password = "");
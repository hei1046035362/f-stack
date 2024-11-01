#include <cstring>
#include <iostream>
#include <hiredis/hiredis.h>
#include <map>
#include <set>
#include <list>
#include <vector>
#include <string>
#include <unordered_map>

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
    bool Connect(const std::string& redis_password = "", const std::string& redis_user = "") {
        char authstr[128] = {0};
        // AUTH命令必须要在调用命令之前格式化，使用redisCommand函数中的格式化会授权失败，暂不清楚原因
        sprintf(authstr, "AUTH %s %s", redis_user.c_str(), redis_password.c_str());
        for (const auto& node : cluster_nodes) {
            int pos = node.find(":");
            std::string ip = node.substr(0, pos);
            std::string port = node.substr(pos+1);
            // 设置超时时间为5秒
            struct timeval timeout = {5, 0};
            redisContext* context = redisConnectWithTimeout(ip.c_str(), std::stoi(port), timeout);
            if (context == nullptr || context->err) {
                if (context) {
                    std::cerr << "Error connecting to node " << node << ": " << context->errstr << std::endl;
                    redisFree(context);
                } else {
                    std::cerr << "Can't allocate redis context for node " << node << std::endl;
                }
                continue;
                // return false;
            } else {
                std::cout << "Connect OK to node: " << node << std::endl;
            }
            if (!redis_password.empty())
            {
                // 发送AUTH命令进行认证
                redisReply* reply = static_cast<redisReply*>(redisCommand(context, authstr));
                if (reply == nullptr) {
                    std::cerr << "Error executing AUTH command" << std::endl;
                    redisFree(context);
                    continue;
                }

                // 检查认证结果
                if (reply->type == REDIS_REPLY_STATUS && strcmp(reply->str, "OK") == 0) {
                    std::cout << "认证成功，已连接到Redis服务器。" << std::endl;
                    // 释放回复对象
                    freeReplyObject(reply);
                } else {
                    std::cerr << "认证失败：" << reply->str << std::endl;
                    // 释放回复对象
                    freeReplyObject(reply);
                    // 关闭连接
                    redisFree(context);
                    continue;
                }
            }
            ClusterNodeInfo info = {node, context};
            node_contexts.push_back(info);
        }
        if (node_contexts.size() > 0) {
            return true;
        }
        return false;
    }

    // 从Redis集群中读取指定key的hash数据（类似map）
    std::unordered_map<std::string, std::string> ReadMap(const char* key) {
        std::unordered_map<std::string, std::string> result;

        for (const auto& info : node_contexts) {
            redisReply* reply = static_cast<redisReply*>(redisCommand(info.context, "HGETALL %s", key));
            if (reply == nullptr) {
                std::cerr << "Error executing HGETALL command on node " << info.node << std::endl;
                continue;
            }

            if (reply->type == REDIS_REPLY_ARRAY) {
                for (size_t i = 0; i < reply->elements; i += 2) {
                    result[reply->element[i]->str] = reply->element[i + 1]->str;
                }
            }

            freeReplyObject(reply);
        }

        return result;
    }

    // 从Redis或Redis集群中读取指定key的list数据
    std::list<std::string> ReadList(const char* key) {
        std::list<std::string> result;

        for (const auto& info : node_contexts) {
            redisReply* reply = static_cast<redisReply*>(redisCommand(info.context, "LRANGE %s 0 -1", key));

            if (reply == nullptr) {
                std::cerr << "Error executing LRANGE command on node" << info.node  << std::endl;
                continue;
                //return result;
            }

            if (reply->type == REDIS_REPLY_ARRAY) {
                for (size_t i = 0; i < reply->elements; ++i) {
                    result.push_back(reply->element[i]->str);
                }
            } else {
                std::cerr << "Key not found or incorrect type" << std::endl;
            }

            freeReplyObject(reply);
        }
        return result;
    }

    // 从Redis或Redis集群中读取指定key的set数据
    std::set<std::string> ReadSet(const char* key) {
        std::set<std::string> result;

        for (const auto& info : node_contexts) {
            redisReply* reply = static_cast<redisReply*>(redisCommand(info.context, "SMEMBERS %s", key));

            if (reply == nullptr) {
                std::cerr << "Error executing SMEMBERS command on node" << info.node  << std::endl;
                continue;
                //return result;
            }

            if (reply->type == REDIS_REPLY_ARRAY) {
                for (size_t i = 0; i < reply->elements; ++i) {
                    result.insert(reply->element[i]->str);
                }
            } else {
                std::cerr << "Key not found or incorrect type" << std::endl;
            }

            freeReplyObject(reply);
        }
        return result;
    }

    // 从Redis或Redis集群中根据指定规则找匹配的keys
    std::list<std::string> GetKeys(const char* patterns) {
        std::list<std::string> result;

        for (const auto& info : node_contexts) {
            char cursor[32] = "0";
            do {
                redisReply* reply = static_cast<redisReply*>(redisCommand(info.context, "SCAN %s MATCH %s COUNT 100", cursor, patterns));

                if (reply == nullptr) {
                    std::cerr << "Error executing SCAN command on node" << info.node  << std::endl;
                    break;
                }
                // 获取下一个游标值
                strcpy(cursor, reply->element[0]->str);
                if (reply->type == REDIS_REPLY_ARRAY) {
                    for (size_t i = 0; i < reply->element[1]->elements; ++i) {
                        result.push_back(std::string(reply->element[1]->element[i]->str));
                    }
                } else {
                    std::cerr << "No key matchs the patterns[" << patterns << "]." << std::endl;
                }

                freeReplyObject(reply);
            } while (strcmp(cursor, "0") != 0);
        }
        return result;
    }


    std::string GetKeyType(const char* key_name)
    {
        std::string result;
            // 执行TYPE命令获取键的类型回复
        for (const auto& info : node_contexts) {
            redisReply* reply = static_cast<redisReply*>(redisCommand(info.context, "TYPE %s", key_name));
            if (reply == nullptr) {
                std::cerr << "Error executing TYPE command" << std::endl;
                continue;
            }
            result = reply->str;
            // 输出键的类型
            std::cout << "键 " << key_name << " 的类型是: " << reply->str << std::endl;

            // 释放回复对象和连接上下文
            freeReplyObject(reply);
            break;
        }
        return result;
    }
    // 关闭与Redis集群的连接
    void Disconnect() {
        for (const auto& info : node_contexts) {
            redisFree(info.context);
        }
        node_contexts.clear();
    }

private:
    std::vector<std::string> cluster_nodes;
    std::vector<ClusterNodeInfo> node_contexts;
};


std::string ExtractStringBetweenColons(const std::string& input) {
    size_t firstColonPos = input.find(':');
    if (firstColonPos == std::string::npos) {
        return "";
    }

    size_t secondColonPos = input.find(':', firstColonPos + 1);
    if (secondColonPos == std::string::npos) {
        return "";
    }

    return input.substr(firstColonPos + 1, secondColonPos - firstColonPos - 1);
}

std::map<std::string, std::set<std::string> > GetUserWithGids(std::vector<std::string>& clusterNodes, 
    const char* password)
{
    std::map<std::string, std::set<std::string>> result;
    RedisClient reader(clusterNodes);

    if (reader.Connect(password)) {
        std::list<std::string> lstuser = reader.GetKeys("user*");
        if (lstuser.size() > 0) {
            std::list<std::string>::iterator it = lstuser.begin();
            while (it != lstuser.end()) {
                std::string userId = ExtractStringBetweenColons(*it);
                if (!userId.empty()) {
                    result[userId] = reader.ReadSet((*it).c_str());
                } else {
                    std::cout << "Key: " << *it << " not found." << std::endl;
                }
                it++;
            }
        }
    }
    return result;
}

// int main(int argc, char* argv[]) {
//     // 假设这里是Redis集群的节点列表，根据实际情况修改
//     std::vector<std::string> clusterNodes = {
//         "54.241.112.155:7001",
//         "54.241.112.155:7002",
//     };
//     std::map<std::string, std::set<std::string>> mapUsers = GetUserWithGids(clusterNodes, "bZSCEI3VyV");
//     std::map<std::string, std::set<std::string>>::iterator itUid = mapUsers.begin();
//     while(itUid != mapUsers.end()) {
//         std::set<std::string>::iterator itGid = itUid->second.begin();
//         while(itGid != itUid->second.end()) {
//             std::cout << "userId[" << itUid->first << "]: Gid[" << *itGid << "]" << std::endl;
//             itGid++;
//         }
//         itUid++;
//     }

//     return 0;
// }
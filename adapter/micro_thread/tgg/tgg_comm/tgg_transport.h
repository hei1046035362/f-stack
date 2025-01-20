#pragma once
#include <string>
#include <list>
/// 透传接口

/// 服务端透传到客户端
// 后台直发客户端的数据，token校验成功后才能正常调用本接口
void Send2Client(const char* cid, const std::string& data, int fd_opt);

// 批量发送接口
void BatchSend2Client(std::list<std::string> cids, const std::string& data, int fd_opt);

// 批量发送接口
void BatchSend2Client(std::list<int> fds, const std::string& data, int fd_opt);

// 客户端的内容透传到服务端

// 客户端数据转发给服务端
/// @param --fd   客户端连接的fd，用来做随机的，发送的队列是固定的，
///               防止进程之间不必要的信息交换，直接用fd%队列数做负载均衡,因为fd是可回收的，所以这个均衡还是有一定保障的
void Send2Server(int fd, const std::string& data, int fd_opt);
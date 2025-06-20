#pragma once
#include <string>
#include <list>
/// 透传接口

/// 服务端透传到客户端
// 通过fd发送
void Send2Fd(int core_id, int fd, int idx, const std::string& data, int fd_opt, int encode);
// 通过cid发送 后台直发客户端的数据，token校验成功后才能正常调用本接口
void Send2Client(int cid, const std::string& data, int fd_opt, int encode);

// 批量发送接口
void BatchSend2ClientBycids(std::list<int> cids, const std::string& data, int fd_opt, int encode);

// 批量发送接口
void BatchSend2ClientByfds(std::list<int64_t> fds, const std::string& data, int fd_opt, int encode);

// 客户端的内容透传到服务端


enum SEND_SERVER_STATUS {
    SEND_SUCCESS = 0,
    NO_BW_AVALIABLE = 1,
    SEND_FAILED = 2
};

// 客户端数据转发给服务端
/// @param --fd   客户端连接的fd，用来做随机的，发送的队列是固定的，
///               防止进程之间不必要的信息交换，直接用fd%队列数做负载均衡,因为fd是可回收的，所以这个均衡还是有一定保障的
int Send2Server(int core_id, int fd, const std::string& data, int fd_opt);
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
void BatchSend2ClientBycids(std::list<int>& cids, const std::string& data, int fd_opt, int encode);

// 批量发送接口
void BatchSend2ClientByfds(const std::list<int64_t> &fds, const std::string& data, int fd_opt, int encode);

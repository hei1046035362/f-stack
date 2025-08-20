#pragma once

#include "comm/RWIni.hpp"

class TggConfigure {
private:
    static TggConfigure* instance;

    // 构造函数私有，防止外部创建实例
    TggConfigure() {}

    // 拷贝构造函数和赋值运算符也设为私有，避免通过拷贝或赋值产生多个实例
    TggConfigure(const TggConfigure&) = delete;
    TggConfigure& operator=(const TggConfigure&) = delete;

public:
    // 静态函数获取单例实例
    static TggConfigure* getInstance() {
        return instance;
    }
    int init(const char* fstack_conf, const char* tgg_conf);
private:
    int gwwrite_co_count;          // gwrcv写协程的个数
    int lcore_mask;         // 收包进程绑定的核
    int ccore_mask;         // cli处理进程绑定的核
    std::vector<int>    lcore_pos;/// 收包进程绑定的核的位置
    std::vector<int>    ccore_pos;/// cli处理进程每个core在掩码中的位置，创建线程池及绑核使用
    std::string addr;        // 网关对外ip  客户端
    unsigned short port;    // 网关对外使用的端口  客户端
    std::vector<std::string> redis_addrs; // redis集群地址
    std::string redis_pwd;  // redis 登陆密码
    int bcore_mask;         // bw 进程要绑定的核
    unsigned int bwsvr_count;        // bw处理进程个数
    int co_count;           // 单个bwserver持有的协程数
    std::string bw_addr;        // 网关对内ip   服务端
    unsigned short bw_port;    // 网关对内使用的端口  服务端
    int bw_heart_beat;
    std::string register_addr;        // 注册中心的地址
    unsigned short register_port;    // 注册中心的端口
    std::string secret_key;        // 网关和bw消息加密的秘钥
    std::string log_path;        // 日志级别
    std::string gateway_log_level;         // gwrcv和cliprc的日志级别
    std::string register_log_level;        // register日志级别
    std::string bwserver_log_level;        // bwrcv服务日志级别

    int gwrcv_fd_limit;             // gwrcv单个进程承载的客户端连接的上限
    int gwbwprc_fd_limit;           // gwbwprc单个进程承载的bw连接的上限

    int auto_start;             // # 是否启用进程自动管理，1 是，0，否(意味着所有进程全部都需要手动启动)

    std::string ip_filter_path;     // ip 黑、白名单文件路径

public:
    int get_lcore_mask() { return lcore_mask;}
    int get_ccore_mask() { return ccore_mask;}
    int get_bcore_mask() { return bcore_mask;}
    int get_gwwrite_co_count() {return gwwrite_co_count;}
    const std::vector<int>& get_lcore_pos() {return lcore_pos;}
    const std::vector<int>& get_ccore_pos() {return ccore_pos;}
    const std::string& get_gateway_addr() {return addr;}
    unsigned short get_gateway_port() {return port;}
    const std::vector<std::string>& get_redis_addrs() {return redis_addrs;}
    const std::string& get_redis_pwd() {return redis_pwd;}
    unsigned int get_bwsvr_count() {return bwsvr_count;}
    int get_bwsvr_co_count() {return co_count;}
    const std::string& get_bwsvr_bw_addr() {return bw_addr;}
    unsigned short get_bwsvr_bw_port() {return bw_port;}
    int get_bwsvr_heart_beat() {return bw_heart_beat;}
    const std::string& get_register_addr() {return register_addr;}
    unsigned short get_register_port() {return register_port;}
    const std::string& get_secret_key() {return secret_key;}
    const std::string& get_log_path() {return log_path;}
    const std::string& get_gateway_log_level() {return gateway_log_level;}
    const std::string& get_register_log_level() {return register_log_level;}
    const std::string& get_bwserver_log_level() {return bwserver_log_level;}
    int get_gwrcv_fd_limit() { return gwrcv_fd_limit;}
    int get_gwbwprc_fd_limit() { return gwbwprc_fd_limit;}
    int get_auto_start() { return auto_start;}
    const std::string& get_ip_filter_path() {return ip_filter_path;}
};

int tgg_init_config(int& argc, char* argv[]);

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
    int init(const char* filename);
private:
    int lcore_count;   // 收包进程的个数，从f-stack的conf.ini的lcore_mask获取，用于process确定要启动多少个进程
    std::vector<int>    lcore_pos;/// 每个lcore在掩码中的位置
    std::string addr;        // 网关对外ip  客户端
    unsigned short port;    // 网关对外使用的端口  客户端
    std::vector<std::string> redis_addrs; // redis集群地址
    std::string redis_pwd;  // redis 登陆密码
    int bwsvr_count;        // bw处理进程个数
    int co_count;           // 单个bwserver持有的协程数
    std::string bw_addr;        // 网关对内ip   服务端
    unsigned short bw_port;    // 网关对内使用的端口  服务端
    int bw_heart_beat;
public:
    int get_lcore_count() {return lcore_count;}
    const std::vector<int>& get_lcore_pos() {return lcore_pos;}
    const std::string& get_gateway_addr() {return addr;}
    unsigned short get_gateway_port() {return port;}
    const std::vector<std::string>& get_redis_addrs() {return redis_addrs;}
    const std::string& get_redis_pwd() {return redis_pwd;}
    int get_bwsvr_count() {return bwsvr_count;}
    int get_bwsvr_co_count() {return co_count;}
    const std::string& get_bwsvr_bw_addr() {return bw_addr;}
    unsigned short get_bwsvr_bw_port() {return bw_port;}
    int get_bwsvr_heart_beat() {return bw_heart_beat;}
};

TggConfigure* TggConfigure::instance = new TggConfigure;

void tgg_init_config(int argc, char* argv[]);

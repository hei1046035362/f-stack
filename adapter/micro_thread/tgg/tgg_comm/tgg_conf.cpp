#include "tgg_conf.h"
#include "comm/common.hpp"
#include <getopt.h>
#include <rte_log.h>
#include <unistd.h>
#include <string.h>

#ifndef MAX_LCORE_COUNT
#define MAX_LCORE_COUNT 32
#endif

TggConfigure* TggConfigure::instance = new TggConfigure;
// 输入参数解析
const char* tgg_short_options = "c:t:p:g:";
struct option tgg_long_options[] = {
    { "conf", 1, NULL, 'c'},
    { "proc-type", 1, NULL, 't'},
    { "proc-id", 1, NULL, 'p'},
    { "tgg-conf", 1, NULL, 'g'},
    { 0, 0, 0, 0}
};

// std::string sco_count = pTgg_Ini.getValue("bwserver", "co_count");

static int get_int_value(IniFileHandler *pFstack_Ini, const char* section, 
    const char* key)
{
    std::string value = pFstack_Ini->getValue(section, key);
    int data = -1;
    if(!value.empty()) {
        try {
            data = std::stoi(value);
        } catch (...) {
            RTE_LOG(ERR, USER1, "[%s][%d] parse config section[%s] key[%s] value[%s] failed.",
             __FILE__, __LINE__, section, key, value.c_str());
            return -1;
        }
        return data;
    }
    RTE_LOG(ERR, USER1, "[%s][%d] parse config section[%s] key[%s] failed:[%s], value is empty.",
         __FILE__, __LINE__, section, key, value.c_str());
    return -1;
}

static int parse_lcore_mask(const std::string& lcore_mask) {
    int value = 0;
    try {
        if (lcore_mask.empty()) {
            throw std::invalid_argument("Empty lcore_mask string");
        }

        // 判断字符串的前缀来确定进制
        if (lcore_mask.size() > 1 && lcore_mask[0] == '0') {
            if (lcore_mask[1] == 'x' || lcore_mask[1] == 'X') {
                // 16进制
                value = std::stoi(lcore_mask, nullptr, 16);
            } else if (lcore_mask[1] == 'b' || lcore_mask[1] == 'B') {
                // 二进制
                value = std::stoi(lcore_mask.substr(2), nullptr, 2);
            } else {
                // 8进制
                value = std::stoi(lcore_mask, nullptr, 8);
            }
        } else {
            // 十进制
            value = std::stoi(lcore_mask, nullptr, 10);
        }
    } catch (const std::invalid_argument& e) {
        RTE_LOG(ERR, USER1, "[%s][%d] Invalid argument: %s.", __FILE__, __LINE__, e.what());
        throw;
    } catch (const std::out_of_range& e) {
        RTE_LOG(ERR, USER1, "[%s][%d] Out of range: %s.", __FILE__, __LINE__, e.what());
        throw;
    }

    return value;
}

int TggConfigure::init(const char* fstack_conf, const char* tgg_conf)
{
    if(access(fstack_conf, F_OK) || access(tgg_conf, F_OK)) {
        RTE_LOG(ERR, USER1, "[%s][%d] fstack config[%s] or tgg config file[%s] not exist.",
         __FILE__, __LINE__, fstack_conf, tgg_conf);
        return -1;
    }
    IniFileHandler pFstack_Ini;
    pFstack_Ini.readFromFile(fstack_conf);

    // lcore_mask  从f-stack的配置中获取
    std::string core_mask = pFstack_Ini.getValue("dpdk", "lcore_mask");
    int lcore_mask = parse_lcore_mask(core_mask);
    if(lcore_mask <= 0) {
        RTE_LOG(ERR, USER1, "[%s][%d] read config lcore mask failed:[%d].",
         __FILE__, __LINE__, lcore_mask);
        return -1;
    }
    this->lcore_mask = lcore_mask;
    unsigned int i = 0;
    int lcore_count = 0;
    for (; i < MAX_LCORE_COUNT; ++i) {  // 循环遍历整数的每一位（以int类型为例，共32位）
        if (this->lcore_mask & (1 << i)) {  // 通过与运算判断当前位是否为1
            lcore_count++;
            this->lcore_pos.push_back(i);  // 如果当前位是1，记录其位置（从右往左，从0开始计数）
        }
    }

    // 以下是从 tgg 的配置文件中获取
    IniFileHandler pTgg_Ini;
    pTgg_Ini.readFromFile(tgg_conf);
    // 网关ip
    this->addr = pTgg_Ini.getValue("gateway", "ip");
    if(!is_ipv4(this->addr)) {
        RTE_LOG(ERR, USER1, "[%s][%d] parse config gateway ip:[%s] failed.", __FILE__, __LINE__, this->addr.c_str());
        return -1;
    }
    // 网关端口
    int ret = get_int_value(&pTgg_Ini, "gateway", "port");
    if(ret <= 0) return -1;
    this->port = ret;
    if(this->port > 65535) {
        RTE_LOG(ERR, USER1, "[%s][%d] invalid gateway port:[%d].", __FILE__, __LINE__, this->port);
        return -1;
    }

    // ccore_mask  cli的线程绑定那几个core
    core_mask = pTgg_Ini.getValue("gateway", "ccore_mask");
    int ccore_mask = parse_lcore_mask(core_mask);
    if(ccore_mask <= 0) {
        RTE_LOG(ERR, USER1, "[%s][%d] read config lcore mask failed:[%d].",
         __FILE__, __LINE__, ccore_mask);
        return -1;
    }
    // 收包进程绑定的core不能与cli处理线程绑定的core重叠
    if(this->lcore_mask & ccore_mask) {
        RTE_LOG(ERR, USER1, "[%s][%d] lcore mask[%x] can't duplicate with ccore mask:[%x].",
         __FILE__, __LINE__, this->lcore_mask, ccore_mask);
        return -1;
    }
    this->ccore_mask = ccore_mask;
    int ccore_count = 0;
    i = 0;
    for (; i < MAX_LCORE_COUNT; ++i) {  // 循环遍历整数的每一位（以int类型为例，共32位）
        if (this->ccore_mask & (1 << i)) {  // 通过与运算判断当前位是否为1
            this->ccore_pos.push_back(i);  // 如果当前位是1，记录其位置（从右往左，从0开始计数）
            ccore_count++;
        }
    }

    // 收包进程数要等于cli处理线程数
    if(lcore_count != ccore_count) {
        RTE_LOG(ERR, USER1, "[%s][%d] lcore count[%d] not equal to ccore count:[%d].",
         __FILE__, __LINE__, lcore_count, ccore_count);
        return -1;
    }

    // redis cluster ip
    std::string redis_ips = pTgg_Ini.getValue("redis", "addrs");
    split_string(redis_ips, ',', this->redis_addrs);
    for (auto addr : this->redis_addrs) {
        if(!is_ipport_format(addr)) {
            RTE_LOG(ERR, USER1, "[%s][%d] parse config redis ipport:[%s] failed.", __FILE__, __LINE__, addr.c_str());
            return -1;
        }
    }
    // redis password
    this->redis_pwd = pTgg_Ini.getValue("redis", "password");
    if(this->redis_pwd.empty() || this->redis_pwd.length() > 128) {
        RTE_LOG(ERR, USER1, "[%s][%d] parse config redis password:[%s] failed.", __FILE__, __LINE__, this->redis_pwd.c_str());
        return -1;
    }
    // bw服务进程个数
    int nbwsvr_count = get_int_value(&pTgg_Ini, "bwserver", "process_count");
    if(nbwsvr_count <= 0)  return -1;
    this->bwsvr_count = nbwsvr_count;
    if(this->bwsvr_count > 100 || this->bwsvr_count < 1) {
        // this->bwsvr_count = 3;// 默认个数
        RTE_LOG(ERR, USER1, "[%s][%d] invalid bw process_count:[%d].", __FILE__, __LINE__, this->bwsvr_count <= 0);
        return -1;
    }

    // bw 单个服务进程协程的个数
    this->co_count = get_int_value(&pTgg_Ini, "bwserver", "co_count");
    if(this->co_count <= 0) return -1;

    if(this->co_count > 100 || this->co_count < 1) {
        RTE_LOG(ERR, USER1, "[%s][%d] invalid bw co_count:[%d].", __FILE__, __LINE__, this->co_count);
        return -1;
    } else {
        this->co_count = 50;// 默认个数
    }
    // 网关对内ip
    this->bw_addr = pTgg_Ini.getValue("bwserver", "ip");
    if(!is_ipv4(this->bw_addr)) {
        RTE_LOG(ERR, USER1, "[%s][%d] parse config bwserver ip:[%s] failed.", __FILE__, __LINE__, this->bw_addr.c_str());
        return -1;
    }
    // 网关对内端口
    ret = get_int_value(&pTgg_Ini, "bwserver", "port");
    if (ret <= 0) return -1;
    this->bw_port = (unsigned short)ret;
    if(this->bw_port > 65535) {
        RTE_LOG(ERR, USER1, "[%s][%d] invalid bwserver port:[%d].", __FILE__, __LINE__, ret);
        return -1;
    }
    // 监控进程假死的间隔  单位(min)
    this->bw_heart_beat = get_int_value(&pTgg_Ini, "bwserver", "port");
    if (this->bw_heart_beat <= 0) this->bw_heart_beat = 5;

    // 网关对内ip
    this->register_addr = pTgg_Ini.getValue("register", "ip");
    if(!is_ipv4(this->register_addr)) {
        RTE_LOG(ERR, USER1, "[%s][%d] parse config register ip:[%s] failed.", __FILE__, __LINE__, this->register_addr.c_str());
        return -1;
    }
    // 网关对内端口
    ret = get_int_value(&pTgg_Ini, "register", "port");
    if (ret <= 0) return -1;
    this->register_port = (unsigned short)ret;
    if(this->register_port > 65535) {
        RTE_LOG(ERR, USER1, "[%s][%d] invalid register port:[%d].", __FILE__, __LINE__, ret);
        return -1;
    }

    return 0;
}


// 删除 argv 中的选项及其值
static void remove_option_from_argv(int& argc, char** argv, int opt_index) {
    if (opt_index < 0 || opt_index >= argc) {
        return; // 无效索引
    }

    // 将后面的参数向前移动
    for (int i = opt_index; i < argc - 1; ++i) {
        argv[i] = argv[i + 1];
    }

    // 减少 argc 并置空最后一个元素
    argc--;
    argv[argc] = nullptr;
}

int tgg_init_config(int& argc, char* argv[])
{
    int c;
    int index = 0;
    int index_argv_g = -1;
    bool has_value = false;
    std::string fstack_filename = "/etc/tgg_gw/config.ini";
    std::string tgg_filename = "/etc/tgg_gw/tgg_conf.ini";
    optind = 1;
    while((c = getopt_long(argc, argv, tgg_short_options, tgg_long_options, &index)) != -1) {
        switch (c) {
            case 'c':
                fstack_filename = strdup(optarg);
                break;
            case 'g':
                tgg_filename = strdup(optarg);
                // 找到 -g 及其值在 argv 中的位置
                for (int i = 1; i < argc; ++i) {
                    if (strcmp(argv[i], "-g") == 0) {
                        index_argv_g = i;
                        has_value = true;
                        break;
                    } else if(strcmp(argv[i], "--tgg-conf") == 0) {
                        index_argv_g = i;
                        break;
                    }
                }
                break;
            default:
                break;
        }
    }
    // 从argv中移除-g|--tgg-conf选项
    if(index_argv_g > 0) {
        // 删除 -g 选项
        remove_option_from_argv(argc, argv, index_argv_g);
        // 删除 -g 的值
        if(has_value) {
            remove_option_from_argv(argc, argv, index_argv_g);
        }
    }

    if (TggConfigure::getInstance()->init(fstack_filename.c_str(), tgg_filename.c_str()) < 0) {
        return -1;
    }
    return 0;
}

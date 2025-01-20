#include "tgg_conf.h"
// 输入参数解析
char* const tgg_short_options = "c:t:";
struct option tgg_long_options[] = {
    { "conf", 1, NULL, 'c'},
    { "tgg-conf", 1, NULL, 't'},
    { 0, 0, 0, 0},
};
    string sco_count = pTgg_Ini.getValue("bwserver", "co_count");

static int get_int_value(IniFileHandler *pFstack_Ini, const char* section, 
    const char* key)
{
    string value = pTgg_Ini.getValue(section, key);
    int data = -1;
    if(!value.empty()) {
        try {
            data = std::stoi(value);
        } catch (...) {
            RTE_LOG(ERR, USER1, "[%s][%d] parse config section[%s] key[%s] value[%s] failed:[%s].",
             __FILE__, __LINE__, section, key, value.c_str());
            return -1;
        }
        return data;
    }
    RTE_LOG(ERR, USER1, "[%s][%d] parse config section[%s] key[%s] failed:[%s], value is empty.",
         __FILE__, __LINE__, section, key, value.c_str());
    return -1;
}

int TggConfigure::init(const char* fstack_conf, const char* tgg_conf)
{
    if(access(fstack_conf, F_OK) || access(tgg_conf, F_OK)) {
        RTE_LOG(ERR, USER1, "[%s][%d] fstack config[%s] or tgg config file[%s] not exist.",
         __FILE__, __LINE__, fstack_conf.c_str(), tgg_conf.c_str());
        return -1;
    }
    IniFileHandler pFstack_Ini;
    pFstack_Ini.readFromFile(fstack_conf);

    // lcore_mask  从f-stack的配置中获取
    string core_mask = pFstack_Ini.getValue("dpdk", "lcore_mask");
    int lcore_mask = get_int_value(core_mask);
    if(lcore_mask <= 0) {
        RTE_LOG(ERR, USER1, "[%s][%d] read config lcore mask failed:[%s].",
         __FILE__, __LINE__, core_mask.c_str());
        return -1;
    }
    for (int i = 0; i < sizeof(int) * 8; ++i) {  // 循环遍历整数的每一位（以int类型为例，共32位）
        if (lcore_mask & (1 << i)) {  // 通过与运算判断当前位是否为1
            this->lcore_count++;
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

    // redis cluster ip
    std::string redis_ips = pTgg_Ini.getValue("redis", "addrs");
    split_string(redis_addrs, ',', this->redis_addrs);
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
    this->bwsrv_count = get_int_value(&pTgg_Ini, "bwserver", "process_count");
    if(this->bwsrv_count <= 0)  return -1;
    if(this->bwsrv_count > 100 || this->bwsrv_count < 1) {
        RTE_LOG(ERR, USER1, "[%s][%d] invalid bw process_count:[%d].", __FILE__, __LINE__, this->bwsrv_count <= 0);
        return -1;
    } else {
        this->bwsrv_count = 3;// 默认个数
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

    return 0;
}

void tgg_init_config(int argc, char* argv[])
{
    int c;
    int index = 0;
    optind = 1;
    char* fstack_filename = "";
    char* tgg_filename = "";
    while((c = getopt_long(argc, argv, tgg_short_options, tgg_long_options, &index)) != -1) {
        switch (c) {
            case 'c':
                fstack_filename = strdup(optarg);
                break;
            case 't':
                tgg_filename = strdup(optarg);
                break;
            default:
                break;
        }
    }
    if(fstack_filename.empty() || tgg_filename.empty()) {
        RTE_LOG(ERR, USER1, "[%s][%d] fstack conf[%s] and tgg conf[%s] can't be empty.",
         __FILE__, __LINE__, fstack_filename.c_str(), tgg_filename.c_str());
        return -1;
    }
    if (TggConfigure::instance::init(fstack_filename, tgg_filename) < 0) {
        return -1;
    }
}

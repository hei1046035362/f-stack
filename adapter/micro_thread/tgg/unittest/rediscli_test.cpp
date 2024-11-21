#include "../comm/RedisClient.hpp"
#include "../tgg_comm/tgg_bw_cache.h"
#include "../tgg_comm/tgg_bwcomm.h"
#include "../tgg_comm/tgg_common.h"
#include "../dpdk_init.h"

#include <rte_eal.h>
#include <rte_debug.h>
std::vector<std::string> g_redis_clusternodes = {
    "54.241.112.155:7001",
    "54.241.112.155:7002",
};
const char* g_redis_pwd = "bZSCEI3VyV";


void tgg_process_init()
{
    tgg_secondary_init();
    if(tgg_init_uidgid(g_redis_clusternodes, g_redis_pwd) < 0) {
        rte_exit(EXIT_FAILURE, "Init uidgid hash failed.\n");
    }
}


int main(int argc, char* argv[]) {
    int ret;
    char c_flag[] = "-c3";
    char n_flag[] = "-n4";
    char mp_flag[] = "--proc-type=secondary";
    char log_flag[] = "--log-level=6";
    char *argp[5];

    argp[0] = argv[0];
    argp[1] = c_flag;
    argp[2] = n_flag;
    argp[3] = mp_flag;
    argp[4] = log_flag;
    argc += 4;
    ret = rte_eal_init(argc, argp);

    tgg_process_init();

    ret = rte_eal_cleanup();
    if (ret)
        printf("Error from rte_eal_cleanup(), %d\n", ret);

    return 0;
}

// g++ redistest.cpp -lstdc++ -lhiredis -I../ -L../ -lmt -lfstack -L${FF_PATH}/lib $(pkg-config --static --libs libdpdk) -lrt -lm -ldl -lcrypto
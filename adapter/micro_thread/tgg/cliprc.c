#include <stdio.h>
#include <stdlib.h>
// #include "mt_incl.h"
// #include "micro_thread.h"
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <sys/wait.h>
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/tgg_struct.h"
#include "dpdk_init.h"
#include "tgg_comm/WsConsumer.h"
#include "comm/Encrypt.hpp"
// #include "bwserver.h"
#include "tgg_comm/tgg_bw_cache.h"
#include "tgg_comm/tgg_bwcomm.h"
#include "tgg_comm/tgg_conf.h"
#include <vector>
#include "tgg_comm/tgg_cliprc.h"
// 绝对路径
const char* f_stack_ini = "/data/code/f-stack/config.ini"

int g_run = 1;
static const char* s_dump_file = "/var/corefiles/tgg_gw_cliprc_core";

void signal_handler(int signum)
{
	if(signum == SIGINT || signum == SIGTERM) {
		if(g_run) {
			g_run = 0;
			rte_eal_cleanup();
        	prc_exit(0, "catched signal:%d\n", signum);
		}
	}
}

void tgg_gw_process(void* data)
{
	ThreadArray threads(TggConfigure::instance::get_lcore_pos());
	threads.startThreads(tgg_process_read);
}

void tgg_sig_init()
{
	if (signal(SIGINT, signal_handler) == SIG_ERR) {
        perror("Error setting signal handler");
        prc_exit(-1, "Error setting signal handler");
    }
	if (signal(SIGTERM, signal_handler) == SIG_ERR) {
        perror("Error setting signal handler");
        prc_exit(-1, "Error setting signal handler");
    }

}

void tgg_process_init()
{
	tgg_sig_init();// 信号处理初始化
	tgg_secondary_init();// dpdk相关初始化
	tgg_iterprint_gidsbyuid();// 打印redis中获取的数据
	initOpenSSL();// 初始化ssl加解密环境
	init_endians();// 大小端判断初始化
}

void tgg_process_uninit()
{
	rte_eal_cleanup();
}

// dpdk附加参数初始化，取代f-stack的初始化，f-stack的框架限制太多
static void prc_dpdk_eal_init(int argc, char **argv)
{
	char c_flag[] = "-c1";
	char n_flag[] = "-n4";
	char mp_flag[] = "--proc-type=secondary";
	char log_flag[] = "--log-level=6";
	char *argp[argc + 4];
	uint16_t nb_ports;

	argp[0] = argv[0];
	argp[1] = c_flag;
	argp[2] = n_flag;
	argp[3] = mp_flag;
	argp[4] = log_flag;

	for (i = 1; i < argc; i++)
		argp[i + 4] = argv[i];

	argc += 4;

	ret = rte_eal_init(argc, argp);
	if (ret < 0)
		rte_panic("Cannot init EAL\n");
}


int main(int argc, char *argv[])
{
	init_core(s_dump_file);
	std::string 
	if (tgg_init_config(argc, argv) < 0) {
		printf("init config error.");
		return -1;
	}
	prc_dpdk_eal_init(argc, argv);
	// mt_init_frame(argc, argv);
	tgg_process_init();
	init_flag_for_process();
	// init_bwserver();
	tgg_gw_process(NULL);
	// uninit_bwserver();
	// TODO 进程退出时要回收资源
	tgg_process_uninit();
	printf("\n-----------main end----------\n");
	return 0;
}
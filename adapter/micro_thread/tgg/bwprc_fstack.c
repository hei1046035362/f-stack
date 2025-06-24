#include <stdio.h>
#include <stdlib.h>
#include <rte_eal.h>
#include <rte_debug.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/tgg_bwserver_fstack.h"
#include "dpdk_init.h"
#include "comm/Encrypt.hpp"
#include "comm/common.hpp"
#include "tgg_comm/tgg_conf.h"
#include "comm/log.hpp"
#include "mt_incl.h"
#include "mt_api.h"
#include "micro_thread.h"

int g_run = 1;
static const char* s_dump_file = "/var/corefiles/";//tgg_gw_bwprc_core

// 目前使用输入参数-i 指定进程编号，
// TODO 优化方向：在master中开辟一块共享内存，bwprc进程启动时去内存中查找可用的数组下标id，
                // 对应的时间在规定时间内没有更新,就视为无人使用，同时要主动检查并结束之前占用这个id的进程
int g_prc_id = -1;
extern int g_listen_fd;

static void prc_dpdk_eal_init(int argc, char **argv);

void signal_handler(int signum)
{
	if(signum == SIGINT || signum == SIGTERM) {
		if(g_run) {
			g_run = 0;
		}
	}
    LOG_WARNING("signal num:%d.", signum);
}


void tgg_sig_init()
{
    prctl(PR_SET_PDEATHSIG, SIGTERM);
	if (signal(SIGINT, signal_handler) == SIG_ERR) {
        perror("Error setting signal handler");
        exit(-1);
    }
	if (signal(SIGTERM, signal_handler) == SIG_ERR) {
        perror("Error setting signal handler");
        exit(-1);
    }
}

void tgg_process_init()
{
	tgg_sig_init();
	// tgg_iterprint_gidsbyuid();
	initOpenSSL();
	init_endians();
}

void tgg_process_uninit()
{
    clean_queue_data();
    tgg_clean_bwprc(g_prc_id);
	tgg_bwprc_uninit(TggConfigure::getInstance()->get_bwsvr_count());
}

static void prc_dpdk_eal_init(int argc, char **argv)
{
	// char c_flag[] = "-c1";
	// char n_flag[] = "-n4";
	// char mp_flag[] = "--proc-type=secondary";
	// char log_flag[] = "--log-level=6";
	// char *argp[argc + 4];
	// // uint16_t nb_ports;

	// argp[0] = argv[0];
	// argp[1] = c_flag;
	// argp[2] = n_flag;
	// argp[3] = mp_flag;
	// argp[4] = log_flag;

	// for (int i = 1; i < argc; i++)
	// 	argp[i + 4] = argv[i];

	// argc += 4;

	// int ret = rte_eal_init(argc, argp);
	// if (ret < 0)
	// 	rte_panic("Cannot init EAL\n");
    tgg_bwprc_init(TggConfigure::getInstance()->get_bwsvr_count());
}


int main(int argc, char *argv[])
{
	init_core(s_dump_file);
	if (tgg_init_config(argc, argv) < 0) {
		printf("init config error.\n");
		return -1;
	}
    if (AsyncLogger::getInstance().init(TggConfigure::getInstance()->get_log_path(), 
        TggConfigure::getInstance()->get_bwserver_log_level()) < 0) {
        printf("init log error.\n");
        return -1;
    }
    LOG_INFO("-----------bwprc start----------");
	tgg_process_init();


    // unsigned int port = TggConfigure::getInstance()->get_bwsvr_bw_port();
    // const std::string& ip = TggConfigure::getInstance()->get_bwsvr_bw_addr();
	// g_listen_fd = create_tcp_socket( port, ip.c_str(), true );
    // listen(g_listen_fd, 1024);
    // if(g_listen_fd == -1){
    //     LOG_ERROR("Port %d is in use.", port);
    //     return -1;
    // }
    // LOG_INFO("listen %d %s:%d,total server count:%d.",g_listen_fd, ip.c_str(), port, TggConfigure::getInstance()->get_bwsvr_count());

    // set_non_block( g_listen_fd );

    mt_init_frame(argc, argv);
    prc_dpdk_eal_init(argc, argv);
    g_prc_id = tgg_get_valid_bwprc(TggConfigure::getInstance()->get_bwsvr_count(), get_system_ms());
    if(g_prc_id < 0) {
        close(g_listen_fd);
        LOG_ERROR("-----------main end: no valid bwprc-id avalible----------");
        return -1;
    }
    LOG_INFO("--------bwprc started [pid:%d][prc_id:%d]---------", getpid(), g_prc_id);
    main_bw_proc(g_prc_id);
    
    print_queue_counts();
    print_mem_statistics();
	// TODO 进程退出时要回收资源
    mt_uninit_frame();
	tgg_process_uninit();
    AsyncLogger::getInstance().shutdown();
	LOG_INFO("-----------main end----------");
    AsyncLogger::getInstance().shutdown();
	return 0;
}
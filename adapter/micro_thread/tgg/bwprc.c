#include <stdio.h>
#include <stdlib.h>
#include <rte_eal.h>
#include <rte_debug.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/tgg_bwserver.h"
#include "dpdk_init.h"
#include "comm/Encrypt.hpp"
#include "comm/common.hpp"
#include "tgg_comm/tgg_conf.h"
#include "comm/log.hpp"

int g_run = 1;
static const char* s_dump_file = "/var/corefiles/";//tgg_gw_bwprc_core

// 目前使用输入参数-i 指定进程编号，
// TODO 优化方向：在master中开辟一块共享内存，bwprc进程启动时去内存中查找可用的数组下标id，
                // 对应的时间在规定时间内没有更新,就视为无人使用，同时要主动检查并结束之前占用这个id的进程
int g_prc_id = -1;
extern int g_listen_fd;
extern int g_need_authorize;
static void prc_dpdk_eal_init(int argc, char **argv);

void signal_handler(int signum)
{
    printf("gwbwprc coreid[%d] catched signal:%d\n", g_prc_id, signum);
	if(signum == SIGINT || signum == SIGTERM) {
		if(g_run) {
			g_run = 0;
		}
	}
}

static uint64_t s_last_update_time = 0;
// 定时器回调函数
void update_heart_beat() {
    uint64_t now = get_system_ms();
    if(now - s_last_update_time > BW_PRC_HEART_BEAT_UPDATE) {
        // printf("update heart beat for [PID:%d][prc_id:%d]\n", getpid(), g_prc_id);
        s_last_update_time = now;
        tgg_update_bwprc(g_prc_id, now);
    }
}

int local_eventloop_fun(void* arg) {
    if (!g_run)
        return -1;// 终止coroutine的eventloop
    update_heart_beat();
    return 0;
}

static void main_bw_proc(int prc_id)
{
    // 启动之前，先清理数据，防止上次异常退出导致资源没有正常清理
    tgg_init_bwfdx_prc(prc_id);

    for(int i = 0; i < TggConfigure::getInstance()->get_bwsvr_co_count() ; i++)
    {
            // read操作的协程
        task_t * task = (task_t*)calloc( 1,sizeof(task_t) );
        task->fd = -1;
        co_create( &(task->co),NULL,read_routine,task );
        co_resume( task->co );
    }

        // write操作的协程
    // for(int i = 0; i < TggConfigure::getInstance()->get_bwsvr_co_count() ; i++)
    // {
        stCoRoutine_t *write_co = NULL;
        co_create( &write_co, NULL, write_routine, (void*)&prc_id);
        co_resume( write_co );
    // }
    stCoRoutine_t *accept_co = NULL;
    co_create( &accept_co, NULL, accept_routine, 0 );
    co_resume( accept_co );

    // 协程启动完后，把进程的状态设置为正在运行
    tgg_set_bw_prcstatus(prc_id, 1);

    // 开始协程循环
    co_eventloop( co_get_epoll_ct(), local_eventloop_fun,0 );
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
    signal(SIGPIPE, SIG_IGN);
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
    // tgg_clean_bwprc(g_prc_id);
	tgg_bwprc_uninit(TggConfigure::getInstance()->get_bwsvr_count());
}

static void prc_dpdk_eal_init(int argc, char **argv)
{
    // char c_flag[24] = {0};
    // sprintf(c_flag, "-c%x", TggConfigure::getInstance()->get_bcore_mask());
	char n_flag[] = "-n4";
	char mp_flag[] = "--proc-type=secondary";
	char log_flag[] = "--log-level=6";
	char *argp[argc + 3];
	// uint16_t nb_ports;

	argp[0] = argv[0];
	// argp[1] = c_flag;
	argp[1] = n_flag;
	argp[2] = mp_flag;
	argp[3] = log_flag;

	for (int i = 1; i < argc; i++)
		argp[i + 3] = argv[i];

	argc += 3;

	int ret = rte_eal_init(argc, argp);
	if (ret < 0)
		rte_panic("Cannot init EAL\n");
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
    LOG_INFO("-----------bwprc[pid:%d] start----------", getpid());
	tgg_process_init();
    prc_dpdk_eal_init(argc, argv);


    unsigned int port = TggConfigure::getInstance()->get_bwsvr_bw_port();
    const std::string& ip = TggConfigure::getInstance()->get_bwsvr_bw_addr();
	g_listen_fd = create_tcp_socket( port, ip.c_str(), true );
    listen(g_listen_fd, 1024);
    if(g_listen_fd == -1){
        LOG_ERROR("Port %d is in use.", port);
        return -1;
    }
    LOG_INFO("listen %d %s:%d,total server count:%d.",g_listen_fd, ip.c_str(), port, TggConfigure::getInstance()->get_bwsvr_count());

    set_non_block( g_listen_fd );


    g_need_authorize = TggConfigure::getInstance()->get_secret_key().empty() ? 0 : 1;
    if(TggConfigure::getInstance()->get_auto_start()) {
        g_prc_id = tgg_get_bwprc_id(TggConfigure::getInstance()->get_bwsvr_count());
    } else {
        g_prc_id = tgg_get_valid_bwprc(TggConfigure::getInstance()->get_bwsvr_count(), get_system_ms());

    }
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
	tgg_process_uninit();
	LOG_INFO("-----------main end----------");
    AsyncLogger::getInstance().shutdown();
	return 0;
}
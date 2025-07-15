#include <stdio.h>
#include <stdlib.h>
#include <rte_eal.h>
#include <rte_debug.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/tgg_register.h"
#include "dpdk_init.h"
#include "comm/common.hpp"
#include "tgg_comm/tgg_conf.h"
#include "comm/log.hpp"

int g_run = 1;
static const char* s_dump_file = "/var/corefiles/";//tgg_gw_register_core

// 目前使用输入参数-i 指定进程编号，
// TODO 优化方向：在master中开辟一块共享内存，bwprc进程启动时去内存中查找可用的数组下标id，
                // 对应的时间在规定时间内没有更新,就视为无人使用，同时要主动检查并结束之前占用这个id的进程
int g_prc_id = -1;
extern int g_register_fd;

static void prc_dpdk_eal_init(int argc, char **argv);

void signal_handler(int signum)
{
	if(signum == SIGINT || signum == SIGTERM) {
		if(g_run) {
			g_run = 0;
		}
	}
    printf("signal num:%d\n", signum);
}
void sigchld_handler(int sig) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0); // 非阻塞回收所有僵尸进程[5,7](@ref)
}

static uint64_t s_last_check_time = 0;
static int* s_pid_check_times;
// 定时器回调函数
void check_bwprc()
{
    uint64_t now = get_system_ms();
    if(s_last_check_time + 5000 < now) {
        // 5s检测一次
        s_last_check_time = now;
    } else {
        // 没到检测时间，不检测
        return;
    }
    int bwcount = TggConfigure::getInstance()->get_bwsvr_count();
    for (int i = 0; i < bwcount; ++i)
    {
        if(tgg_checkif_bwprc_timeout(i, now)) {
            pid_t pid = tgg_get_bwprc_pid(i);
            if(pid > 0) {
                if (kill(pid, SIGINT) == -1) {// 不能kill -9，可能会导致其他进程死锁
                    if (errno == ESRCH) {
                        LOG_ERROR("process[%d] not exist anymore.", pid);
                    } else if (errno == EPERM) {
                        LOG_ERROR("Permission denied process[%d].", pid);
                        continue;
                    } else {
                        LOG_ERROR("kill process[%d] faild error:%d.", pid, errno);
                        // TODO 上线后这段代码要放开，防止死锁导致无法启动新的进程
                        if (kill(pid, 0) == 0) {
                            if(s_pid_check_times[i] < 3) {// 重试三次，不方便sleep，如果三个周期都没有退出，就强制结束
                                s_pid_check_times[i]++;
                                continue;
                            }
                            LOG_WARNING("Process %d exists. Sending SIGKILL...", pid);
                            // 2. 发送 SIGKILL 信号
                            if (kill(pid, SIGKILL) == 0) {
                                LOG_WARNING("SIGKILL sent successfully.");
                            } else {
                                LOG_ERROR("kill(SIGKILL) failed");
                                continue;
                            }
                        }
                        // continue;
                    }
                }
            }
            s_pid_check_times[i] = 0;
            // 构造参数数组
            // char* file_prefix = (char*)malloc(128);
            // sprintf(file_prefix, "--file-prefix=gwbwprc_%d_", i);
            // LOG_DEBUG("file prefix arg:%s", file_prefix);
            char **args = (char**)malloc((2) * sizeof(char*));
            args[0] = const_cast<char*>("gwbwprc");
            // args[1] = const_cast<char*>("--single-file-segments");
            // args[2] = file_prefix;
            args[1] = NULL; // 必须以 NULL 结尾
            custom_fork("gwbwprc", args);
            // free(file_prefix);
            free(args);
        }
    }
}

static uint64_t s_last_update_time = 0;
// 定时器回调函数
void update_register_heart_beat() {
    uint64_t now = get_system_ms();
    if(now - s_last_update_time > GW_MONITOR_HEART_BEAT) {
        // printf("update heart beat for [PID:%d][prc_id:%d]\n", getpid(), g_prc_id);
        s_last_update_time = now;
        tgg_update_gw_monitor(count_ones(TggConfigure::getInstance()->get_lcore_mask()) + 1, now);
    }
}

int local_eventloop_fun(void* arg) {
    if (!g_run || g_register_fd <= 0)
        return -1;// 终止coroutine的eventloop
    check_bwprc();
    update_register_heart_beat();
    return 0;
}

static void main_register_proc()
{
    set_non_block( g_register_fd );

    register_routine_data wdata = {
        .fd = g_register_fd,
        .ip = TggConfigure::getInstance()->get_register_addr().c_str(),
        .port = TggConfigure::getInstance()->get_register_port(),
        .seckey = "",
        .ping_interval = 25*1000
    };
    // read操作的写成
    stCoRoutine_t *read_co = NULL;
    co_create( &read_co, NULL, register_read_routine, &wdata);
    co_resume( read_co );

    wdata.bw_ip = TggConfigure::getInstance()->get_bwsvr_bw_addr().c_str();
    wdata.bw_port = TggConfigure::getInstance()->get_bwsvr_bw_port();
    // write操作的协程
    stCoRoutine_t *write_co = NULL;
    co_create( &write_co, NULL, register_write_routine, &wdata);
    co_resume( write_co );

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
    if (signal(SIGCHLD, sigchld_handler) == SIG_ERR) {
        perror("Error setting signal handler");
        exit(-1);
    }
}

void tgg_process_init()
{
	tgg_sig_init();
	// tgg_iterprint_gidsbyuid();
	init_endians();
    int bwcount = TggConfigure::getInstance()->get_bwsvr_count();
    s_pid_check_times = new int[bwcount]{0};
}

void tgg_process_uninit()
{
    delete[] s_pid_check_times;
	tgg_register_uninit();
}

static void prc_dpdk_eal_init(int argc, char **argv)
{
	char c_flag[] = "-c1";
	char n_flag[] = "-n4";
	char mp_flag[] = "--proc-type=secondary";
	char log_flag[] = "--log-level=6";
	char *argp[argc + 4];
	// uint16_t nb_ports;

	argp[0] = argv[0];
	argp[1] = c_flag;
	argp[2] = n_flag;
	argp[3] = mp_flag;
	argp[4] = log_flag;

	for (int i = 1; i < argc; i++)
		argp[i + 4] = argv[i];

	argc += 4;

	int ret = rte_eal_init(argc, argp);
	if (ret < 0)
		rte_panic("Cannot init EAL\n");
    tgg_register_init();
}

void daemon()
{
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);  // 创建失败
    if (pid > 0) exit(EXIT_SUCCESS); // 父进程退出
}

int main(int argc, char *argv[])
{
	init_core(s_dump_file);
	if (tgg_init_config(argc, argv) < 0) {
		printf("init config error.\n");
		return -1;
	}
    if (AsyncLogger::getInstance().init(TggConfigure::getInstance()->get_log_path(), 
        TggConfigure::getInstance()->get_register_log_level()) < 0) {
        printf("init log error.\n");
        return -1;
    }
    LOG_INFO("-----------register start----------");
	tgg_process_init();
    prc_dpdk_eal_init(argc, argv);

    if (tgg_setup_gw_monitor(count_ones(TggConfigure::getInstance()->get_lcore_mask()) + 1) < 0) {// 上一个进程尚未结束
        LOG_INFO("-------register exit, prev instance still running-------");
        tgg_process_uninit();
        AsyncLogger::getInstance().shutdown();
        return 0;
    }

    LOG_INFO("Try to start gwbwprc");
    check_bwprc();
    LOG_INFO("started gwbwprc.....");
    sleep(5);// 等待进程启动完成

    unsigned int port = TggConfigure::getInstance()->get_register_port();
    const std::string& ip = TggConfigure::getInstance()->get_register_addr();
    while(g_run) {
        g_register_fd = connect_tcp_socket( port, ip.c_str());
        while (g_register_fd < 0 && g_run) {// 没连上就每隔5s重连一次
            LOG_INFO("connect to register[%s:%d] failed, check if register server is alive.", ip.c_str(), port);
            int looptimes = 500; // 没连上的话，每5s重连一次注册中心
            while(g_run && looptimes > 0) {
                usleep(10);
                looptimes--;
            }
            g_register_fd = connect_tcp_socket( port, ip.c_str());
        }
        if(g_register_fd > 0) {
            LOG_INFO("connected to register %s:%d.\n", ip.c_str(), port);
        }

        main_register_proc();
        
        LOG_WARNING("connection to register is down.");
    }


	// TODO 进程退出时要回收资源
	tgg_process_uninit();
	LOG_INFO("-----------main end----------");
    AsyncLogger::getInstance().shutdown();
	return 0;
}
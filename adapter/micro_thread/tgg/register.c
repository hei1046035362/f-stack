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

static int get_exec_path(char* exe_path, const char* exec_name)
{
    ssize_t len = readlink("/proc/self/exe", exe_path, PATH_MAX - 1); // 读取符号链接[3,5,6](@ref)
    if (len == -1) {
        perror("readlink failed");
        return -1;
    }
    printf("[%s][%d]readlink exec path[%s]\n", __FILE__, __LINE__, exe_path);
    exe_path[len] = '\0';

    // 提取目录：从末尾向前找到最后一个 '/' 并截断
    char *last_slash = strrchr(exe_path, '/');
    if (last_slash != NULL) {
        memcpy(last_slash+1, exec_name, strlen(exec_name));
        last_slash[(1+strlen(exec_name))] = '\0';
        return 0;
    }
    printf("[%s][%d]invalid exec path[%s]\n", __FILE__, __LINE__, exe_path);
    return -1;
}

static void custom_fork(const char* exec_name)
{
    char exe_path[PATH_MAX];
    if(get_exec_path(exe_path, exec_name) < 0) {
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("fork failed.");
    }

    if (pid == 0) {  // 子进程
        // 1. 验证路径安全
        if (access(exe_path, X_OK) != 0) {
            perror("目标程序不可执行");
            LOG_ERROR("access filepath failed [%s].", exe_path);
            exit(EXIT_FAILURE);
        }
        
        struct stat st;
        if (stat(exe_path, &st) == -1 || !S_ISREG(st.st_mode)) {
            fprintf(stderr, "错误：无效文件\n");
            LOG_ERROR("stat filepath failed [%s]", exe_path);
            exit(EXIT_FAILURE);
        }
        LOG_INFO("launch up a new process for [%s]", exe_path);
        // 2. 构造参数数组
        char **args = (char**)malloc((2) * sizeof(char*));
        args[0] = exe_path;
        args[1] = NULL; // 必须以 NULL 结尾
        execv(exe_path, args);

        // 若execv返回，说明执行失败
        LOG_ERROR("execv failed.");
        free(args);
        exit(EXIT_FAILURE);
    }
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
            custom_fork("gwbwprc");
        }
    }
}

int local_eventloop_fun(void* arg) {
    if (!g_run)
        return -1;// 终止coroutine的eventloop
    check_bwprc();
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

    unsigned int port = TggConfigure::getInstance()->get_register_port();
    const std::string& ip = TggConfigure::getInstance()->get_register_addr();
    g_register_fd = connect_tcp_socket( port, ip.c_str());
    while (g_register_fd < 0 && g_run) {// 没连上就每隔5s重连一次
        LOG_INFO("connect to register[%s:%d] failed, check if server is alive.", ip.c_str(), port);
        poll(NULL, 0, 5000);// sleep 5s
        g_register_fd = connect_tcp_socket( port, ip.c_str());
    }

    LOG_INFO("connect to register %s:%d.\n", ip.c_str(), port);

    if(g_run) {
        main_register_proc();
    }


	// TODO 进程退出时要回收资源
	tgg_process_uninit();
	LOG_INFO("-----------main end----------");
    AsyncLogger::getInstance().shutdown();
	return 0;
}
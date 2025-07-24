#include <stdio.h>
#include <stdlib.h>
#include <rte_eal.h>
#include <rte_debug.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/fcntl.h>
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
static int s_bwcount = 0;
// 目前使用输入参数-i 指定进程编号，
// TODO 优化方向：在master中开辟一块共享内存，bwprc进程启动时去内存中查找可用的数组下标id，
                // 对应的时间在规定时间内没有更新,就视为无人使用，同时要主动检查并结束之前占用这个id的进程
// int g_prc_id = -1;
extern int g_register_fd;

static void prc_dpdk_eal_init(int argc, char **argv);

static int sig_pipe[2];

static void start_gwbwprc()
{
    char **args = (char**)malloc((2) * sizeof(char*));
    args[0] = const_cast<char*>("gwbwprc");
    args[1] = NULL; // 必须以 NULL 结尾
    custom_fork("gwbwprc", args);
    free(args);
}


void signal_handler(int signum)
{
    printf("gwregister catched signal:%d\n", signum);
	if(signum == SIGINT || signum == SIGTERM) {
		if(g_run) {
			g_run = 0;
		}
	}
}
void sigchld_handler(int sig) {
    int saved_errno = errno;
    char buf[16];
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) { // 非阻塞回收所有僵尸进程[5,7](@ref)
        if(g_run && TggConfigure::getInstance()->get_auto_start()) {
            // 监控到子进程退出，立刻再启动一个
            int len = snprintf(buf, sizeof(buf), "%d\n", pid);
            write(sig_pipe[1], buf, len);
            if (WIFEXITED(status)) {
                printf("register child %d exit normal, exit code: %d\n", pid, WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                printf("register child %d exit by signal: %d\n", pid, WTERMSIG(status));
            }
        }
    }
    errno = saved_errno;
}

int deal_sigchild(void* arg) {
    struct pollfd pfds[1];
    pfds[0].fd = sig_pipe[0]; // 管道读端
    pfds[0].events = POLLIN;  // 监听可读事件
    // 非阻塞检查管道（超时=0立即返回）
    int ret = poll(pfds, 1, 0);
    if (ret > 0 && (pfds[0].revents & POLLIN)) {
        // stCoEpoll_t* ctx = (stCoEpoll_t*)arg;
        char pid_buf[32];
        ssize_t nread;

        // 检查管道是否有数据（非阻塞读取）
        while (g_run && (nread = read(sig_pipe[0], pid_buf, sizeof(pid_buf)-1)) > 0) {
            pid_buf[nread] = '\0';
            pid_t dead_pid = atoi(pid_buf);
            for (int i = 0; i < s_bwcount && g_run; ++i)// 0号进程 自己不能监控自己，由service监控 
            {
                pid_t bwprc_pid = tgg_get_bwprc_pid(i);
                LOG_WARNING("sigchild from register[%d] pid:%d bwprc_pid:%d\n", i, dead_pid, bwprc_pid);// 信号处理函数中不能用日志，可能导致崩溃，日志类中有可重入函数
                if (dead_pid == bwprc_pid) {
                    tgg_clean_bwprc(i);
                    start_gwbwprc();
                }
            }
        }
    }
    return 0;
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
    // int bwcount = TggConfigure::getInstance()->get_bwsvr_count();
    for (int i = 0; i < s_bwcount; ++i)
    {
        if(tgg_checkif_bwprc_timeout(i, now)) {
            pid_t pid = tgg_get_bwprc_pid(i);
            if(pid > 0) {
                LOG_INFO("prc_id[%d] pid[%d] heartbeat timeout, try to kill.", i, pid);
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
            tgg_clean_bwprc(i);
            // 构造参数数组
            start_gwbwprc();
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
    if(TggConfigure::getInstance()->get_auto_start()) {
        check_bwprc();
        update_register_heart_beat();
        deal_sigchild(arg);
    }
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
    s_bwcount = TggConfigure::getInstance()->get_bwsvr_count();
    s_pid_check_times = new int[s_bwcount]{0};
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

static int check_if_all_child_up()
{
    // int bwcount = TggConfigure::getInstance()->get_bwsvr_count();
    for (int i = 0; i < s_bwcount; ++i)
    {
        if(tgg_check_bwprc_up(i) <= 0) {
            return 0;
        }
    }
    return 1;
}
static void kill_all_child()
{
    // int bwcount = TggConfigure::getInstance()->get_bwsvr_count();
    for (int i = 0; i < s_bwcount; ++i)
    {
        pid_t pid = tgg_get_bwprc_pid(i);
        if(pid <= 0) {
            continue;
        }
        LOG_INFO("try to kill prc_id[%d] pid[%d].", i, pid);
        if (kill(pid, SIGINT) == -1) {// 不能kill -9，可能会导致其他进程死锁
            if (errno == ESRCH) {
                LOG_ERROR("core_id[%d] process[%d] not exist anymore.", i, pid);
            } else if (errno == EPERM) {
                LOG_ERROR("Permission denied process[%d] core_id[%d].", pid, i);
            } else {
                LOG_ERROR("kill core_id[%d] process[%d] faild error:%d.", i, pid, errno);
                int wait_times = 500;// 最长等待5s，还没有退出的话，就发送kill -9
                while (kill(pid, 0) == 0) {// 进程还存在
                    if (wait_times > 0) {
                        usleep(10000);
                        wait_times--;
                        continue;
                    }
                    LOG_WARNING("core_id[%d] Process %d exists. Sending SIGKILL...", i, pid);
                    // 2. 发送 SIGKILL 信号
                    if (kill(pid, SIGKILL) == 0) {
                        LOG_WARNING("SIGKILL sent successfully.");
                    } else {
                        LOG_ERROR("kill(SIGKILL) failed");
                    }
                    break;
                }
            }
        }
        tgg_clean_bwprc(i);
    }

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
    LOG_INFO("-----------register[pid:%d] start----------", getpid());
	tgg_process_init();
    prc_dpdk_eal_init(argc, argv);

    if(TggConfigure::getInstance()->get_auto_start()) {
        pipe2(sig_pipe, O_NONBLOCK | O_CLOEXEC);
        kill_all_child();// 启动时，先把之前还在运行的进程杀掉
        if (tgg_setup_gw_monitor(count_ones(TggConfigure::getInstance()->get_lcore_mask()) + 1) < 0) {// 上一个进程尚未结束
            LOG_INFO("-------register exit, prev instance still running-------");
            tgg_process_uninit();
            AsyncLogger::getInstance().shutdown();
            return 0;
        }

        LOG_INFO("Try to start gwbwprc");
        check_bwprc();
        LOG_INFO("started gwbwprc.....");
        // 检查子进程是否已全部启动
        int check_times = 100;// 最多等待10s
        while (g_run && check_times > 0) {
            if(check_if_all_child_up() > 0) {
                break;
            }
            check_times--;
            usleep(10000);
        }
        LOG_INFO("start gwbwprc done, check_times:%d.", check_times);
        if(!check_if_all_child_up()) {
            kill_all_child();
            g_run = 0;
            LOG_FATAL("not all gwbwprc is working on the beginning, exiting...");
        }
        sleep(2);// (兜底)等待gwbwprc的 socket就绪(服务端连gwbwprc的时候，一次连不上，就不连了，但是这时候gwbwprc的socket还没有完全就绪)
    }
    unsigned int port = TggConfigure::getInstance()->get_register_port();
    const std::string& ip = TggConfigure::getInstance()->get_register_addr();
    while(g_run) {
        g_register_fd = connect_tcp_socket( port, ip.c_str());
        while (g_register_fd < 0 && g_run) {// 没连上就每隔5s重连一次
            LOG_INFO("connect to register[%s:%d] failed, check if register server is alive.", ip.c_str(), port);
            int looptimes = 500; // 没连上的话，每5s重连一次注册中心
            while(g_run && looptimes > 0) {
                usleep(10000);
                looptimes--;
            }
            g_register_fd = connect_tcp_socket( port, ip.c_str());
        }
        if(g_register_fd > 0) {
            LOG_INFO("connected to register %s:%d.", ip.c_str(), port);
        }

        main_register_proc();
        
        LOG_WARNING("connection to register is down.");
    }
    if(TggConfigure::getInstance()->get_auto_start()) {

        kill_all_child();
        wait_all_child_exit();
    }
	// TODO 进程退出时要回收资源
	tgg_process_uninit();
	LOG_INFO("-----------main end----------");
    AsyncLogger::getInstance().shutdown();
	return 0;
}
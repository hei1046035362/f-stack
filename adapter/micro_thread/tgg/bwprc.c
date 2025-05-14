#include <stdio.h>
#include <stdlib.h>
// #include "mt_incl.h"
// #include "micro_thread.h"
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <sys/wait.h>
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/tgg_struct.h"
#include "tgg_comm/tgg_bwserver.h"
#include "dpdk_init.h"
#include "tgg_comm/tgg_transport.h"
#include "comm/Encrypt.hpp"
#include "tgg_comm/tgg_bw_cache.h"
#include "tgg_comm/tgg_bwcomm.h"
#include "tgg_comm/tgg_conf.h"
#include <vector>
#include "tgg_comm/tgg_cliprc.h"
#include "tgg_comm/tgg_conf.h"

// 绝对路径
const char* f_stack_ini = "/data/code/f-stack/config.ini";

int g_run = 1;
static const char* s_dump_file = "/var/corefiles/tgg_gw_bwprc_core";
static pid_data *s_pids = NULL;
static int s_pid_count = 0;
static unsigned long long s_heart_beat_interval = 5*1000; // 心跳间隔5s
int g_prc_id = -1;

extern const char* g_rte_malloc_type;
extern struct rte_mempool* g_mempool_read;
extern struct rte_mempool* g_mempool_write;
extern struct rte_mempool* g_mempool_bwrcv;
extern int g_listen_fd;

static void prc_dpdk_eal_init(int argc, char **argv);

void signal_handler(int signum)
{
	if(signum == SIGINT || signum == SIGTERM) {
		if(g_run) {
			g_run = 0;
			// uninit_bwserver();
			// rte_eal_cleanup();
        	// prc_exit(0, "catched signal:%d\n", signum);
		}
	}
	if (signum == SIGCHLD) {
        int status;
        pid_t terminated_pid;
        while ((terminated_pid = waitpid(-1, &status, WNOHANG)) > 0) {
            if (WIFEXITED(status)) {
            	// 正常退出暂时不管，正常退出资源一般都正常回收了
                printf("Child %d exited normally with status %d\n", terminated_pid, WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                printf("Child %d terminated by signal %d\n", terminated_pid, WTERMSIG(status));
				for (int i = 0; i < s_pid_count; ++i)
				{
					if(terminated_pid == s_pids[i].pid) {
        				tgg_set_bw_prcstatus(i, 0);
        				tgg_init_bwfdx_prc(g_prc_id);
					}
				}
            }
        }
    }
	if (signum > SIGUSR1)
	{
		pid_t pid = signum - SIGUSR1;
		for (int i = 0; i < s_pid_count; ++i)
		{
			if (s_pids[i].pid == pid) {
				// 有信号就重置心跳计数
				s_pids[i].heart_beat = 0;
			}
		}
	}
}

int local_eventloop_fun(void* arg) {
    if (!g_run)
        return -1;// 终止coroutine的eventloop
    return 0;
}

void fork_oneprocess(void* data, int argc, char **argv)
{
   	g_prc_id = *((int*)data);
	pid_t pid = fork();
    
    if (pid < 0) {
        perror("Fork failed.");
        prc_exit(EXIT_FAILURE, "Fork failed.\n");
    } else if (pid == 0) {
        // 子进程
        prc_dpdk_eal_init(argc, argv);

        // 启动之前，先清理数据，防止上次异常退出导致资源没有正常清理
        tgg_init_bwfdx_prc(g_prc_id);

        for(int i = 0; i < TggConfigure::getInstance()->get_bwsvr_co_count() ; i++)
        {
      		// read操作的协程
            task_t * task = (task_t*)calloc( 1,sizeof(task_t) );
            task->fd = -1;
            co_create( &(task->co),NULL,read_routine,task );
            co_resume( task->co );
        }

        // write操作的协程
        stCoRoutine_t *write_co = NULL;
        co_create( &write_co, NULL, write_routine, data );
        co_resume( write_co );

        stCoRoutine_t *accept_co = NULL;
        co_create( &accept_co, NULL, accept_routine, 0 );
        co_resume( accept_co );

        // 协程启动完后，把进程的状态设置为正在运行
        tgg_set_bw_prcstatus(g_prc_id, 1);

        // 开始协程循环
        co_eventloop( co_get_epoll_ct(), local_eventloop_fun,0 );


        prc_exit(0, "child exit.\n");
    } else {
        // 父进程
        // s_pids[g_prc_id].idx = g_prc_id;
        s_pids[g_prc_id].pid = pid;
        s_pids[g_prc_id].heart_beat = 0;
        // *ppid = pid;
    }
}

void fork_processes(int argc, char **argv)
{
	s_pids = (pid_data*)malloc(s_pid_count * sizeof(pid_data));
    
    // 创建子进程
    for (int i = 0; i < s_pid_count; i++) {
    	fork_oneprocess(&i, argc, argv);
    }
    prc_dpdk_eal_init(argc, argv);
}

// 检查心跳
void check_heart_beat(pid_data* pdata)
{
	// 心跳间隔大于5min钟，就判定进程假死了，直接重启进程
	if (pdata->heart_beat > 60 * s_heart_beat_interval)
	{
		// 重启进程
		kill(pdata->pid, SIGTERM);
        wait(NULL);
	}
    pdata->heart_beat = 0;
}
void monitor_process(int argc, char **argv)
{
	// 定期检查子进程状态
    while (g_run) {
        sleep(1); // 每秒检查一次
        for (int i = 0; i < s_pid_count; i++) {
            int status;
            pid_t result = waitpid(s_pids[i].pid, &status, WNOHANG); // 非阻塞等待
            
            if (result == 0) {
                printf("子进程[%d]仍在运行\n", s_pids[i].pid);
            } else if (result == -1) {
                // 出现错误
                perror("waitpid 错误");
            } else {
                if(g_run) {
                    // 子进程已结束
                    printf("子进程 (PID: %d) 异常结束，重启进程.\n", s_pids[i].pid);
                    // 重新创建
                    fork_oneprocess(&i, argc, argv);
                    // pids[i] = s_pids[--s_pid_count]; // 移除已结束的子进程
                    // i--; // 调整索引，以便正确检查下一个进程
                }
                if (result == s_pids[i].pid) {
                    if (WIFSTOPPED(status)) {  // 子进程暂停（可能因死锁卡在锁操作）
                        printf("子进程[%d]可能死锁，终止信号：%d\n", s_pids[i].pid, WSTOPSIG(status));
                    } else if (WIFSIGNALED(status)) {  // 子进程被信号终止
                        printf("子进程[%d]被信号终止：%d\n", s_pids[i].pid, WTERMSIG(status));
                    } 
                }
            }
            //pdata->heart_beat++;
            // 
        }
    }
    pid_t pid;
    int status = 0;
    while ((pid = wait(&status)) != -1) {
        printf("子进程 %d 退出，状态码: %d\n", pid, WEXITSTATUS(status));
    }
    free(s_pids);
    printf("所有子进程已结束，父进程退出.\n");

}

void tgg_sig_init()
{
	if (signal(SIGINT, signal_handler) == SIG_ERR) {
        perror("Error setting signal handler");
        exit(-1);
    }
	if (signal(SIGTERM, signal_handler) == SIG_ERR) {
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
	rte_eal_cleanup();
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
    tgg_secondary_init();
}


int main(int argc, char *argv[])
{
	init_core(s_dump_file);
	if (tgg_init_config(argc, argv) < 0) {
		printf("init config error.");
		return -1;
	}
	// mt_init_frame(argc, argv);
	tgg_process_init();


    unsigned int port = TggConfigure::getInstance()->get_bwsvr_bw_port();
    const std::string& ip = TggConfigure::getInstance()->get_bwsvr_bw_addr();
	g_listen_fd = create_tcp_socket( port, ip.c_str(), true );
    s_pid_count = TggConfigure::getInstance()->get_bwsvr_count();
    listen( g_listen_fd,1024 );
    if(g_listen_fd == -1){
        printf("Port %d is in use\n", port);
        return -1;
    }
    printf("listen %d %s:%d,server count:%d\n",g_listen_fd, ip.c_str(), port, s_pid_count);

    set_non_block( g_listen_fd );



    // 启动bwserver服务进程组
	fork_processes(argc, argv);
	// 启动透传线程
	init_bwtrans();
	// init_bwserver();
	// 主进程循环监控 bwserver服务进程组，循环
	monitor_process(argc, argv);
	// 主进程结束，开始销毁资源
	uninit_bwtrans();
	// tgg_gw_process(NULL);
	// uninit_bwserver();
	// TODO 进程退出时要回收资源
	tgg_process_uninit();
	printf("\n-----------main end----------\n");
	return 0;
}
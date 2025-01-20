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
#include "tgg_comm/tgg_transport.h"
#include "comm/Encrypt.hpp"
#include "tgg_comm/tgg_bwserver.h"
#include "tgg_comm/tgg_bw_cache.h"
#include "tgg_comm/tgg_bwcomm.h"
#include "tgg_comm/tgg_conf.h"
#include <vector>
#include "tgg_comm/tgg_cliprc.h"
// 绝对路径
const char* f_stack_ini = "/data/code/f-stack/config.ini"

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
				s_pids[i].heard_beat = 0;
			}
		}
	}
}

static int tgg_process_bwrcv()
{
	tgg_bw_data* bdata = NULL;
	if (tgg_dequeue_bwrcv(&bdata) < 0) {
	    // 队列空
		return 1;
	}
	if (!bdata) {
		return 0;
	}
	if (!g_run) {
		rte_free(bdata->data);
		memset(bdata, 0, sizeof(tgg_bw_data));
		rte_mempool_put(g_mempool_bwrcv, (void*)bdata);
		return -1;
	}
	return 0;
}

void fork_oneprocess(void* data)
{
   	g_prc_id = *((int*)data);
	pid_t pid = fork();
    
    if (pid < 0) {
        perror("Fork failed.");
        prc_exit(EXIT_FAILURE, "Fork failed.\n");
        // exit(EXIT_FAILURE);
    } else if (pid == 0) {
        // 子进程

        // 启动之前，先清理数据，防止上次异常退出导致资源没有正常清理
        tgg_init_bwfdx_prc(g_prc_id);

        for(int i = 0; i < ; i++)
        {
      		// read操作的协程
            task_t * task = (task_t*)calloc( 1,sizeof(task_t) );
            task->fd = -1;
            co_create( &(task->co),NULL,read_routine,task );
            co_resume( task->co );
        }

        // write操作的协程
        co_create( &(task->co),NULL,write_routine,data );
        co_resume( task->co );

        stCoRoutine_t *accept_co = NULL;
        co_create( &accept_co,NULL,accept_routine,0 );
        co_resume( accept_co );

        // 协程启动完后，把进程的状态设置为正在运行
        tgg_set_bw_prcstatus(g_prc_id, 1);

        // 开始协程循环
        co_eventloop( co_get_epoll_ct(),0,0 );


		// init_bwserver();
		// tgg_gw_process(NULL);
		uninit_bwserver();
        prc_exit(0, "child exit.\n");
    } else {
        // 父进程
        // s_pids[g_prc_id].idx = g_prc_id;
        s_pids[g_prc_id].pid = pid;
        // *ppid = pid;
    }
}

void fork_processes()
{
	s_pids = (pid_data*)malloc(s_pid_count * sizeof(pid_data));
    
    // 创建子进程
    for (int i = 0; i < s_pid_count; i++) {
    	fork_oneprocess(&i);
    }
}

// 检查心跳
void check_heart_beat(pid_data* pdata)
{
	// 心跳间隔大于5min钟，就判定进程假死了，直接重启进程
	if (pdata->heard_beat > 60 * s_heart_beat_interval)
	{
		// 重启进程
		kill(pdata->pid, SIGTERM);
        wait(NULL);
	}
}
void monitor_process()
{
	// 定期检查子进程状态
    while (g_run) {
        sleep(1); // 每秒检查一次
        for (int i = 0; i < s_pid_count; i++) {
            int status;
            pid_t result = waitpid(s_pids[i].pid, &status, WNOHANG); // 非阻塞等待
            
            if (result == 0) {
                // 子进程仍在运行
                printf("子进程 (PID: %d) 仍在运行.\n", s_pids[i].pid);
                check_heart_beat(&s_pids[i]);
            } else if (result == -1) {
                // 出现错误
                perror("waitpid 错误");
            } else {
                // 子进程已结束
                printf("子进程 (PID: %d) 已结束.\n", s_pids[i].pid);
                // 重新创建
                fork_oneprocess(&i);
                // pids[i] = s_pids[--s_pid_count]; // 移除已结束的子进程
                // i--; // 调整索引，以便正确检查下一个进程
            }

            // 
        }
    }
    free(s_pids);
    printf("所有子进程已结束，父进程退出.\n");

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
    // 子进程退出
    struct sigaction sa;
    sa.sa_handler = child_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	if (sigaction(SIGCHLD, &sa, NULL) == SIG_ERR) {
        perror("Error setting signal handler");
        prc_exit(-1, "Error setting signal handler");
    }

}

void tgg_process_init()
{
	tgg_sig_init();
	tgg_secondary_init();
	tgg_iterprint_gidsbyuid();
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



	g_listen_fd = create_tcp_socket( port,ip,true );
    listen( g_listen_fd,1024 );
    if(g_listen_fd==-1){
        printf("Port %d is in use\n", port);
        return -1;
    }
    printf("listen %d %s:%d\n",g_listen_fd,ip,port);

    set_non_block( g_listen_fd );



    // 启动bwserver服务进程组
	fork_processes();
	// 启动透传线程
	init_bwtrans();
	// init_bwserver();
	// 主进程循环监控 bwserver服务进程组，循环
	monitor_process();
	// 主进程结束，开始销毁资源
	uninit_bwtrans();
	// tgg_gw_process(NULL);
	// uninit_bwserver();
	// TODO 进程退出时要回收资源
	tgg_process_uninit();
	printf("\n-----------main end----------\n");
	return 0;
}
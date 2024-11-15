#include <stdio.h>
#include <stdlib.h>
#include "mt_incl.h"
#include "micro_thread.h"
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <sys/wait.h>
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/tgg_struct.h"
#include "dpdk_init.h"
#include "cmd/WsConsumer.h"
#include "comm/Encrypt.hpp"
#include "bwserver.h"


#include <vector>


static int g_run = 1;

static const char* s_dump_file = "/var/corefiles/tgg_gw_process_core";
static pid_data *s_pids = NULL;
static int s_pid_count = 0;
static unsigned long long s_heart_beat_interval = 5*1000; // 心跳间隔5s

extern const char* g_rte_malloc_type;
extern struct rte_mempool* g_mempool_read;
extern struct rte_mempool* g_mempool_write;
extern struct rte_mempool* g_mempool_bwrcv;

std::vector<std::string> g_redis_clusternodes = {
    "54.241.112.155:7001",
    "54.241.112.155:7002",
};
const char* g_redis_pwd = "bZSCEI3VyV";


void signal_handler(int signum)
{
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

void tgg_process_read_data(void* arg)
{
	WsConsumer cons;
	cons.ConsumerData(arg);
}

static int tgg_process_read()
{
	tgg_read_data* rdata = NULL;
	if (tgg_dequeue_read(&rdata) < 0) {
	    // 队列空
		return 1;
	}
	if (!rdata) {
		return 0;
	}
	// 等待可用线程
	while (mt_nomore_thread() && g_run) {
		usleep(10);
		continue;
	}
	// 进程要退出了，不再启动新的线程，释放内存就退出
	if (!g_run) {
		rte_free(rdata->data);
		memset(rdata, 0, sizeof(tgg_read_data));
		rte_mempool_put(g_mempool_read, (void*)rdata);
		return -1;
	}

	mt_start_thread((void *)tgg_process_read_data, (void *)rdata);
	return 0;
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
	// 等待可用线程
	while (mt_nomore_thread() && g_run) {
		usleep(10);
		continue;
	}
	// 进程要退出了，不再启动新的线程，释放内存就退出
	if (!g_run) {
		rte_free(bdata->data);
		memset(bdata, 0, sizeof(tgg_bw_data));
		rte_mempool_put(g_mempool_bwrcv, (void*)bdata);
		return -1;
	}

	mt_start_thread((void *)tgg_process_bwrcv_data, (void *)bdata);
	return 0;
}


int tgg_gw_process(void* data)
{
	unsigned long long last_time = mt_time_ms();
	while (g_run) {
		// 处理客户端连接和转发数据
		while (g_run) {
			// 优先保证客户端能上报
			if (tgg_process_read() > 0)
			{
				break;
			}
		}
		// 处理从bw过来的数据
		if (tgg_process_bwrcv() > 0)
			usleep(10);

		// 发送心跳
		// unsigned long long cur_time = mt_time_ms();
		// if (cur_time > last_time + s_heart_beat_interval) {
		// 	last_time = cur_time;
		// 	// 向父进程发送 SIGUSR1 信号
    	// 	kill(getppid(), SIGUSR1 + getpid());
		// }
	}
	return 0;
}

void fork_oneprocess(pid_t* ppid, void* data)
{
	pid_t pid = fork();
    
    if (pid < 0) {
        perror("创建子进程失败");
        exit(EXIT_FAILURE);
    } else if (pid == 0) {
        // 子进程
        tgg_gw_process(data);
        exit(0); // 子进程完成后退出
    } else {
        // 父进程
        *ppid = pid;
    }
}

void fork_processes()
{
	s_pids = (pid_data*)malloc(s_pid_count * sizeof(pid_data));
    
    // 创建子进程
    for (int i = 0; i < s_pid_count; i++) {
    	fork_oneprocess(&(s_pids[i].pid), &i);
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
                fork_oneprocess(&s_pids[i].pid, &i);
                // pids[i] = s_pids[--s_pid_count]; // 移除已结束的子进程
                // i--; // 调整索引，以便正确检查下一个进程
            }

            // 
        }
    }
    free(s_pids);
    printf("所有子进程已结束，父进程退出.\n");

}

void tgg_process_init()
{
	if(tgg_init_uidgid(g_redis_clusternodes, g_redis_pwd) < 0) {
		rte_exit(EXIT_FAILURE, "Init uidgid hash failed.\n");
	}
	initOpenSSL();
}

int main(int argc, char *argv[])
{
	init_core(s_dump_file);
	mt_init_frame(argc, argv);
	tgg_process_init();
	tgg_gw_process(NULL);
	init_bwserver();
	uninit_bwserver();
	// TODO 进程退出时要回收资源
	//tgg_process_uninit();
}
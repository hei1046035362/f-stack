#include <stdio.h>
#include <stdlib.h>
#include "mt_incl.h"
#include "micro_thread.h"
#include <rte_mempool.h>
#include "tgg_common.h"
#include "tgg_struct.h"
#include "tgg_init.h"

static int g_run = 1;

static const char* s_dump_file = "/var/corefiles/tgg_gw_process_core"
static pid_data *s_pids = NULL;
static int s_pid_count = 0;
static unsigned long long s_heart_beat_interval = 5*1000; // 心跳间隔5s

extern const char* g_rte_malloc_type;
extern const struct rte_mempool* g_mempool_read;
extern const struct rte_mempool* g_mempool_write;


void signal_handler(int signum)
{
	if (signum > SIGUSR1)
	{
		pid_t pid = signum - SIGUSR1;
		for (int i = 0; i < s_pid_count; ++i)
		{
			if (s_pids[i]->pid == pid) {
				// 有信号就重置心跳计数
				s_pids[i]->heard_beat = 0;
			}
		}
	}
}

void tgg_process(void* arg)
{
	tgg_read_data* rdata = (tgg_read_data*)arg
}

static int tgg_dequeue()
{
	tgg_read_data* rdata = NULL;
	if (tgg_dequeue_read(&rdata) < 0) {
	    	// 队列空
		usleep(10);
		return 0;
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
		memset(rdata->data, 0, rdata->data_len);
		rdata->data_len = 0;
		rdata->fd = 0;
		rte_mempool_put(g_mempool_read, (void*)rdata);
		return -1;
	}

	mt_start_thread((void *)tgg_process, (void *)rdata);
	return 0;
}

int tgg_gw_process(void* data)
{
	unsigned long long last_time = mt_time_ms();
	while (g_run) {
		// 处理客户端连接和转发数据
		if (tgg_dequeue() < 0)
			break;

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
	s_pids = malloc(s_pid_count * sizeof(pid_data));
    
    // 创建子进程
    for (int i = 0; i < s_pid_count; i++) {
    	fork_oneprocess(&s_pids[i]->pid, &i);
    }
    return 0;
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
            pid_t result = waitpid(s_pids[i]->pid, &status, WNOHANG); // 非阻塞等待
            
            if (result == 0) {
                // 子进程仍在运行
                printf("子进程 (PID: %d) 仍在运行.\n", pids[i]);
                check_heart_beat(s_pids[i]);
            } else if (result == -1) {
                // 出现错误
                perror("waitpid 错误");
            } else {
                // 子进程已结束
                printf("子进程 (PID: %d) 已结束.\n", pids[i]);
                // 重新创建
                fork_oneprocess(&s_pids[i]->pid, &i);
                // pids[i] = s_pids[--s_pid_count]; // 移除已结束的子进程
                // i--; // 调整索引，以便正确检查下一个进程
            }

            // 
        }
    }
    free(pids);
    printf("所有子进程已结束，父进程退出.\n");

}

int main(int argc, char *argv[])
{
	init_core(s_dump_file);
	mt_init_frame(argc, argv);
	tgg_process_init();
	tgg_gw_process();
	tgg_process_uninit();
}
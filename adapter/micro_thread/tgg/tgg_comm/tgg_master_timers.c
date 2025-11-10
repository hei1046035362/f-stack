#include "tgg_master_timers.h"
#include "tgg_common.h"
#include <sys/wait.h>
#include <sys/prctl.h>
#include "comm/log.hpp"
#include "comm/common.hpp"
#include "tgg_comm/tgg_conf.h"
#include "tgg_bw_cache.h"
#include "tgg_ip_filter.h"
#include "tgg_common.h"
#include "tgg_bw_cache.h"
#include <poll.h>

extern int g_core_id;

// 进程是否退出  master进程退出不需要做什么事情，但是secondary退出前必须要释放他持有的内存
extern int g_run_status;
extern int g_monitor_count;
// using namespace NS_MICRO_THREAD;
extern int sig_pipe[2];// 信号处理放入主函数异步处理，信号函数中很多系统函数不能调用，会崩溃死锁

// static uint64_t s_last_check_time = 0;
extern int* g_pid_check_times;

// 不在信号函数中左复杂的操作，改为管道传递给主线程
static void deal_sigchild(struct rte_timer* tim, void* arg)
{
    struct pollfd pfds[1];
    pfds[0].fd = sig_pipe[0]; // 管道读端
    pfds[0].events = POLLIN;  // 监听可读事件
    // 非阻塞检查管道（超时=0立即返回）
    int ret = poll(pfds, 1, 0);
    if (ret > 0 && (pfds[0].revents & POLLIN)) {
        char pid_buf[64];
        ssize_t nread;

        // 检查管道是否有数据（非阻塞读取）
        while (g_run_status && (nread = read(sig_pipe[0], pid_buf, sizeof(pid_buf)-1)) > 0) {
            pid_buf[nread] = '\0';
            pid_t dead_pid = 0, sig_num = 0;
            if (sscanf(pid_buf, "%d_%d", &dead_pid, &sig_num) == 2) {
                LOG_WARNING("deal signo[%d] for pid[%d].", sig_num, dead_pid);
            } else {
                LOG_FATAL("error format[%s] for sig_pip", pid_buf);
            }
            // 监控到子进程退出，立刻再启动一个
            for (int i = 0; i < g_monitor_count; ++i)// 0号进程 自己不能监控自己，由service监控 
            {
                if(i == g_core_id) {// primary进程 自己不能监控自己，由service监控 
                    continue;
                }
                pid_t monitor_pid = tgg_get_gw_monitor_pid(i);
                // printf("sigchild from pid:%d monitor_pid:%d\n", pid, monitor_pid);
                if (dead_pid == monitor_pid) {
                    LOG_WARNING("gwrcv child[%d] pid:%d exit.", i, dead_pid);
                    tgg_clean_gw_monitor(i);
                    pid_t pid = -1;
                    if(i < g_monitor_count-2) {
                        if(sig_num == 9) {// gwrcv进程收到信号9时，所有进程都退出，未正常退出的gwrcv无法正常启动
                            g_run_status = 0;
                            LOG_FATAL("core_id[%d] pid[%d] catched an sigkill, all process will exit.", i, dead_pid);
                            return;
                        }
                        pid = start_gwrcv_sendary(i);
                    } else if (i == g_monitor_count-2) {
                        pid = start_gwcliprc(i);
                    } else if (i == g_monitor_count - 1) {
                        pid = start_register(i);
                    }
                    if(pid < 0) {
                        LOG_ERROR("fork for lcore[%d] failed", i);
                    }
                    if(tgg_setup_gw_monitor(i, pid) < 0) {
                        LOG_ERROR("setup monitor for lcore[%d] failed, pid:%d", i, pid);
                    }
                }
            }
        }
    }
}


// 定时器回调函数
void check_gw_monitor(struct rte_timer* tm, void* arg)
{
    uint64_t now = get_system_ms();
    for (int i = 0; i < g_monitor_count; ++i)
    {
        if(i == g_core_id) {// primary进程 自己不能监控自己，由service监控 
            continue;
        }
        if(tgg_checkif_gw_monitor_timeout(i, now)) {
            pid_t pid = tgg_get_gw_monitor_pid(i);
            if(pid > 0) {
                LOG_INFO("core_id[%d] pid[%d] heartbeat timeout, try to kill.", i, pid);
                if(i < g_monitor_count-2) { // gwrcv 强制结束会导致rte_timer_reset死锁,
                                            //      但是通常死锁时并不会在mt_sleep中，所以这里任然待观察
                    if(g_pid_check_times[i] < 3) {// gwrcv由于自身框架限制，更新并不及时，重试三次，不方便sleep，如果三个周期都没有退出，再结束
                        g_pid_check_times[i]++;
                        continue;
                    }
                }
                if (kill(pid, SIGINT) == -1) {// 不能kill -9，可能会导致其他进程死锁
                    if (errno == ESRCH) {
                        LOG_ERROR("core_id[%d] process[%d] not exist anymore.", i, pid);
                    } else if (errno == EPERM) {
                        LOG_ERROR("Permission denied process[%d] core_id[%d].", pid, i);
                        continue;
                    } else {
                        LOG_ERROR("kill core_id[%d] process[%d] faild error:%d.", i, pid, errno);
                        // continue;
                    }
                }
                if(i < g_monitor_count-2) {// gwrcv不管进程在不在我们都不会重启，因此检测次数要重置
                    LOG_ERROR("check_times[%d] beyond max check_times[3]", g_pid_check_times[i]);
                    g_pid_check_times[i] = 0;
                    g_run_status = 0;// gwrcv的子进程超过三个检测周期了，主进程直接退出，让服务重启
                }
                // TODO 上线后这段代码要放开，防止死锁导致无法启动新的进程
                if (kill(pid, 0) == 0) {
                    if(i < g_monitor_count-2) { // gwrcv 强制结束会导致rte_timer_reset死锁,
                                                //      但是通常死锁时并不会在mt_sleep中，所以这里任然待观察
                        LOG_ERROR("kill gwrcv[%d] failed, pid[%d] still exist.", i, pid);
                        continue;
                    }
                    if(g_pid_check_times[i] < 3) {// 重试三次，不方便sleep，如果三个周期都没有退出，就强制结束
                        g_pid_check_times[i]++;
                        continue;
                    }
                    LOG_WARNING("core_id[%d] Process %d exists. Sending SIGKILL...", i, pid);
                    // 2. 发送 SIGKILL 信号  理论上死锁发生时，是整个dpdk都死锁住了，杀掉一个进程并不起作用，应该要整个程序重启了
                    if (kill(pid, SIGKILL) == 0) {
                        LOG_WARNING("SIGKILL sent successfully.");
                    } else {
                        LOG_ERROR("kill(SIGKILL) failed");
                        continue;
                    }
                }
            }
            tgg_clean_gw_monitor(i);
            g_pid_check_times[i] = 0;
            pid = -1;
            if(i < g_monitor_count-2) {
                pid = start_gwrcv_reactor_sendary(i);
            } else if (i == g_monitor_count-2) {
                pid = start_gwcliprc(i);
            } else if (i == g_monitor_count - 1) {
                pid = start_register(i);
            }
            if(pid < 0) {
                LOG_ERROR("fork for lcore[%d] failed", i);
            }
            if(tgg_setup_gw_monitor(i, pid) < 0) {
                LOG_ERROR("setup monitor for lcore[%d] failed, pid:%d", i, pid);
            }
        } else {
            if(g_pid_check_times[i])
                g_pid_check_times[i] = 0;
        }
    }
}

static uint64_t s_last_update_time = 0;
static uint64_t s_check_times = 0;// 函数进入次数
// 定时器回调函数
static void update_gwrcv_secondary_heart_beat(struct rte_timer* tm, void* arg) {
    uint64_t now = get_system_ms();
    s_check_times++;
    if(now - s_last_update_time >= GW_MONITOR_HEART_BEAT_UPDATE) {
        tgg_update_gw_monitor(g_core_id, now);
        s_last_update_time = now;
        s_check_times = 0;
    }
}

int64_t g_max_concurency = 0;
static int64_t s_last_fd_count = 0;
static void concurrency_stat(struct rte_timer* tm, void* arg)
{
    int64_t cur_count = tgg_count_idx(g_core_id);
    int64_t concurency = cur_count - s_last_fd_count;
    if(concurency > 0 && g_max_concurency < concurency) {// 最大并发
        g_max_concurency = concurency;
        LOG_WARNING("core[%d] max concurency :%ld", g_core_id, g_max_concurency);
    }
    s_last_fd_count = cur_count;
}

static void deal_master_cmd_dequeue(struct rte_timer* tm, void* arg)
{
    if(rte_eal_process_type() == RTE_PROC_PRIMARY) {
        tgg_send_master_data* cmd = NULL;
        if(tgg_dequeue_master(&cmd) >= 0) {
            switch(cmd->cmd) {
                case CMD_IP_FILTER_RELOAD:
                    reload_ip_filter(TggConfigure::getInstance()->get_ip_filter_path().c_str());
                    break;
                case CMD_PRINT_DATA_STATS:
                    print_mem_statistics();
                    print_hash_statistics();
                    break;
                default:
                    LOG_WARNING("unknown cmd:%d", cmd->cmd);
                    break;
            }
            dpdk_rte_free(cmd);
        }
    } else {
        // primary执行完成后会设置标志，sync_ip_filter函数内部会根据标记判断是否执行同步
        sync_ip_filter();
    }
}
// 定时任务
struct rte_timer timer_task_sigchild;// 处理子进程信号
struct rte_timer timer_task_monitor;// 监控管理子进程
struct rte_timer timer_task_update_heartbeat;// secondary更新心跳
struct rte_timer timer_task_concurrency;// 计算最高并发
struct rte_timer timer_task_master_cmd;// 计算最高并发

void init_timer()
{
    uint64_t hz = rte_get_timer_hz();
    uint64_t ticks_50ms = (hz * 50) / 1000;  // 以毫秒为单位

    rte_timer_init(&timer_task_concurrency);// 统计并发 周期1s
    rte_timer_reset(&timer_task_concurrency, hz, PERIODICAL, 
            rte_lcore_id(), concurrency_stat, NULL);
    if(TggConfigure::getInstance()->get_auto_start()) {
        if(rte_eal_process_type() == RTE_PROC_PRIMARY) {
            rte_timer_init(&timer_task_sigchild);//  信号处理轮训周期50ms
            rte_timer_reset(&timer_task_sigchild, ticks_50ms, PERIODICAL, 
                    rte_lcore_id(), deal_sigchild, NULL);
            rte_timer_init(&timer_task_monitor);// 心跳检查周期 5s
            rte_timer_reset(&timer_task_monitor, hz * GW_MONITOR_HEART_BEAT_CHECK, PERIODICAL, 
                    rte_lcore_id(), check_gw_monitor, NULL);
        } else {
            rte_timer_init(&timer_task_update_heartbeat);// 心跳更新周期 1s
            rte_timer_reset(&timer_task_update_heartbeat, hz, PERIODICAL, 
                    rte_lcore_id(), update_gwrcv_secondary_heart_beat, NULL);
        }
    }
    rte_timer_init(&timer_task_master_cmd);// 统计并发 周期1s
    rte_timer_reset(&timer_task_master_cmd, hz, PERIODICAL, 
            rte_lcore_id(), deal_master_cmd_dequeue, NULL);
}

void stop_timer()
{
    rte_timer_stop_sync(&timer_task_master_cmd);
    rte_timer_stop_sync(&timer_task_concurrency);
    if(TggConfigure::getInstance()->get_auto_start()) {
        if(rte_eal_process_type() == RTE_PROC_PRIMARY) {
            rte_timer_stop_sync(&timer_task_sigchild);
            rte_timer_stop_sync(&timer_task_monitor);
        } else {
            rte_timer_stop_sync(&timer_task_update_heartbeat);
        }
    }
}

int check_if_all_child_up()
{
    // int monitor_count = count_ones(TggConfigure::getInstance()->get_lcore_mask()) + 2;// +2 是gwcliprc和register
    for (int i = 0; i < g_monitor_count; ++i)// 0号进程 自己不能监控自己，由service监控 
    {
        if(i == g_core_id) {// primary进程 自己不能监控自己，由service监控 
            continue;
        }
        if(tgg_check_gw_monitor_up(i) <= 0) {
            return 0;
        }
    }
    return 1;
}


void kill_all_child()
{
    // int monitor_count = count_ones(TggConfigure::getInstance()->get_lcore_mask()) + 2;// +2 是gwcliprc和register
    for (int i = 0; i < g_monitor_count; ++i)// 0号进程 自己不能监控自己，由service监控 
    {
        if(i == g_core_id) {// primary进程 自己不能监控自己，由service监控 
            continue;
        }
        pid_t pid = tgg_get_gw_monitor_pid(i);
        if(pid <= 0) {
            continue;
        }
        LOG_INFO("core_id[%d] pid[%d] heartbeat timeout, try to kill.", i, pid);
        if (kill(pid, SIGINT) == -1) {// 不能kill -9，可能会导致其他进程死锁
            if (errno == ESRCH) {
                LOG_ERROR("core_id[%d] process[%d] not exist anymore.", i, pid);
            } else if (errno == EPERM) {
                LOG_ERROR("Permission denied process[%d] core_id[%d].", pid, i);
            } else {
                LOG_ERROR("kill core_id[%d] process[%d] faild error:%d.", i, pid, errno);
            }
        }
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

        tgg_clean_gw_monitor(i);
    }

}

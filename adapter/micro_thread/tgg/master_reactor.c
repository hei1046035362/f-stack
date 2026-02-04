#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include "mt_incl.h"
#include "mt_api.h"
#include "micro_thread.h"
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <rte_timer.h>
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/tgg_struct.h"
#include "dpdk_init.h"
#include <arpa/inet.h>
#include <tgg_comm/tgg_bw_cache.h>
#include "tgg_comm/tgg_conf.h"
#include "comm/common.hpp"
#include "comm/log.hpp"
#include "tgg_comm/WsConsumer.h"
#include "comm/Encrypt.hpp"
#include "tgg_comm/tgg_master_timers.h"
#include "tgg_comm/tgg_ip_filter.h"
#include "reactor/reactor.h"

static const char* s_dump_file = "/var/corefiles/";//tgg_gw_master_core

// 1、心跳检测间隔，没收到数据就会结束fd，
// 2、freebsd底层销毁并回收fd的时间是30s，这个时间最好是大于30
// static unsigned long long s_fd_timeout = 60*1000;
extern struct rte_mempool* g_mempool_write;
extern struct rte_mempool* g_mempool_write_data;
extern struct rte_mempool* g_mempool_clictx_buffer;

extern ushort g_gateway_port;
extern tgg_stats g_tgg_stats;
extern int g_fd_limit;
extern int g_core_id;
extern int64_t g_max_concurency;
// 进程是否退出  master进程退出不需要做什么事情，但是secondary退出前必须要释放他持有的内存
int g_run_status = 1;
int g_monitor_count = 0;
using namespace NS_MICRO_THREAD;
int sig_pipe[2];// 信号处理放入主函数异步处理，信号函数中很多系统函数不能调用，会崩溃死锁
static int64_t s_left_fd = 0;// 剩余客户端连接数
int* g_pid_check_times;

typedef struct st_conn_info {
    int cli_fd;
    unsigned int ip;
    unsigned short port;
} conn_info;

void signal_handler(int signum)
{
    SIG_PRINTF("gwrcv coreid[%d] catched signal:%d\n", g_core_id, signum);
    if(signum == SIGINT || signum == SIGTERM) {
        if(g_run_status) {
            g_run_status = 0;
        }
    }
}

void sigchld_handler(int sig) {
    int saved_errno = errno;
    char buf[32];
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) { // 非阻塞回收所有僵尸进程[5,7](@ref)
        if(g_run_status && TggConfigure::getInstance()->get_auto_start()) {
            // 监控到子进程退出，立刻再启动一个
            int len = snprintf(buf, sizeof(buf), "%d_%d\n", pid, WTERMSIG(status));
            write(sig_pipe[1], buf, len);
            if (WIFEXITED(status)) {
                SIG_PRINTF("gwrcv child %d exit normal, exit code: %d\n", pid, WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                SIG_PRINTF("gwrcv child %d exit by signal: %d\n", pid, WTERMSIG(status));
            }
        }
    }
    errno = saved_errno;
}

void tgg_sig_init()
{
    if (signal(SIGINT, signal_handler) == SIG_ERR) {
        LOG_ERROR("Error setting signal handler");
        prc_exit(-1, "Error setting signal handler");
    }
    if (signal(SIGTERM, signal_handler) == SIG_ERR) {
        LOG_ERROR("Error setting signal handler");
        prc_exit(-1, "Error setting signal handler");
    }

    if (signal(SIGCHLD, sigchld_handler) == SIG_ERR) {
        LOG_ERROR("Error setting signal handler");
        exit(-1);
    }
}

static int set_fd_nonblock(int fd)
{
    int nonblock = 1;
    return ff_ioctl(fd, FIONBIO, &nonblock);
}

static int create_tcp_sock()
{
    int fd;
    fd = ff_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("create tcp socket failed, error: %s.", strerror(errno));
        return -1;
    }
    if (set_fd_nonblock(fd) == -1) {
        LOG_ERROR("set tcp socket nonblock failed");
        return -1;
    }

    return fd;
}

static int consume_rdata(int clt_fd, const char* buf, int len, int idx, enum FD_OPT opt)
{
    g_tgg_stats.en_read_stats.malloc_st++;
    tgg_read_data rdata = {0};
    rdata.fd = clt_fd;
    rdata.coreid = g_core_id;
    rdata.idx = idx;
    rdata.fd_opt = opt;
    rdata.data_len = len;
    rdata.data = (void*)buf;
    WsConsumer cons;
    int ret = cons.ConsumerData(&rdata);
    return ret;
}

static void free_client_context(client_context_t *ctx) {
    if (ctx) {
        if (ctx->fd >= 0) {
            ff_close(ctx->fd);
        }
        high_freq_free(g_mempool_clictx_buffer, ctx, sizeof(client_context_t));
        // free(ctx);
        // ctx = NULL;
    }
}

static void clean_client_data(int cli_fd, int idx)
{
    LOG_DEBUG("close client %d.", cli_fd);
    tgg_del_idx(g_core_id, idx);
    tgg_close_cli(g_core_id, cli_fd);
    release_ws_buffer(g_core_id, cli_fd);
    s_left_fd--;
}

// static uint64_t send_times = 0;
static void do_real_send(int fd, event_type_t events, void *arg)
{
    client_context_t *ctx = (client_context_t *)arg;
    
    if (!(events & EVENT_WRITE)) {
        return;
    }
    
    tgg_send_data* data = NULL;
    int ret = 0;
    while((data = tgg_pop_cli_snd_data(g_core_id, fd)) != NULL) {
        if(data && data->data) {
            if(((tgg_write_data*)(data->data))->data) {
                if(ret >= 0) {
                    ret = ff_write(fd, ((tgg_write_data*)(data->data))->data, ((tgg_write_data*)(data->data))->data_len);
                    if (ret <= 0) {
                        if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                            LOG_ERROR("Write error to client %d, closing", fd);
                            reactor_modify_event(fd, EVENT_READ);
                            ff_close(fd);
                        } else {
                            tgg_set_write_data(g_core_id, fd, (void*)data);
                        }
                        return;
                    }
                    tgg_set_write_data(g_core_id, fd, NULL);
                    // send_times++;
                    // 更新活动时间
                    reactor_update_activity(fd);
                    if ( ((tgg_write_data*)(data->data))->fd_opt & FD_CLOSE) {
                        LOG_INFO("Closing Connection[%d].", fd);
                        tgg_set_cli_idx(g_core_id, fd, TGG_FD_CLOSING);// 先设置标记，防止队列没人消费，影响其他连接
                    }

                }

                ((tgg_write_data*)(data->data))->ref--;
            }
            if(((tgg_write_data*)(data->data))->ref <= 0) {
                // LOG_DEBUG("sendtime:%lu, ctxfd[%d] fd[%d], ref:%d",
                //  send_times, ctx->fd, fd, ((tgg_write_data*)(data->data))->ref);
                clean_write_data(g_core_id, (tgg_write_data*)(data->data));
            }
            tgg_free_cli_snd_data(g_core_id, data);
        }
    }
    if (tgg_get_cli_idx(g_core_id, fd) == TGG_FD_CLOSING) {
        LOG_INFO("Client %d closing", fd);
        reactor_remove_event(fd);
        clean_client_data(fd, ctx->idx);
        free_client_context(ctx);
        return;
    }
    if(tgg_get_cli_idx(g_core_id, fd) >= 0) {
        reactor_modify_event(fd, EVENT_READ);
    }
    // return ret;
}

static client_context_t *create_client_context(int fd, unsigned int ip, unsigned short port, int idx) {
    client_context_t *ctx = NULL;
    if(high_freq_malloc(g_mempool_clictx_buffer, (void**)&ctx, sizeof(client_context_t)) < 0)//(client_context_t *)malloc(sizeof(client_context_t));
        return NULL;
    
    ctx->fd = fd;
    ctx->ip = ip;
    ctx->port = port;
    ctx->idx = idx;
    return ctx;
}

static void tgg_recv(int fd, event_type_t events, void *arg)
{
    client_context_t *ctx = (client_context_t *)arg;
    int idx = tgg_get_cli_idx(g_core_id, fd);
    char buf[BUFFER_PACKET_LEN] = {0};
    int n = 0;
    if(ctx == NULL || idx == TGG_FD_CLOSED) {
        LOG_INFO("Client %d is closed, idx:%d", fd, idx);
        return;
    }
    if (events & EVENT_HUP) {
        // 正常关闭：对方发送了 FIN（EPOLLHUP）
        LOG_DEBUG("Client %d peer hangup (normal close)", fd);
        goto recv_failed;
    }
    if (events & EVENT_ERROR) {
        // 真正的连接错误：EPOLLERR
        LOG_ERROR("Client %d connection error, closing", fd);
        goto recv_failed;
    }
    
    if (!(events & EVENT_READ)) {
        return;
    }
    if (idx != ctx->idx) {
        LOG_WARNING("Client %d idx[%d] != ctx->idx[%d] , closing", fd, idx, ctx->idx);
        goto recv_failed;        
    }
    // 读取数据
    n = ff_read(fd, buf, BUFFER_PACKET_LEN);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No data available now, wait for the next read event
            reactor_update_activity(fd);
            return;
        }
        // real read error
        LOG_ERROR("Read error from client %d, closing: %s", fd, strerror(errno));
        goto recv_failed;
    }
    if (n == 0) {
        // peer has performed an orderly shutdown
        LOG_INFO("Client %d disconnected", fd);
        goto recv_failed;
    }
    // 更新活动时间
    reactor_update_activity(fd);

    if(AsyncLogger::getInstance().getloglevel() == LogLevel::DEBUG) {
        // 调试打印
        if(!strncmp(buf, "GET", 3)) {// GET请求消息
            LOG_DEBUG("fd:%d idx:%d recv data:%s.", fd, idx, (char*)buf);
        } else {// 其他消息
            LOG_DEBUG("fd:%d idx:%d revc data:%s.", fd, idx, bin2hex(std::string_view((char*)buf, n)).c_str());
        }
    }

    if (consume_rdata(fd, buf, n, idx, FD_READ) < 0) {
        LOG_ERROR("consume data failed.");
        goto recv_failed;
    }
    return;
recv_failed:
    if(!(tgg_get_cli_status(g_core_id, fd) & FD_STATUS_CLOSING) && // 没发送过close给gwcliprc
        (tgg_get_cli_idx(g_core_id, fd) != TGG_FD_CLOSING)) {// ws握手完成
        consume_rdata(fd, NULL, 0, ctx->idx, FD_CLOSE);// 通知bwprc 清理这个客户端相关信息
    }
    if((tgg_get_cli_idx(g_core_id, fd) == TGG_FD_CLOSED)) {
        LOG_WARNING("Client %d already closed.", fd);
        free_client_context(ctx);
        return;
    }
    reactor_remove_event(fd);
    clean_client_data(fd, ctx->idx);
    free_client_context(ctx);
}

static void on_client_connect(void *arg)
{
    s_left_fd++;
    // int ret, consume_ret = 0;
    conn_info* cli_info = (conn_info *)arg;
    char ip_str[INET_ADDRSTRLEN] = {0};
    // unsigned short port = cli_info->port;
    int idx = -1;
    bool exclude = is_ip_exclude(cli_info->ip);// exclude的连接只recv，不进入业务逻辑
    if(!exclude) {
        if(tgg_init_cli(g_core_id, cli_info->cli_fd, ip_str, cli_info->ip, cli_info->port) < 0) {
            LOG_ERROR("init client info failed.");
            ff_close(cli_info->cli_fd);
            tgg_close_cli(g_core_id, cli_info->cli_fd);
            free(cli_info);
            s_left_fd--;
            return;
        }
        idx = tgg_get_cli_idx(g_core_id, cli_info->cli_fd);
    }
    ff_fcntl(cli_info->cli_fd, F_SETFL, ff_fcntl(cli_info->cli_fd, F_GETFL, 0) | O_NONBLOCK);

    client_context_t *client_ctx = create_client_context(cli_info->cli_fd, cli_info->ip, cli_info->port, idx);
    if (!client_ctx) {
        ff_close(cli_info->cli_fd);
        tgg_close_cli(g_core_id, cli_info->cli_fd);
        free(cli_info);
        s_left_fd--;
        return;
    }

    if (reactor_add_event(cli_info->cli_fd, EVENT_READ, tgg_recv, do_real_send, 
        client_ctx, TggConfigure::getInstance()->get_gateway_fd_timeout()) < 0) {
        LOG_ERROR("add read event for client[%d] failed.", cli_info->cli_fd);
        ff_close(cli_info->cli_fd);
        tgg_close_cli(g_core_id, cli_info->cli_fd);
        free_client_context(client_ctx);
        s_left_fd--;
    }
    free(cli_info);
}
// static uint64_t add_send_times = 0;
static void tgg_do_send(tgg_write_data* wdata)
{
    tgg_fd_id_list* fd_id_list = wdata->lst_fd;
    while (fd_id_list) {
        int cli_fd = fd_id_list->fdid;// 数据传递时fdid存的是fd
        int idx = tgg_get_cli_idx(g_core_id, cli_fd);
        if(AsyncLogger::getInstance().getloglevel() == LogLevel::DEBUG) {
            if(wdata->data_len > 4 && !strncmp((char*)wdata->data, "HTTP", 4)) {// GET请求消息
                LOG_DEBUG("fd:%d idx:%d send to clien:%s.", cli_fd, idx, (char*)wdata->data);
            } else {// 其他消息
                LOG_DEBUG("fd:%d idx:%d send to clien:%s.", cli_fd, idx, bin2hex(std::string_view((char*)wdata->data, wdata->data_len)).c_str());
            }
        }
        // 只有未关闭的连接才需要走以下逻辑，已经关闭的连接，不再发送数据
        if(idx > 0) {
            // 新的连接旧的数据就不要发送了，直接清理空间
            if (idx != fd_id_list->idx) {// 后台推送给前端时，可能会出现这种情况
                LOG_ERROR("Idx[%d:%d] Changed, Closing Connection[%d].", idx, fd_id_list->idx, cli_fd);
                wdata->ref--;
                goto send_client_end;
            }

            // 是否需要发送数据
            if (wdata->fd_opt & FD_WRITE && (!(tgg_get_cli_status(g_core_id, cli_fd) & FD_STATUS_DISCONNECTED))) {
                int try_times = 1000;// 最多等待1s，否则就关闭连接
                while (tgg_add_cli_snd_data(g_core_id, cli_fd, wdata) < 0 && try_times-- > 0)
                {
                    mt_sleep(1);
                }
                if(try_times < 0) {
                    LOG_ERROR("add cli[fd:%d, idx:%d] snd data failed, exceed try_times", cli_fd, idx);
                    ff_close(cli_fd);
                    wdata->ref--;
                    goto send_client_end;
                }
            }
            // add_send_times++;
            reactor_modify_event(cli_fd, EVENT_WRITE);
        } else {
            // 连接标记已设置为关闭，队列中的数据直接丢弃
            LOG_DEBUG("write to client data droped, cause connection[fd:%d] not published[idx:%d].", cli_fd, idx);
        }

send_client_end:
        // tgg_fd_id_list* tmp = fd_id_list;
        fd_id_list = fd_id_list->next;
        // clean_fdidnode(tmp);
    }
    // LOG_DEBUG("addsendtime:%lu", add_send_times);
    // 所有fd都发送完了之后，需要清理并回收内存
    if(wdata->ref <= 0) {
        clean_write_data(g_core_id, wdata);
    }else {
        clean_fdidlist(wdata->lst_fd);
        wdata->lst_fd = NULL;// 清理完必须要置空，否则后续clean_write_data时，会重复释放
    }
}

static void tgg_send(void *arg)
{
    while(g_run_status) {
        tgg_write_data* wdata = NULL;
        if (tgg_dequeue_write(g_core_id, &wdata) < 0) {
            // 队列空
            mt_sleep(1);
            continue;
        }
        if (!wdata) {
            continue;
        }

        // TODO send
        tgg_do_send(wdata);
    }
}

static int tgg_gw_master()
{
    // 启动发送线程
    // for(int i = 0; i < TggConfigure::getInstance()->get_gwwrite_co_count(); ++i) {
        mt_start_thread((void *)tgg_send, NULL);
    // }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;

    addr.sin_port = big_endian() ? TggConfigure::getInstance()->get_gateway_port() : htons(TggConfigure::getInstance()->get_gateway_port());

    int fd = create_tcp_sock();
    if (fd < 0) {
        LOG_ERROR("create listen socket failed");
        return -1;
    }

    if (ff_bind(fd, (const struct linux_sockaddr *)&addr, sizeof(addr)) < 0) {
        ff_close(fd);
        LOG_ERROR("bind failed [%s]", strerror(errno));
        return -1;
    }

    if (ff_listen(fd, 1024) < 0) {
        ff_close(fd);
        LOG_ERROR("listen failed [%s]", strerror(errno));
        return -1;
    }
    LOG_INFO("start service for port:%d.", TggConfigure::getInstance()->get_gateway_port());
    int clt_fd = 0;
    // int idx = -1;
    conn_info *p;
    while (g_run_status) {
        struct sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);

        clt_fd = ff_accept(fd, (struct linux_sockaddr*)&client_addr, (socklen_t*)&addr_len);
        if (clt_fd < 0) {
            if(clt_fd != -1) {
                LOG_WARNING("accept error[%d]", clt_fd);
            }
            mt_sleep(1);
            continue;
        }
        if (clt_fd >= g_fd_limit - 1)   {
            LOG_WARNING("given fd[%d] is invalid,[0,%d]", fd, g_fd_limit - 1);
            ff_close(clt_fd);
            mt_sleep(10);
            continue;
        }
        // 如果fd还在使用中，拒绝连接
        if (tgg_get_cli_idx(g_core_id, clt_fd) != TGG_FD_CLOSED) {
            LOG_ERROR("socket fd[%d] still in use.", clt_fd);
            // clean_client_data(fd, ctx->idx);
            // reactor_remove_event(fd);
            // free_client_context(ctx);
            ff_close(clt_fd);
            continue;
        }
        if (set_fd_nonblock(clt_fd) == -1) {
            LOG_ERROR("set clt_fd nonblock failed [%s]", strerror(errno));
            break;
        }
        p = (conn_info *)malloc(sizeof(conn_info));
        if (!p) {
            LOG_ERROR("malloc conn_info failed.");
            ff_close(clt_fd); 
            return -1;
        }
        p->cli_fd = clt_fd;
        p->ip = client_addr.sin_addr.s_addr;
        p->port = client_addr.sin_port;
        // 启动一个接收线程
        // void* pthread = mt_start_thread((void *)tgg_recv, (void *)p);
        // tgg_set_cli_thread(g_core_id, clt_fd, pthread);
        on_client_connect((void*)p);
    }
    ff_close(fd);
    return 0;
}

static void tgg_recv_clean_prev()
{
    for (int i = 0; i < g_fd_limit; ++i)
    {// 防止secondary进程异常重启后，上一次的缓存没有清理
        int idx = tgg_get_cli_idx(g_core_id, i);
        if( idx > 0 && tgg_check_idx_exist(g_core_id, idx)) {
            // 通知gwbwprc 清理这个链接对应的缓存
            LOG_ERROR("clean prev data coreid[%d] fd[%d] idx[%d].", g_core_id, i, idx);
            consume_rdata(i, NULL, 0, idx, FD_CLOSE);
            clean_client_data(i, idx);
        }
    }
    tgg_iter_del_idx(g_core_id);
}

int main(int argc, char *argv[])
{
    init_core(s_dump_file);
    if (tgg_init_config(argc, argv) < 0) {
        printf("init config error.\n");
        return -1;
    }
    if (AsyncLogger::getInstance().init(TggConfigure::getInstance()->get_log_path(), 
        TggConfigure::getInstance()->get_gateway_log_level()) < 0) {
        printf("init log error.\n");
        return -1;
    }

    tgg_sig_init();// 信号处理初始化
    initOpenSSL();// 初始化ssl加解密环境

    if (!mt_init_frame(argc, argv)) {
        LOG_ERROR("mt frame init failed.");
        return -1;
    }
    if (!init_ip_filter(rte_eal_process_type() == RTE_PROC_PRIMARY, TggConfigure::getInstance()->get_ip_filter_path().c_str())) {
        LOG_WARNING("init ip filter failed.");        
    }
    g_core_id = mt_get_proc_id();//rte_lcore_to_cpu_id(rte_lcore_id());
    if(rte_eal_process_type() == RTE_PROC_PRIMARY) {
        pipe2(sig_pipe, O_NONBLOCK | O_CLOEXEC);
        LOG_INFO("-------master[pid:%d] core[%d] start-------", getpid(), g_core_id);
        tgg_master_init();
        if(TggConfigure::getInstance()->get_auto_start()) {
            g_monitor_count = count_ones(TggConfigure::getInstance()->get_lcore_mask()) + 2;// +2 是gwcliprc和register
            g_pid_check_times = new int[g_monitor_count]{0};
            check_gw_monitor(NULL, NULL);
            // 检查子进程是否已全部启动
            int check_times = 1500;// 最多等待15s
            while (g_run_status && check_times > 0) {
                if(check_if_all_child_up()) {
                    break;
                }
                check_times--;
                usleep(10000);
            }
            if(!check_if_all_child_up()) {
                kill_all_child();
                g_run_status = 0;
                LOG_FATAL("not all child process is working on the beginning, exiting...");
            }
            // mt_sleep(2000);// 等待所有进程启动完成
        }
    } else {
        LOG_INFO("-------secondary[pid:%d] core[%d] start-------", getpid(), g_core_id);
        tgg_gwrcv_secondary_init();
        if(TggConfigure::getInstance()->get_auto_start()) {
            if (tgg_check_gw_monitor_up(g_core_id)) {// 上一个进程尚未结束
                LOG_INFO("-------secondary core[%d] exit, prev coreid still running-------", g_core_id);
                mt_uninit_frame();
                rte_eal_cleanup();
                AsyncLogger::getInstance().shutdown();
                return 0;
            }
        }
        tgg_recv_clean_prev();
    }
    // 启动定时器
    init_timer();
    reactor_create(MAX_CLIENTS, TggConfigure::getInstance()->get_gateway_fd_timeout());
    // 主循环
    tgg_gw_master();
    reactor_stop();
    // 停止定时器
    stop_timer();
    print_mem_statistics();
    if(rte_eal_process_type() == RTE_PROC_PRIMARY) {
        if(TggConfigure::getInstance()->get_auto_start()) {
            kill_all_child();
            wait_all_child_exit();
            LOG_INFO("-------master core[%d] exit-------", g_core_id);
            delete[] g_pid_check_times;
        }
        tgg_master_uninit();
        cleanup_ip_filter();
    } else {
        LOG_INFO("-------secondary core[%d] exit-------", g_core_id);
    }
    mt_uninit_frame();
    reactor_destroy();
    rte_eal_cleanup();
    LOG_WARNING("gwrcv left fd count:%ld", s_left_fd);
    AsyncLogger::getInstance().shutdown();
    return 0;
}

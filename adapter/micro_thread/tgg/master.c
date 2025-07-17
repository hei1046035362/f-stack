#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include "mt_incl.h"
#include "mt_api.h"
#include "micro_thread.h"
#include <rte_mempool.h>
#include <rte_malloc.h>
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

static const char* s_dump_file = "/var/corefiles/";//tgg_gw_master_core

// 1、心跳检测间隔，没收到数据就会结束fd，
// 2、freebsd底层销毁并回收fd的时间是30s，这个时间最好是大于30
static int s_fd_timeout = 60*1000;
extern const char* g_rte_malloc_type;
extern struct rte_mempool* g_mempool_write;
extern struct rte_mempool* g_mempool_write_data;
extern ushort g_gateway_port;
extern tgg_stats g_tgg_stats;
extern int g_fd_limit;
extern int g_core_id;

// 进程是否退出  master进程退出不需要做什么事情，但是secondary退出前必须要释放他持有的内存
int g_run_status = 1;

using namespace NS_MICRO_THREAD;

void signal_handler(int signum)
{
	if(signum == SIGINT || signum == SIGTERM) {
		if(g_run_status) {
			g_run_status = 0;
			RTE_LOG(WARNING, USER1, "catched signal:%d\n", signum);
		}
	}
}

void sigchld_handler(int sig) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0); // 非阻塞回收所有僵尸进程[5,7](@ref)
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
        perror("Error setting signal handler");
        exit(-1);
    }
}

static int get_remote_info(int sockfd, uint32_t& ip, ushort& port, char* ip_str)
{
     // 获取IP地址信息
     struct sockaddr_in local_addr;
     socklen_t addrlen = sizeof(local_addr);
     if (ff_getpeername(sockfd, (struct linux_sockaddr *)&local_addr, &addrlen) < 0) {
         LOG_ERROR("getsockname");
         close(sockfd);
         return -1;
     }
     // char ip_str[INET_ADDRSTRLEN];
     inet_ntop(AF_INET, &(local_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
     // printf("ip str:%s\n", ip_str);
     struct in_addr ip_addr;
     inet_pton(AF_INET, ip_str, &ip_addr);
     ip = ip_addr.s_addr;
     port = local_addr.sin_port;
     LOG_INFO("IP address in decimal: %u\n", ip);
     return 0;
}


static int set_fd_nonblock(int fd)
{
	int nonblock = 1;
	return ioctl(fd, FIONBIO, &nonblock);
}

static int create_tcp_sock()
{
	int fd;
	fd = socket(AF_INET, SOCK_STREAM, 0);
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

static void clean_client_data(int cli_fd, int idx)
{
	tgg_del_idx(g_core_id, idx);
	tgg_close_cli(g_core_id, cli_fd);
	release_ws_buffer(g_core_id, cli_fd);
}

static void tgg_recv(void *arg)
{
	int ret, consume_ret = 0;
	int cli_fd = *((int *)arg);
	delete (int *)arg;
	uint32_t ip;
	ushort port;
	char ip_str[INET_ADDRSTRLEN] = {0};
	if (get_remote_info(cli_fd, ip, port, ip_str) < 0) {
		LOG_ERROR("get client remote info failed.");
		close(cli_fd);
		return;
	}
	if(tgg_init_cli(g_core_id, cli_fd, ip_str, ip, port) < 0) {
		LOG_ERROR("init client info failed.");
		close(cli_fd);
		tgg_close_cli(g_core_id, cli_fd);
		return;
	}
	int idx = tgg_get_cli_idx(g_core_id, cli_fd);
	// 通知后台有新的连接
	if (consume_rdata(cli_fd, "", 0, idx, FD_NEW) < 0) {
		LOG_ERROR("send new connection[%d] to cliprc failed, core id:%d idx:%d.", cli_fd, g_core_id, idx);
		close(cli_fd);
		tgg_close_cli(g_core_id, cli_fd);
		return;
	}
	char buf[1024] = {0};
	while (g_run_status) {
		// 1、接收数据  mt_recv在没有数据包的情况下会阻塞，让出cpu给其他的action执行
		ret = mt_recv(cli_fd, (void *)buf, 1024, 0, s_fd_timeout);
		if(ret == -1 && errno == ETIME) {
			LOG_ERROR("client heart beat timeout, idx:%d.", idx);
			break;
		}
		if(ret == -4) {
			// 主动断开连接
			LOG_ERROR("closing connection affected,idx:%d.", idx);
			break;
		}
		g_tgg_stats.recv++;
		if (ret < 0) {
			// 接收出现错误
			LOG_ERROR("recv from client ret:%d error:%s, idx:%d.", ret, strerror(errno), idx);
			break;
		}
		if (!ret) {
			// 对端主动关闭了
			LOG_INFO("recv close from client, idx:%d.", idx);
			break;
		}
		if(AsyncLogger::getInstance().getloglevel() == LogLevel::DEBUG) {
			// 调试打印
			if(!strncmp(buf, "GET", 3)) {// GET请求消息
				LOG_DEBUG("fd:%d idx:%d recv data:%s.", cli_fd, idx, (char*)buf);
			} else {// 其他消息
	    		LOG_DEBUG("fd:%d idx:%d revc data:%s.", cli_fd, idx, bin2hex(std::string((char*)buf, ret)).c_str());
			}
		}
		if(tgg_get_cli_idx(g_core_id, cli_fd) == TGG_FD_CLOSING) {// 服务端发送踢人命令的时候会触发
			LOG_WARNING("connection[%d] idx[%d] is closing.", cli_fd, idx);
			break;
		}
		consume_ret = consume_rdata(cli_fd, buf, ret, idx, FD_READ);
		if (consume_ret < 0) {
			if(AsyncLogger::getInstance().getloglevel() == LogLevel::DEBUG) {
				// 调试打印
				if(!strncmp(buf, "GET", 3)) {// GET请求消息
					LOG_WARNING("fd:%d idx:%d recv data:%s.", cli_fd, tgg_get_cli_idx(g_core_id, cli_fd), (char*)buf);
				} else {// 其他消息
					LOG_WARNING("fd:%d idx:%d revc data:%s.", cli_fd, tgg_get_cli_idx(g_core_id, cli_fd), bin2hex(std::string((char*)buf, ret)).c_str());
				}
			}
			LOG_ERROR("consume data failed.");
			break;
		}
		if(tgg_get_cli_status(g_core_id, cli_fd) & FD_STATUS_CLOSING) {// 已经发送过关闭帧了
			break;
		}
	}
	if(ret <= 0) {// 连接已断开，通知写协程，不必再执行发送
		tgg_set_cli_status(g_core_id, cli_fd, FD_STATUS_DISCONNECTED);
	}
	if(!(tgg_get_cli_status(g_core_id, cli_fd) & FD_STATUS_CLOSING) && tgg_get_cli_idx(g_core_id, cli_fd) != TGG_FD_CLOSING) {// 没发送过close给gwcliprc
		consume_rdata(cli_fd, NULL, 0, idx, FD_CLOSE);
	}
	LOG_DEBUG("wait client[%d] close...", cli_fd);
	// 等待连接在缓存中的数据被消费完才能关闭
	int index = 100*60;// 最长等待1分钟，关闭包会在trans队列中可能多次enqueue back，尽可能让数据包走正常流程关闭
	while(tgg_get_cli_idx(g_core_id, cli_fd) != TGG_FD_CLOSING && index > 0 && g_run_status) {
	    mt_sleep(10);
	    index--;
	}
	if(index <= 0) {
        LOG_WARNING("recv close fram from gwprc timeout, coreid[%d] fd[%d].", g_core_id, cli_fd);		
	}
	close(cli_fd);// 这里不能使用mt_close,mt_close只设置标记，不会发送fin包，fd依然还存在
	clean_client_data(cli_fd, idx);
	LOG_WARNING("client coreid[%d] fd[%d] idx[%d] closed.", g_core_id, cli_fd, idx);
}

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
	    		LOG_DEBUG("fd:%d idx:%d send to clien:%s.", cli_fd, idx, bin2hex(std::string((char*)wdata->data, wdata->data_len)).c_str());
			}
		}
		// 只有未关闭的连接才需要走以下逻辑，已经关闭的连接，不再发送数据
		if(idx > 0) {
			// 新的连接旧的数据就不要发送了，直接清理空间
			if (idx != fd_id_list->idx) {// 后台推送给前端时，可能会出现这种情况
				LOG_ERROR("Idx[%d:%d] Changed, Closing Connection[%d].", idx, fd_id_list->idx, cli_fd);
				goto send_client_end;
			}

			// 是否需要发送数据
			if (wdata->fd_opt & FD_WRITE && (!(tgg_get_cli_status(g_core_id, cli_fd) & FD_STATUS_DISCONNECTED))) {
				int ret = mt_send(cli_fd, (void *)wdata->data, wdata->data_len, 0, 1000);
				if (ret == -4) {
					// 主动断开连接
					LOG_INFO("closing connection affected.");
				} else if (ret < 0) {
					LOG_ERROR("send data to client fd[%d] idx[%d] error, ret[%d]", cli_fd, idx, ret);
				} else {
					g_tgg_stats.en_read_stats.enqueue++;
				}
			}

			if ( wdata->fd_opt & FD_CLOSE) {
				LOG_INFO("Closing Connection[%d].", cli_fd);
				tgg_set_cli_idx(g_core_id, cli_fd, TGG_FD_CLOSING);// 先设置标记，防止队列没人消费，影响其他连接
				// if(wdata->fd_opt & FD_WRITE) {
				// 	// 这里不能sleep，我们只有一个发送的协程，一旦sleep会影响其他fd的写入
				// 	// mt_sleep(1000);// ws的关闭帧发送完以后等待客户端先关闭，如果1s后没有关闭，我们要主动结束
				// 					// 到了这里后面的数据其实都应该要丢弃了，所以后续数据已经不重要了
				// }
				// mt_close(cli_fd);// TODO:待优化，在这里结束可能会报错，四次挥手不完整：epoll schedule failed, errno: 62
								 // 但正常结束流程里close，需要等待30s，不可配置，freebsd内部控制
			}
		} else {
			// 连接标记已设置为关闭，队列中的数据直接丢弃
			LOG_DEBUG("write to client data droped, cause connection[fd:%d] not published[idx:%d].", cli_fd, idx);
		}

send_client_end:
		fd_id_list = fd_id_list->next;
	}
	// 所有fd都发送完了之后，需要清理并回收内存
	clean_write_data(g_core_id, wdata);
	// if(wdata->data) {
	// 	memset(wdata->data, 0, wdata->data_len);
	// 	high_freq_free(g_mempool_write_data, wdata->data, wdata->data_len);
	// }
	// memset(wdata, 0, sizeof(tgg_write_data));
	// high_freq_free(g_mempool_write[g_core_id], wdata, sizeof(tgg_write_data));
}

static void tgg_send(void *arg)
{
	// std::vector< std::list<void*> > vec_queue;
	// vec_queue.reserve(TggConfigure::getInstance()->get_gwwrite_co_count());
	// for(int i = 0; i < TggConfigure::getInstance()->get_gwwrite_co_count(); ++i) {
	// 	mt_start_thread((void *)tgg_do_send, vec_queue[i]);
	// }
	while(g_run_status) {
	    tgg_write_data* wdata = NULL;
	    if (tgg_dequeue_write(g_core_id, &wdata) < 0) {
	    	// 队列空
			mt_sleep(5);
	    	continue;
	    }
	    if (!wdata) {
	    	continue;
	    }

	    // TODO send
	    tgg_do_send(wdata);
	}
}

static void start_gwrcv_sendary(int lcore_id)
{
    // 构造参数数组
    char* proc_id = (char*)malloc(24);
    sprintf(proc_id, "--proc-id=%d", lcore_id);
    LOG_DEBUG("proc id arg:%s", proc_id);
    // char* file_prefix = (char*)malloc(128);
    // sprintf(file_prefix, "--file-prefix=gwrcv_%d_", lcore_id);
    // LOG_DEBUG("file prefix arg:%s", file_prefix);
    char **args = (char**)calloc(3, sizeof(char*));
    args[0] = const_cast<char*>("gwrcv");
    // args[1] = const_cast<char*>("--single-file-segments");
    // args[2] = file_prefix;
    args[1] = proc_id;
    args[2] = NULL; // 必须以 NULL 结尾
    custom_fork("gwrcv", args);
    free(args);
    // free(file_prefix);
}

static void start_gwcliprc()
{
    // char* file_prefix = (char*)malloc(128);
    // sprintf(file_prefix, "--file-prefix=gwcliprc_%d_", rte_lcore_id());
    // LOG_DEBUG("file prefix arg:%s", file_prefix);
    char **args = (char**)calloc(2, sizeof(char*));
    args[0] = const_cast<char*>("gwcliprc");
    // args[1] = const_cast<char*>("--single-file-segments");
    // args[2] = file_prefix;
    args[1] = NULL; // 必须以 NULL 结尾
    custom_fork("gwcliprc", args);
    free(args);
    // free(file_prefix);
}

static void start_register()
{
    // char* file_prefix = (char*)malloc(128);
    // sprintf(file_prefix, "--file-prefix=register_%d_", rte_lcore_id());
    // LOG_DEBUG("file prefix arg:%s", file_prefix);
    char **args = (char**)calloc(2, sizeof(char*));
    args[0] = const_cast<char*>("gwregister");
    // args[1] = const_cast<char*>("--single-file-segments");
    // args[2] = file_prefix;
    args[1] = NULL; // 必须以 NULL 结尾
    custom_fork("gwregister", args);// gwbwserver由register管理，会先启动gwbwserver,然后向注册中心发起连接请求
    free(args);
    // free(file_prefix);
}

static uint64_t s_last_check_time = 0;
static int* s_pid_check_times;
// 定时器回调函数
void check_gw_monitor()
{
    uint64_t now = get_system_ms();
    if(s_last_check_time + GW_MONITOR_HEART_BEAT < now) {
        // 500ms检测一次
        s_last_check_time = now;
    } else {
        // 没到检测时间，不检测
        return;
    }
    int monitor_count = count_ones(TggConfigure::getInstance()->get_lcore_mask()) + 2;// +2 是gwcliprc和register
    for (int i = 1; i < monitor_count; ++i)// 0号进程 自己不能监控自己，由service监控 
    {
        if(tgg_checkif_gw_monitor_timeout(i, now)) {
            pid_t pid = tgg_get_gw_monitor_pid(i);
            if(pid > 0) {
                if (kill(pid, SIGINT) == -1) {// 不能kill -9，可能会导致其他进程死锁
                    if (errno == ESRCH) {
                        LOG_ERROR("core_id[%d] process[%d] not exist anymore.", i, pid);
                    } else if (errno == EPERM) {
                        LOG_ERROR("Permission denied process[%d] core_id[%d].", pid, i);
                        continue;
                    } else {
                        LOG_ERROR("kill core_id[%d] process[%d] faild error:%d.", i, pid, errno);
                        // TODO 上线后这段代码要放开，防止死锁导致无法启动新的进程
                        if (kill(pid, 0) == 0) {
                            if(s_pid_check_times[i] < 3) {// 重试三次，不方便sleep，如果三个周期都没有退出，就强制结束
                                s_pid_check_times[i]++;
                                continue;
                            }
                            LOG_WARNING("core_id[%d] Process %d exists. Sending SIGKILL...", i, pid);
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
            if(i < monitor_count-2) {
            	start_gwrcv_sendary(i);
            } else if (i == monitor_count-2) {
            	start_gwcliprc();
            } else if (i == monitor_count - 1) {
            	start_register();
            }
        }
    }
}

static uint64_t s_last_update_time = 0;
// 定时器回调函数
void update_gwrcv_secondary_heart_beat() {
    uint64_t now = get_system_ms();
    if(now - s_last_update_time > GW_MONITOR_HEART_BEAT) {
        s_last_update_time = now;
        tgg_update_gw_monitor(rte_lcore_id(), now);
    }
}

static void gw_monitor(void* argv)
{
	while(g_run_status) {
		if(rte_eal_process_type() == RTE_PROC_PRIMARY) {
			check_gw_monitor();
		} else {
			update_gwrcv_secondary_heart_beat();
		}
		mt_sleep(100);
	}
}

static int tgg_gw_master()
{
	mt_start_thread((void *)gw_monitor, NULL);
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

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		LOG_ERROR("bind failed [%s]", strerror(errno));
		return -1;
	}

	if (listen(fd, 1024) < 0) {
		close(fd);
		LOG_ERROR("listen failed [%s]", strerror(errno));
		return -1;
	}
	LOG_INFO("start service for port:%d.", TggConfigure::getInstance()->get_gateway_port());
    int clt_fd = 0;
	int *p;
	while (g_run_status) {
		struct sockaddr_in client_addr;
		int addr_len = sizeof(client_addr);

        clt_fd = mt_accept(fd, (struct sockaddr*)&client_addr, (socklen_t*)&addr_len, -1);
		if (clt_fd < 0) {
			if(clt_fd != -1) {
				LOG_WARNING("accept error[%d]", clt_fd);
			}
			mt_sleep(1);
			continue;
		}
		if (clt_fd >= g_fd_limit - 1)	{
			LOG_WARNING("given fd[%d] is invalid,[0,%d]", fd, g_fd_limit - 1);
			mt_sleep(10);
			continue;
		}
		// 如果fd还在使用中，拒绝连接
		if (tgg_get_cli_idx(g_core_id, clt_fd) != TGG_FD_CLOSED) {
			LOG_ERROR("socket fd[%d] still in use.", clt_fd);
			close(clt_fd);
			continue;
		}
		if (set_fd_nonblock(clt_fd) == -1) {
			LOG_ERROR("set clt_fd nonblock failed [%s]", strerror(errno));
			break;
		}
		// 启动一个接收线程
		p = new int(clt_fd);
		mt_start_thread((void *)tgg_recv, (void *)p);
	}
	close(fd);
	return 0;
}

static void tgg_recv_clean_prev()
{
	for (int i = 0; i < g_fd_limit; ++i)
	{// 防止secondary进程异常重启后，上一次的缓存没有清理
		int idx = tgg_get_cli_idx(g_core_id, i);
		if( idx > 0 && tgg_check_idx_exist(g_core_id, idx)) {
			// 通知gwbwrcv 清理这个链接对应的缓存
			LOG_ERROR("clean prev data coreid[%d] fd[%d] idx[%d].", g_core_id, i, idx);
			consume_rdata(i, NULL, 0, idx, FD_CLOSE);
			tgg_del_idx(g_core_id, idx);
		}
	}
	tgg_iter_del_idx(g_core_id);
}

static int check_if_all_child_up()
{
    int monitor_count = count_ones(TggConfigure::getInstance()->get_lcore_mask()) + 2;// +2 是gwcliprc和register
    for (int i = 1; i < monitor_count; ++i)// 0号进程 自己不能监控自己，由service监控 
    {
        if(tgg_get_gw_monitor_pid(i) <= 0) {
            return 0;
        }
    }
    return 1;
}

static void kill_all_child()
{
	int monitor_count = count_ones(TggConfigure::getInstance()->get_lcore_mask()) + 2;// +2 是gwcliprc和register
    for (int i = 1; i < monitor_count; ++i)// 0号进程 自己不能监控自己，由service监控 
    {
    	pid_t pid = tgg_get_gw_monitor_pid(i);
        if(pid <= 0) {
            continue;
        }
        if (kill(pid, SIGINT) == -1) {// 不能kill -9，可能会导致其他进程死锁
            if (errno == ESRCH) {
                LOG_ERROR("core_id[%d] process[%d] not exist anymore.", i, pid);
            } else if (errno == EPERM) {
                LOG_ERROR("Permission denied process[%d] core_id[%d].", pid, i);
            } else {
                LOG_ERROR("kill core_id[%d] process[%d] faild error:%d.", i, pid, errno);
                int wait_times = 50;// 最长等待5s，还没有退出的话，就发送kill -9
                while (kill(pid, 0) == 0) {// 进程还存在
                    if (wait_times > 0) {
                        usleep(100);
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
	g_core_id = rte_lcore_id();
	if(rte_eal_process_type() == RTE_PROC_PRIMARY) {
		LOG_INFO("-------master[pid:%d] core[%d] start-------", getpid(), g_core_id);
		tgg_master_init();
		int monitor_count = count_ones(TggConfigure::getInstance()->get_lcore_mask()) + 2;// +2 是gwcliprc和register
		s_pid_check_times = new int[monitor_count]{0};
		check_gw_monitor();
    	// 检查子进程是否已全部启动
    	int check_times = 150;// 最多等待15s
    	while (g_run_status && check_times > 0) {
    	    if(check_if_all_child_up()) {
    	        break;
    	    }
    	    check_times--;
    	    usleep(100);
    	}
    	if(check_if_all_child_up()) {
    		kill_all_child();
    	    g_run_status = 0;
    	    LOG_FATAL("not all child process is working on the beginning, exiting...");
    	}
		mt_sleep(10000);// 等待所有进程启动完成
	} else {
		LOG_INFO("-------secondary[pid:%d] core[%d] start-------", getpid(), g_core_id);
		tgg_gwrcv_secondary_init();
		if (tgg_setup_gw_monitor(g_core_id) < 0) {// 上一个进程尚未结束
			LOG_INFO("-------secondary core[%d] exit, prev coreid still running-------", g_core_id);
			mt_uninit_frame();
    		rte_eal_cleanup();
    		AsyncLogger::getInstance().shutdown();
    		return 0;
		}
		tgg_recv_clean_prev();
	}
	tgg_gw_master();
	if(rte_eal_process_type() == RTE_PROC_PRIMARY) {
		wait_all_child_exit();
		LOG_INFO("-------master core[%d] exit-------", g_core_id);
		delete[] s_pid_check_times;
		tgg_master_uninit();
	} else {
		LOG_INFO("-------secondary core[%d] exit-------", g_core_id);
	}
	print_mem_statistics();
	mt_uninit_frame();
    rte_eal_cleanup();
    AsyncLogger::getInstance().shutdown();
	return 0;
}

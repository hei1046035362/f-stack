#include <stdio.h>
#include <stdlib.h>
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
extern struct rte_mempool* g_mempool_read_data;
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

static int consume_rdata(int clt_fd, const char* buf, int len, enum FD_OPT opt)
{
	g_tgg_stats.en_read_stats.malloc_st++;
	tgg_read_data rdata = {0};
	rdata.fd = clt_fd;
	rdata.coreid = g_core_id;
	rdata.idx = tgg_get_cli_idx(rdata.coreid, clt_fd);
	rdata.fd_opt = opt;
	rdata.data_len = len;
	rdata.data = (void*)buf;
    WsConsumer cons;
    int ret = cons.ConsumerData(&rdata);
    if(cons.SendedClose()) {
    	ret |= 0x0100;
    }
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
	if (consume_rdata(cli_fd, "", 0, FD_NEW) < 0) {
		LOG_ERROR("send new connection[%d] to cliprc failed, core id:%d idx:%d.", cli_fd, g_core_id, idx);
		close(cli_fd);
		tgg_close_cli(g_core_id, cli_fd);
		return;
	}
	char buf[4 * 1024] = {0};
	while (g_run_status) {
		// 1、接收数据  mt_recv在没有数据包的情况下会阻塞，让出cpu给其他的action执行
		ret = mt_recv(cli_fd, (void *)buf, 4 * 1024, 0, s_fd_timeout);
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
			LOG_ERROR("recv from client ret:%d error:%d, idx:%d.", ret, strerror(errno), idx);
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
				LOG_DEBUG("fd:%d idx:%d recv data:%s.", cli_fd, tgg_get_cli_idx(g_core_id, cli_fd), (char*)buf);
			} else {// 其他消息
	    		LOG_DEBUG("fd:%d idx:%d revc data:%s.", cli_fd, tgg_get_cli_idx(g_core_id, cli_fd), bin2hex(std::string((char*)buf, ret)).c_str());
			}
		}
		consume_ret = consume_rdata(cli_fd, buf, ret, FD_READ);
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
		if(consume_ret & 0x0100) {// 已经发送过关闭帧了
			break;
		}
	}
	if(!consume_ret || !(consume_ret & 0x0100)) {// 没发送过close
		consume_rdata(cli_fd, NULL, 0, FD_CLOSE);
	}
	LOG_DEBUG("wait client[%d] close...", cli_fd);
	// 等待连接在缓存中的数据被消费完才能关闭
	int index = 1000*60;// 最长等待1分钟，关闭包会在trans队列中可能多次enqueue back，尽可能让数据包走正常流程关闭
	while(tgg_get_cli_idx(g_core_id, cli_fd) != TGG_FD_CLOSING && index > 0 && g_run_status) {
	    mt_sleep(10);
	    index--;
	}
	if(index <= 0) {
        LOG_WARNING("recv close fram from gwprc timeout, coreid[%d] fd[%d].", g_core_id, cli_fd);		
	}
	close(cli_fd);// 这里不能使用mt_close,mt_close只设置标记，不会发送fin包，fd依然还存在
	clean_client_data(cli_fd, idx);
	LOG_WARNING("client[%d] closed.", cli_fd);
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
			if (wdata->fd_opt & FD_WRITE) {
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
				if(wdata->fd_opt & FD_WRITE) {
					mt_sleep(1000);// ws的关闭帧发送完以后等待客户端先关闭，如果1s后没有关闭，我们要主动结束
									// 到了这里后面的数据其实都应该要丢弃了，所以后续数据已经不重要了
				}
				mt_close(cli_fd);// TODO:待优化，在这里结束可能会报错，四次挥手不完整：epoll schedule failed, errno: 62
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
	clean_fdidlist(wdata->lst_fd);
	if(wdata->data) {
		memset(wdata->data, 0, wdata->data_len);
		high_freq_free(g_mempool_write_data, wdata->data, wdata->data_len);
	}
	memset(wdata, 0, sizeof(tgg_write_data));
	high_freq_free(g_mempool_write, wdata, sizeof(tgg_write_data));
}

static void tgg_send(void *arg)
{
	while(g_run_status) {
	    tgg_write_data* wdata = NULL;
	    if (tgg_dequeue_write(g_core_id, &wdata) < 0) {
	    	// 队列空
			mt_sleep(10);
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
	mt_start_thread((void *)tgg_send, NULL);

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
			mt_sleep(10);
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

	mt_init_frame(argc, argv);
	g_core_id = rte_lcore_id();
	if(rte_eal_process_type() == RTE_PROC_PRIMARY) {
		LOG_INFO("-------master core[%d] start-------", g_core_id);
		tgg_master_init();
	} else {
		LOG_INFO("-------secondary core[%d] start-------", g_core_id);
		tgg_gwrcv_secondary_init();
	}
	tgg_sig_init();// 信号处理初始化
	initOpenSSL();// 初始化ssl加解密环境
	tgg_gw_master();
	if(rte_eal_process_type() == RTE_PROC_PRIMARY) {
		LOG_INFO("-------master core[%d] exit-------", g_core_id);
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

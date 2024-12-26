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

static const char* s_dump_file = "/var/corefiles/tgg_gw_master_core";
static int s_fd_timeout = 60*1000;
extern const char* g_rte_malloc_type;
extern struct rte_mempool* g_mempool_read;
extern struct rte_mempool* g_mempool_write;
extern struct rte_ring* g_ring_read;
extern ushort g_gateway_port;
extern tgg_stats g_tgg_stats;
extern int g_fd_limit;
extern int g_core_id;

// 进程是否退出  master进程退出不需要做什么事情，但是secondary退出前必须要释放他持有的内存
int g_run_status = 1;

using namespace NS_MICRO_THREAD;


// TODO 是否要改为通过fd获取ip尚未确定，目前还是采用的配置中的ip地址，会有一定的局限性
// 通过fd获取本机地址，但是只有当有客户端连上来的时候才能获取，有一定延迟，通过服务端自己的fd只能获取到0.0.0.0
uint32_t get_local_addr(int sockfd)
{
     // 获取IP地址信息
     struct sockaddr_in local_addr;
     socklen_t addrlen = sizeof(local_addr);
     if (ff_getsockname(sockfd, (struct linux_sockaddr *)&local_addr, &addrlen) == -1) {
         perror("getsockname");
         close(sockfd);
         return 0;
     }
     char ip_str[INET_ADDRSTRLEN];
     inet_ntop(AF_INET, &(local_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
     // printf("ip str:%s\n", ip_str);
     struct in_addr ip_addr;
     inet_pton(AF_INET, ip_str, &ip_addr);
     uint32_t ip_decimal = big_endian() ? ntohl(ip_addr.s_addr) : ip_addr.s_addr;
     printf("IP address in decimal: %u\n", ip_decimal);
     return ip_decimal;
}

int get_remote_info(int sockfd, uint32_t& ip, ushort& port)
{
     // 获取IP地址信息
     struct sockaddr_in local_addr;
     socklen_t addrlen = sizeof(local_addr);
     if (ff_getpeername(sockfd, (struct linux_sockaddr *)&local_addr, &addrlen) < 0) {
         perror("getsockname");
         close(sockfd);
         return -1;
     }
     char ip_str[INET_ADDRSTRLEN];
     inet_ntop(AF_INET, &(local_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
     // printf("ip str:%s\n", ip_str);
     struct in_addr ip_addr;
     inet_pton(AF_INET, ip_str, &ip_addr);
     ip = big_endian() ? ntohl(ip_addr.s_addr) : ip_addr.s_addr;
     port = big_endian() ? ntohl(local_addr.sin_port) : local_addr.sin_port;
     printf("IP address in decimal: %u\n", ip);
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
		fprintf(stderr, "create tcp socket failed, error: %m\n");
		return -1;
	}
	if (set_fd_nonblock(fd) == -1) {
		fprintf(stderr, "set tcp socket nonblock failed\n");
		return -1;
	}

	return fd;
}

static int tgg_recv_enqueue(int clt_fd, const char* buf, int len, enum FD_OPT opt)
{
	// 1、申请交互数据结构的内存空间，并填充数据
	tgg_read_data* rdata = NULL;
	int idx = 0; // 前期调试性能需要，后期可根据需求屏蔽
	while (rte_mempool_get(g_mempool_read, (void**)&rdata) < 0) {
		// TODO  分配内存失败后的处理，  暂时采用休眠的方式等待消费端消费完释放空间，这里应该就能成功了
		idx++;
		usleep(10);
	}
	if (idx) {
		RTE_LOG(WARNING, USER1, "[%s][%d] Get data from mempool[%s] failed times:%d", 
			__func__, __LINE__, g_mempool_read->name, idx);
		idx = 0;
	}
	g_tgg_stats.en_read_stats.malloc_st++;
	rdata->fd = clt_fd;
	rdata->coreid = g_core_id;
	rdata->idx = tgg_get_cli_idx(rdata->coreid, clt_fd);
	rdata->fd_opt = opt;
	rdata->data_len = len;
	if (rdata->data_len > 0) {
		rdata->data = dpdk_rte_malloc(rdata->data_len);
		if (!rdata->data) {
			// TODO 记录失败次数
			RTE_LOG(WARNING, USER1, "[%s][%d] malloc data failed.", __func__, __LINE__);
			// 分配内存失败，获取的入队列结构体要放回内存池
			rte_mempool_put(g_mempool_read, rdata);
			return -1;
		}
		g_tgg_stats.en_read_stats.malloc_data++;
		memcpy(rdata->data, buf, rdata->data_len);
	}

	// 2、入队列
	while (rte_ring_full(g_ring_read)) {
		// TODO  队列满的情况，  暂时采用休眠的方式等待消费端消费完释放空间，这里应该就能成功了
		idx++;
		usleep(10);
	}
	if (idx) {
		RTE_LOG(WARNING, USER1, "[%s][%d] enqueue data to ring[%s] failed times:%d", 
			__func__, __LINE__, g_ring_read->name, idx);
	}

	if (tgg_enqueue_read(rdata) < 0) {
		// TODO 统计失败计数，是否要重入队列？
		memset(rdata->data, 0, rdata->data_len);
		rte_free(rdata->data);
		memset(rdata, 0, sizeof(tgg_read_data));
		rte_mempool_put(g_mempool_read, rdata);
	} else {
		g_tgg_stats.en_read_stats.enqueue++;
	}
	return 0;
}

static void tgg_recv(void *arg)
{
	int ret;
	int *p = (int *)arg;
	int clt_fd = *p;
	delete p;
	uint32_t ip;
	ushort port;
	if (get_remote_info(clt_fd, ip, port) < 0) {
		close(clt_fd);
		return;
	}
	if(tgg_init_cli(g_core_id, clt_fd, ip, port) < 0) {
		close(clt_fd);
		tgg_close_cli(g_core_id, clt_fd);
		return;
	}
	// tgg_set_cli_idx(0);
	int status = tgg_get_cli_status(g_core_id, clt_fd);
	// 通知后台有新的连接
	if (tgg_recv_enqueue(clt_fd, "", 0, FD_NEW) < 0) {
		close(clt_fd);
		tgg_close_cli(g_core_id, clt_fd);
		return;
	}
	while (g_run_status) {
		char buf[64 * 1024] = {0};
		// 1、接收数据  mt_recv在没有数据包的情况下会阻塞，让出cpu给其他的action执行
		ret = mt_recv(clt_fd, (void *)buf, 64 * 1024, 0, s_fd_timeout);
		if(ret == -1 && errno == ETIME) {
			printf("client heart beat timeout.\n");
			break;
		}
		if(ret == -4) {
			// 主动断开连接
			RTE_LOG(ERR, USER1, "[%s][%d] closing connection affected.\n", __func__, __LINE__);
			break;
		}
		g_tgg_stats.recv++;
		if (ret < 0) {
			// 接收出现错误
			printf("recv from client error:%d.\n", ret);
			break;
		}
		if (!ret) {
			// 对端主动关闭了
			printf("recv close from client.\n");
			break;
		}
		printf("recv:%s\n", (char*)buf);
		// enum FD_OPT opt;
		// if (!status) {
		// 	// 首次连接
		// 	status |= FD_STATUS_NEWSESSION;
		// 	tgg_set_cli_status(clt_fd, status);
		// 	opt = FD_NEW;
		// } else {
			// 后续数据包
		// 	opt = FD_READ;
		// }

		// 入队列失败，内存不够了，直接退出循环关闭连接
		if (tgg_recv_enqueue(clt_fd, buf, ret, FD_READ) < 0)
			break;
	}
	// 对端主动关闭了
	// memset(cli->uid, 0, sizeof(cli->uid));
	status = FD_STATUS_CLOSING;
	tgg_set_cli_status(g_core_id, clt_fd, status);
	// 为确保fd正确关闭,对应的内存正确释放,就必须要入队列一个关闭的操作
	if (ret)  // 不是对端主动关闭的情况，服务端要主动发送关闭消息
		tgg_recv_enqueue(clt_fd, NULL, 0, FD_CLOSE);

	RTE_LOG(INFO, USER1, "[%s][%d] wait client[%d] close...\n", __func__, __LINE__, clt_fd);
	// 等待连接在缓存中的数据被消费完才能关闭
	int index = 1000;// TODO 防止因process宕机丢包导致无法停止的问题，10s这个时间待商榷
	while(tgg_get_cli_idx(g_core_id, clt_fd) != TGG_FD_CLOSING && index > 0) {
	    mt_sleep(10);
	    index--;
	}
	close(clt_fd);
	tgg_close_cli(g_core_id, clt_fd);
	RTE_LOG(INFO, USER1, "[%s][%d] client[%d] closed.\n", __func__, __LINE__, clt_fd);
}

static void tgg_do_send(tgg_write_data* wdata)
{
	tgg_fd_list* fd_list = wdata->lst_fd;
	while (fd_list) {
		int cli_fd = fd_list->fdid;// 数据传递时fdid存的是fd
		int idx = tgg_get_cli_idx(g_core_id, cli_fd);

		// 只有未关闭的连接才需要走以下逻辑，已经关闭的连接，不再发送数据
		if(idx >= 0) {
			// 新的连接旧的数据就不要发送了，直接清理空间
			if (idx != fd_list->idx) {// 后台推送给前端时，可能会出现这种情况
				RTE_LOG(ERR, USER1, "[%s][%d] Idx Changed, Closing Connection[%d].\n", __func__, __LINE__, cli_fd);
				// tgg_del_idx(fd_list->idx);
				// tgg_set_cli_idx(cli_fd, TGG_FD_CLOSING);
				continue;
			}

			// 是否需要发送数据
			if (wdata->fd_opt & FD_WRITE) {
				int ret = mt_send(cli_fd, (void *)wdata->data, wdata->data_len, 0, 1000);
				if (ret == -4) {
					// 主动断开连接
					RTE_LOG(ERR, USER1, "[%s][%d] closing connection affected.\n", __func__, __LINE__);
				} else if (ret < 0) {
					RTE_LOG(ERR, USER1, "[%s][%d] send data to client fd[%d] idx[%d] error, ret[%d]\n", 
						__func__, __LINE__, cli_fd, wdata->idx, ret);
				} else {
					g_tgg_stats.en_read_stats.enqueue++;
				}
			}

			if ( wdata->fd_opt & FD_CLOSE) {
				RTE_LOG(INFO, USER1, "[%s][%d] Closing Connection[%d].\n", __func__, __LINE__, cli_fd);
				tgg_del_idx(fd_list->idx);
				tgg_set_cli_idx(g_core_id, cli_fd, TGG_FD_CLOSING);
				mt_close(cli_fd);
			}
		}

		wdata->lst_fd = wdata->lst_fd->next;
		memset(fd_list, 0, sizeof(tgg_fd_list));
		rte_free(fd_list);
		fd_list = wdata->lst_fd;
	}
	// 所有fd都发送完了之后，需要清理并回收内存
	memset(wdata->data, 0, wdata->data_len);
	rte_free(wdata->data);
	memset(wdata, 0, sizeof(tgg_write_data));
	rte_mempool_put(g_mempool_write, wdata);	
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
			mt_sleep(1);
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
	addr.sin_port = big_endian() ? htons(g_gateway_port) : g_gateway_port;

	int fd = create_tcp_sock();
	if (fd < 0) {
		fprintf(stderr, "create listen socket failed\n");
		return -1;
	}

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		fprintf(stderr, "bind failed [%m]\n");
		return -1;
	}

	if (listen(fd, 1024) < 0) {
		close(fd);
		fprintf(stderr, "listen failed [%m]\n");
		return -1;
	}
    int clt_fd = 0;
	int *p;
	while (g_run_status) {
		struct sockaddr_in client_addr;
		int addr_len = sizeof(client_addr);

        clt_fd = mt_accept(fd, (struct sockaddr*)&client_addr, (socklen_t*)&addr_len, -1);
		if (clt_fd < 0) {
			mt_sleep(10);
			continue;
		}
		if (clt_fd >= g_fd_limit - 1)	{
			RTE_LOG(INFO, USER1, "given fd[%d] is invalid,[0,%d]",
				fd, g_fd_limit - 1);
			mt_sleep(10);
			continue;
		}
		// 如果fd还在使用中，拒绝连接
		if (tgg_get_cli_idx(g_core_id, clt_fd) != TGG_FD_CLOSED) {
			fprintf(stderr, "socket fd[%d] still in use.\n", clt_fd);
			close(clt_fd);
			continue;
		}
		// TODO 获取ip的方式待商榷
		// uint32_t ip_int = get_local_addr(clt_fd);
		if (set_fd_nonblock(clt_fd) == -1) {
			fprintf(stderr, "set clt_fd nonblock failed [%m]\n");
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
	mt_init_frame(argc, argv);
	g_core_id = rte_lcore_id();
	if(rte_eal_process_type() == RTE_PROC_PRIMARY) {
		tgg_master_init();
	} else {
		tgg_secondary_init();
	}
	tgg_gw_master();
	if(rte_eal_process_type() == RTE_PROC_PRIMARY) {
		tgg_master_uninit();
		mt_uninit_frame();
	} else {
		tgg_secondary_uninit();
	}
}

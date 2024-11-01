#include <stdio.h>
#include <stdlib.h>
#include "mt_incl.h"
#include "micro_thread.h"
#include <rte_mempool.h>
#include "tgg_common.h"
#include "tgg_struct.h"
#include "tgg_init.h"


static const char* s_dump_file = "/var/corefiles/tgg_gw_master_core"

extern const char* g_rte_malloc_type;
extern const struct rte_mempool* g_mempool_read;
extern const struct rte_mempool* g_mempool_write;
extern ushort g_gateway_port;

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
     uint32_t ip_decimal = ntohl(ip_addr.s_addr);
     printf("IP address in decimal: %u\n", ip_decimal);
     return ip_decimal;
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

static int tgg_recv_enqueue(int clt_fd, char* buf, int len, enum FD_OPT opt)
{
	// 1、申请交互数据结构的内存空间，并填充数据
	tgg_read_data* rdata = NULL;
	int idx = 0; // 前期调试性能需要，后期可根据需求屏蔽
	while (rte_mempool_get(g_mempool_read, rdata) < 0) {
		// TODO  分配内存失败后的处理，  暂时采用休眠的方式等待消费端消费完释放空间，这里应该就能成功了
		idx++;
		usleep(10);
	}
	if (idx) {
		RTE_LOG(WARNING, USER1, "[%s][%d] Get data from mempool[%s] failed times:%d", 
			__func__, __LINE__, g_mempool_read->name, idx);
		idx = 0;
	}
	g_tgg_stats->en_read_stats.malloc_st++;
	rdata->fd = clt_fd;
	rdata->fd_opt = opt;FD_READ
	rdata->data_len = ret;
	if (rdata->data_len > 0) {
		rdata->data = dpdk_rte_malloc(rdata->data_len);
		if (!rdata->data) {
			// TODO 记录失败次数
			RTE_LOG(WARNING, USER1, "[%s][%d] malloc data failed.", __func__, __LINE__);
			// 分配内存失败，获取的入队列结构体要放回内存池
			rte_mempool_put(g_mempool_read, rdata);
			return -1;
		}
		g_tgg_stats->en_read_stats.malloc_data++;
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
		rte_free(rdata->data);
		memset(rdata, 0, sizeof(tgg_read_data))
		rte_mempool_put(g_mempool_read, rdata);
	} else {
		g_tgg_stats->en_read_stats.enqueue++;
	}
	return 0;
}

static void tgg_recv(void *arg)
{
	int ret;
	int *p = (int *)arg;
	int clt_fd = *p;
	delete p;
	char buf[64 * 1024];
	int status = tgg_get_cli_status(clt_fd);
	if (!(status & FD_STATUS_READYFORCONNECT )) {
		// TODO 如果fd已经被协议栈回收并再次使用了，但是网关内关于这个fd的操作尚未完成，是否还有更好的方案？
		// 如果上一个fd在系统中还没有操作完，那就先不让连，然后去使用其他的fd
		close(clt_fd);
		return;
	}
	while (g_run_status) {
		// 1、接收数据  mt_recv在没有数据包的情况下会阻塞，让出cpu给其他的action执行
		ret = mt_recv(clt_fd, (void *)buf, 64 * 1024, 0, -1);
		g_tgg_stats->recv++;
		if (ret < 0) {
			// 连接主动断开了
			printf("recv from client error\n");
			break;
		}
		enum FD_OPT opt;
		if (status & FD_STATUS_NEWSESSION) {
			// 首次连接
			status |= FD_STATUS_NEWSESSION;
			tgg_set_cli_status(cli_fd, status);
			opt = FD_NEW;
		} else {
			// 后续数据包
			opt = FD_READ;
		}

		// 入队列失败，内存不够了，直接退出循环关闭连接
		if (tgg_recv_enqueue(clt_fd, buf, ret, opt) < 0)
			break;
	}
	// 对端主动关闭了
	// memset(cli->uid, 0, sizeof(cli->uid));
	status = FD_STATUS_CLOSING;
	tgg_set_cli_status(cli_fd, status);
	// 为确保fd正确关闭,对应的内存正确释放,就必须要入队列一个关闭的操作
	while (tgg_recv_enqueue(clt_fd, NULL, 0, FD_CLOSE) < 0) {

	}
	close(clt_fd);
}

static void tgg_do_send(tgg_write_data* wdata)
{
	while (wdata->lst_fd->next) {
		int cli_fd = wdata->lst_fd->next->fd;
		int status = tgg_get_cli_status(cli_fd);
		// 连接已关闭就不需要发送了，直接清理空间
		if (status & FD_STATUS_CLOSING) {
			RTE_LOG(ERR, USER1, "[%s][%d] Connection is already closed.", __func__, __LINE__);
			wdata->fd_opt &= FD_CLOSE;
			goto DO_SEND_CLOSE;
		}

		// 是否需要发送数据
		if ((wdata->fd_opt & FD_WRITE) && 
			(status & (FD_STATUS_NEWSESSION | FD_STATUS_CONNECTED)) ) {
			ret = mt_send(clt_fd, (void *)wdata->data, wdata->data_len, 0, 1000);
			if (ret < 0) {
				RTE_LOG(ERR, USER1, "[%s][%d] send data to client[%s] error, ret[%d]", __func__, __LINE__, cli->uid, ret);
			} else {
				g_tgg_stats->en_read_stats.enqueue++;
			}
		}
DO_SEND_CLOSE:
		// 是否需关闭连接
		if (wdata->fd_opt & FD_CLOSE) {
			tgg_close_cli(cli_fd);
			close(cli_fd);
		}
		tgg_fd_list* tmp = wdata->lst_fd->next;
		wdata->lst_fd->next = wdata->lst_fd->next->next;
		memset(tmp, 0, sizeof(tgg_fd_list));
		rte_free(tmp);
	}
	memset(wdata->lst_fd, 0, sizeof(tgg_fd_list));
	memset(wdata->data, 0, wdata->data_len);
	rte_free(wdata->lst_fd);
	rte_free(wdata->data);
	memset(wdata, 0, sizeof(tgg_write_data));
	rte_mempool_put(g_mempool_write, wdata);	
}

static void tgg_send(void *arg)
{
	while(g_run_status) {
	    tgg_write_data* wdata = NULL;
	    if (tgg_dequeue_write(&wdata) < 0) {
	    	// 队列空
	    	usleep(10);
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
	addr.sin_port = htons(g_gateway_port);

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
			mt_sleep(1);
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
	return 0;
}

int main(int argc, char *argv[])
{
	init_core(s_dump_file);
	mt_init_frame(argc, argv);
	tgg_master_init();
	tgg_gw_master();
	tgg_master_uninit();
}

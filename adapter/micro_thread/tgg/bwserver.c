#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>

#include "bwserver.h"
#include "ff_api.h"
#include "tgg_comm/tgg_struct.h"
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/tgg_bwcomm.h"
#include "cmd/CmdProcessor.h"
#include <map>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_mempool.h>

#define PORT 8888
#define BUFFER_SIZE 1024
#define MAX_CLIENTS 10


static 	pthread_t s_bwserver_thread;

extern std::map<int, tgg_bw_info*> g_map_bwinfo;

static volatile int s_idx = 0;// 防止同一个fd被重用了，内存中的数据任然往新的连接中发送数据
extern struct rte_mempool* g_mempool_bwrcv;

void tgg_process_bwrcv_data(void* arg)
{
    tgg_bw_data* bdata = (tgg_bw_data*)arg;
    // tgg_bw_info* binfo = g_map_bwinfo[bdata->fd];
    CmdBaseProcessor* pro = get_cmd_processor(bdata->fd, arg);
    pro->ExecCmd();
    // TODO BW断开后，之前没有处理完的事情是否要继续处理
    // if (bdata->fd_opt & FD_CLOSE || 
    //     binfo->idx != bdata->idx ||
    //     binfo->status & (FD_STATUS_CLOSING | FD_STATUS_CLOSED)) {
    //     clean_bw_data(bdata);
    // }

}

static void* bwserver_routine(void* data)
{
	int server_socket, client_sockets[MAX_CLIENTS];
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len;
    char buffer[BUFFER_SIZE];
    fd_set read_fds;
    int max_fd;
    int i;

    // 创建服务器套接字
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Error opening socket");
        exit(1);
    }

    // 初始化服务器地址结构
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    // 绑定套接字到指定端口
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Error on binding");
        exit(1);
    }

    // 开始监听连接请求
    listen(server_socket, 5);

    // 初始化客户端套接字数组
    for (i = 0; i < MAX_CLIENTS; i++) {
        client_sockets[i] = 0;
    }

    max_fd = server_socket;

    while (1) {
        // 清空文件描述符集合
        FD_ZERO(&read_fds);

        // 将服务器套接字加入集合
        FD_SET(server_socket, &read_fds);

        // 将已连接的客户端套接字加入集合
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (client_sockets[i] > 0) {
                FD_SET(client_sockets[i], &read_fds);
            }
            if (client_sockets[i] > max_fd) {
                max_fd = client_sockets[i];
            }
        }

        // 使用 select 等待可读事件
        int activity = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (activity < 0) {
            perror("Error in select");
            exit(1);
        }

        // 检查服务器套接字是否有新连接请求
        if (FD_ISSET(server_socket, &read_fds)) {
            int new_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
            if (new_socket < 0) {
                perror("Error on accept");
                continue;
            }

            // 找到一个空闲的位置存储新连接的套接字
            for (i = 0; i < MAX_CLIENTS; i++) {
                if (client_sockets[i] == 0) {
                    tgg_bw_info* bwinfo = get_valid_bwinfo_by_fd(new_socket);
                    bwinfo->idx = ++s_idx;
                    bwinfo->status = FD_STATUS_NEWSESSION;
                    if (get_connection_info(new_socket, bwinfo->ip_str, &bwinfo->port) < 0) {
                        close(new_socket);
                        delete(bwinfo);
                    } else {
                        client_sockets[i] = new_socket;
                    }
                    break;
                }
            }
        }

        // 检查已连接的客户端套接字是否有数据可读
        for (i = 0; i < MAX_CLIENTS; i++) {
            int client_socket = client_sockets[i];
            if (FD_ISSET(client_socket, &read_fds)) {
                memset(buffer, 0, BUFFER_SIZE);
                int bytes_read = read(client_socket, buffer, BUFFER_SIZE - 1);
                tgg_bw_data* bdata = NULL;
                if (rte_mempool_get(g_mempool_bwrcv, (void**)&bdata) < 0) {
                    RTE_LOG(ERR, USER1, "[%s][%d] Get mem from bwrcv mempool failed.", __func__, __LINE__);
                    continue;
                }
                bdata->fd = client_socket;
                if (bytes_read <= 0) {
                    bdata->fd_opt = FD_CLOSE;
                    // BW断开连接
                    close(client_socket);
                    client_sockets[i] = 0;
                } else {
                    RTE_LOG(ERR, USER1, "[%s][%d] Received from BW %d: %s", __func__, __LINE__, i, buffer);
                    bdata->data = dpdk_rte_malloc(bytes_read);
                    if (!bdata->data)
                    {
                        rte_free(bdata);
                        continue;
                    }
                    bdata->fd_opt = FD_READ;
                    bdata->data_len = bytes_read;
                    // 暂时没有上行的业务
                    // write(client_socket, buffer, strlen(buffer));
                }
                if(tgg_enqueue_bwrcv(bdata) < 0) {
                    rte_mempool_put(g_mempool_bwrcv, bdata);
                }
            }
        }
    }

    // 关闭服务器套接字
    close(server_socket);
    return NULL;
}

int init_bwserver()
{
	return ff_pthread_create(&s_bwserver_thread, NULL, &bwserver_routine, NULL);
}

void uninit_bwserver()
{
	void* retval = NULL;
	if (ff_pthread_join(s_bwserver_thread, &retval) < 0) {
		perror("join thread failed.");
	}
    std::map<int, tgg_bw_info*>::iterator it_info = g_map_bwinfo.begin();
    while(it_info != g_map_bwinfo.end()) {
        delete(it_info->second);
    }
}
#ifndef _TGG_STRUCT_H_
#define _TGG_STRUCT_H_
#include <rte_build_config.h>
#include <netinet/in.h>
// #define CACHE_LINE_SIZE 64
#define SECRET_KEY_LEN 20

#define TGG_CID_LEN 24
#define TGG_UID_LEN 24
#define TGG_GID_LEN 24


#define TGG_FD_CLOSING -1
#define TGG_FD_CLOSED -2

// 应用层协议类型
enum L4_TYPE
{
	L4_TYPE_HTTP,
	L4_TYPE_WEBSOCK	
};

enum AUTH_TYPE
{
	AUTH_TYPE_UNKNOWN = 0,
	AUTH_TYPE_HANDLESHAKED,
	AUTH_TYPE_TOKENCHECKED
};

// 需要对fd进行的操作类型
enum FD_OPT
{
	FD_NEW = 1,
	FD_READ = 2,
	FD_WRITE = 4,
	FD_CLOSE = 8
};

// fd的状态
enum FD_STATUS
{
	FD_STATUS_READYFORCONNECT = 0,
	FD_STATUS_NEWSESSION = 1,
	FD_STATUS_CONNECTED = 2,	// 有这个状态才能发包
	FD_STATUS_BIND = 4,	// 有这个状态才能发包
	FD_STATUS_KEEP = 8,
	FD_STATUS_CLOSING = 16,  // 设置这个标记以后，关于这个fd的所有操作都要停止了，除了清理内存
	FD_STATUS_CLOSED = 32	// 这个状态下或者为0才能接收新的连接
};

#define MAX_WSDATA_LEN 10*1024*1024   // websocket最多缓存10M的数据
typedef struct st_ws_unit {
	int len;
	int pos;	// 偏移量，
				// 1、方便取数据的时候通过偏移量直接取到有效数据部分
				// 2、这里的data存放的是fd读取的数据，为减少拷贝，不能改变data的位置，所以加一个pos
	void* data;
	struct st_ws_unit *next;
} tgg_ws_unit;

typedef struct st_ws_data {
	int total_len;		// 限制可接收数据长度，防止内存涨爆
	int head_complete;
	tgg_ws_unit* data_list;
} tgg_ws_data;

// 客户端需要保留的信息
// TODO:是否要考虑断线重连之后上一个连接的数据包会发送到新的连接中来的问题
typedef struct st_cli_info {
	int status;		// 连接是否已关闭						master 填充
	int idx;		// 和fd一起标识唯一连接，(fd可能被重用了,但是处理方仍不知情)
					// -1 标识关闭中，后续的数据包不再处理，0标识关闭完成并准备就绪
	int authorized; // 连接确认
	tgg_ws_data* ws_data;	// 缓存websocket的数据，用于处理分包的情况下
	// int need_keep;	// 是否为长连接                     	process填充
	// int l4_type;	// 应用层协议类型，http/websocket		process填充
	char cid[TGG_CID_LEN];	// client id 							process 填充
	char uid[TGG_UID_LEN];	// user id 							process 填充
	char reserved[128];	// reserved
} __attribute__((aligned(RTE_CACHE_LINE_SIZE))) tgg_cli_info;

// BW连接信息
typedef struct st_bw_info {
	int fd;			// BW连接的fd
	int status;		// 连接状态
	int idx;		// 和fd共同标识唯一一个连接(fd是可重用的)
	int load;		// BW的负载情况，用于计算负载均衡
	int authorized; // 连接确认
    unsigned short port;
    char ip_str[INET_ADDRSTRLEN];
    char secretKey[SECRET_KEY_LEN];
} tgg_bw_info;

// TODO list 存储BW的fd


// master收到数据后传给process处理，入队列时填充
typedef struct st_read_data {
	int fd;			// socket fd
	int fd_opt;
	int idx;
	int data_len;
	void* data;		// 携带的数据
} __attribute__((aligned(RTE_CACHE_LINE_SIZE))) tgg_read_data;

// list<fd>
typedef struct st_tgg_fd_list {
	int fd;
	int idx;
	struct st_tgg_fd_list* next;
} tgg_fd_list;

// process回传给master处理
typedef struct st_write_data {
	tgg_fd_list* lst_fd;			// socket fd(可能存在同时发多个fd)
	int fd_opt;		// 对fd的操作类型(写/关闭)
	int idx;
	int data_len;
	void* data;		// 携带的数据
} __attribute__((aligned(RTE_CACHE_LINE_SIZE))) tgg_write_data;

// bw数据处理传输结构
typedef struct st_read_data tgg_bw_data;

typedef struct st_tgg_conf {

} tgg_conf;

typedef struct st_en_queue_stats {
	int malloc_st;
	int malloc_data;
	int enqueue;
} tgg_en_queue_stats;
// 统计收发数据
typedef struct st_stats {
	int recv;			// 接收次数
	tgg_en_queue_stats en_read_stats;	// 入读队列次数
	int dequeue_read;	// 出读队列次数
	tgg_en_queue_stats en_write_stats;	// 入写队列次数
	int dequeue_write;	// 出写队列次数
	int send;			//  发送数
} tgg_stats;

// 进程信息
typedef struct st_pid_data {
	pid_t pid;		// 进程id									父进程写入
	uint64_t heard_beat;	// 心跳   防止进程无响应，队列无人消费			父进程写入，子进程通过信号通知并重置计数
	int idx;		// 索引   进程索引，标记进程能使用的队列		子进程写入和使用
} pid_data;

// gid hash data
typedef tgg_fd_list tgg_gid_data;

// uid hash data
typedef tgg_fd_list tgg_uid_data;


typedef struct st_list_iddata {
	char data[TGG_GID_LEN];
	struct st_list_iddata* next;
} tgg_list_id;

typedef tgg_list_id tgg_gid_list;

// cid hash value
typedef struct st_tgg_cid_data {
	int fd;
	// char* uid;
	// struct st_list_gid* lst_gid;  // 考虑使用时直接从redis获取
} tgg_cid_data;



#endif // _TGG_STRUCT_H_
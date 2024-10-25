#ifndef _TGG_STRUCT_H_
#define _TGG_STRUCT_H_

// #define CACHE_LINE_SIZE 64

// 应用层协议类型
enum L4_TYPE
{
	L4_TYPE_HTTP,
	L4_TYPE_WEBSOCK	
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
	FD_STATUS_KEEP = 4,
	FD_STATUS_CLOSING = 8,  // 设置这个标记以后，关于这个fd的所有操作都要停止了，除了清理内存
	FD_STATUS_CLOSED = 16	// 这个状态下或者为0才能接收新的连接
};

// 客户端需要保留的信息
// TODO:是否要考虑断线重连之后上一个连接的数据包会发送到新的连接中来的问题
typedef struct st_cli_info {
	int status;		// 连接是否已关闭						master 填充
	// int need_keep;	// 是否为长连接                     	process填充
	// int l4_type;	// 应用层协议类型，http/websocket		process填充
	char[20] cid;	// client id 							process 填充
	char[20] uid;	// user id 							process 填充
	char[128] reserved;	// reserved
} __attribute__((aligned(CACHE_LINE_SIZE))) tgg_cli_info;

// BW连接信息
typedef struct st_bw_info {
	int fd;			// BW连接的fd
	int status;		// 连接状态
	int load;		// BW的负载情况，用于计算负载均衡
}

// TODO list 存储BW的fd


// master收到数据后传给process处理，入队列时填充
typedef struct st_read_data {
	int fd;			// socket fd
	int fd_opt;
	int data_len;
	void* data;		// 携带的数据
} __attribute__((aligned(CACHE_LINE_SIZE))) tgg_read_data;

// process回传给master处理
typedef struct st_write_data {
	int fd;			// socket fd
	int fd_opt;		// 对fd的操作类型(写/关闭)
	int data_len;
	void* data;		// 携带的数据
} __attribute__((aligned(CACHE_LINE_SIZE))) tgg_write_data;


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
	int heard_beat;	// 心跳   防止进程无响应，队列无人消费			父进程写入，子进程通过信号通知并重置计数
	int idx;		// 索引   进程索引，标记进程能使用的队列		子进程写入和使用
} pid_data;


// list<fd>
typedef struct st_tgg_fd_list {
	int fd;
	struct st_tgg_fd_list* next;
} tgg_fd_list;


// gid hash data
typedef tgg_fd_list tgg_gid_data;

// uid hash data
typedef tgg_fd_list tgg_uid_data;


typedef struct st_list_gid {
	char* gid;
	struct st_list_gid* next;
} list_gid;
// cid hash value
typedef struct st_tgg_cid_data {
	int fd;
	// char* uid;
	// struct st_list_gid* lst_gid;  // 考虑使用时直接从redis获取
} tgg_cid_data;



#endif // _TGG_STRUCT_H_
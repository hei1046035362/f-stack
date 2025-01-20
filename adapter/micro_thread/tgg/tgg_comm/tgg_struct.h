#ifndef _TGG_STRUCT_H_
#define _TGG_STRUCT_H_
#include <rte_build_config.h>
#include <netinet/in.h>
// #define CACHE_LINE_SIZE 64
#define SECRET_KEY_LEN 32
#define WOKER_KEY_LEN 64

#define MAX_LCORE_COUNT 20    // 最大允许的 lcore个数

#define TGG_CID_LEN 24
#define TGG_UID_LEN 24
#define TGG_GID_LEN 24

#define TGG_IPPORT_LEN 12

#define TGG_FD_CLOSING -1
#define TGG_FD_CLOSED -2
#define TGG_FD_NOTEXIST -3

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
    FD_STATUS_CONNECTED = 2,    // 有这个状态才能发包
    FD_STATUS_BIND = 4,    // 有这个状态才能发包
    FD_STATUS_KEEP = 8,
    FD_STATUS_CLOSING = 16,  // 设置这个标记以后，关于这个fd的所有操作都要停止了，除了清理内存
    FD_STATUS_CLOSED = 32    // 这个状态下或者为0才能接收新的连接
};

#define MAX_WSDATA_LEN 10*1024*1024   // websocket最多缓存10M的数据
typedef struct st_ws_unit {
    int len;
    int pos;    // 偏移量，
                // 1、方便取数据的时候通过偏移量直接取到有效数据部分
                // 2、这里的data存放的是fd读取的数据，为减少拷贝，不能改变data的位置，所以加一个pos
    void* data;
    struct st_ws_unit *next;
} tgg_ws_unit;

typedef struct st_ws_data {
    int total_len;        // 限制可接收数据长度，防止内存涨爆
    int head_complete;
    tgg_ws_unit* data_list;
} tgg_ws_data;

// 客户端需要保留的信息
// TODO:是否要考虑断线重连之后上一个连接的数据包会发送到新的连接中来的问题
typedef struct st_cli_info {
    int status;        // 连接是否已关闭                        master 填充
    int idx;        // 和fd一起标识唯一连接，(fd可能被重用了,但是处理方仍不知情)
                    // -1 标识关闭中，后续的数据包不再处理，0标识关闭完成并准备就绪
    int authorized; // 连接确认
    tgg_ws_data* ws_data;    // 缓存websocket的数据，用于处理分包的情况下
    // int need_keep;    // 是否为长连接                         process填充
    // int l4_type;    // 应用层协议类型，http/websocket        process填充
    int ip;
    unsigned short port;
    int bwfdx;        // 绑定的bw
    char cid[TGG_CID_LEN];    // client id                             process 填充
    char uid[TGG_UID_LEN];    // user id                             process 填充
    char reserved[128];    // reserved
} __attribute__((aligned(RTE_CACHE_LINE_SIZE))) tgg_cli_info;

// BW连接信息
typedef struct st_bw_info {
    // int fd;            // BW连接的fd
    int status;        // 连接状态
    int cmd;            // 记录连接类型  bw/gatewayclient
    int idx;        // 暂时不用  和fd共同标识唯一一个连接(fd是可重用的)  bw通信不记录状态，只记录在不在线就行，丢了就丢了
    int load;        // 暂时不用 BW的负载情况，用于计算负载均衡 
    int authorized; // 连接确认
    int lastupdattime;  // 暂时不用
    unsigned short port;// 远端端口
    int ip;// 远端ip
    char ip_str[INET_ADDRSTRLEN];  // 暂时不用
    char workerkey[WOKER_KEY_LEN]; // TODO wokerkey  后续考虑用指针替换，因为长度不确定
    char secretkey[SECRET_KEY_LEN];// TODO 从php的代码中看，他应该是和整个网关绑定的，不是和连接绑定的，测试环境抓包看到目前是空字符串
} tgg_bw_info;

// TODO list 存储BW的fd


// master收到数据后传给process处理，入队列时填充
// 对内和对外共用的收包数据结构体
typedef struct st_read_data {
    int fd;            // socket fd
    int coreid;        // coreid or prcid
    int bwfdx;          // 
    int fd_opt;
    int idx;
    unsigned int data_len;
    void* data;        // 携带的数据
} __attribute__((aligned(RTE_CACHE_LINE_SIZE))) tgg_read_data;

// list<fd>
typedef struct st_tgg_fd_list {
    int fdid;// 存储在hash表中的是fdid，在线程或进程之间传递时是fd
    int idx;
    struct st_tgg_fd_list* next;
} tgg_fd_list;

// process回传给master处理
typedef struct st_write_data {
    tgg_fd_list* lst_fd;            // socket fd(可能存在同时发多个fd)
    int fd_opt;        // 对fd的操作类型(写/关闭)
    int idx;
    unsigned int data_len;
    void* data;        // 携带的数据
} __attribute__((aligned(RTE_CACHE_LINE_SIZE))) tgg_write_data;

// bw数据处理传输结构
typedef struct st_read_data tgg_bw_data;

typedef struct st_en_queue_stats {
    int malloc_st;
    int malloc_data;
    int enqueue;
} tgg_en_queue_stats;
// 统计收发数据
typedef struct st_stats {
    int recv;            // 接收次数
    tgg_en_queue_stats en_read_stats;    // 入读队列次数
    int dequeue_read;    // 出读队列次数
    tgg_en_queue_stats en_write_stats;    // 入写队列次数
    int dequeue_write;    // 出写队列次数
    int send;            //  发送数
} tgg_stats;

// 进程信息
typedef struct st_pid_data {
    pid_t pid;        // 进程id                                    父进程写入
    uint64_t heard_beat;    // 心跳   防止进程无响应，队列无人消费            父进程写入，子进程通过信号通知并重置计数
    int idx;        // 索引   进程索引，标记进程能使用的队列        子进程写入和使用
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



// bw和gw通信协议头，固定长度部分 28字节
typedef struct __attribute__((__packed__)) st_bwprotocal {
    unsigned int pack_len; //数据包总长度，包含包头
    unsigned char cmd;// 命令
    unsigned int local_ip;// 网关对内的ip
    unsigned short local_port;// 网关对内的port
    unsigned int client_ip;// 客户端的ip
    unsigned short client_port;// 客户端 port
    unsigned int connection_id;// 客户端 cid中的idx
    unsigned char flag;// 发送数据格式是否要转换
    unsigned short gateway_port;// 网关对外的port
    unsigned int ext_len;
    char data[0];                   /// 柔性数组，不占长度，只作为标识数据部分的起始位置
} tgg_bw_protocal;
// 抓包显示
// 0000003f
// 05
// 00000000
// 0000
// 00000000
// 0000
// 00000367
// 01
// 0000
// 00000000
// fffe000000000023000000000000000100010101ab564ace4d51b232d051029226b500

#endif // _TGG_STRUCT_H_
#ifndef _TGG_STRUCT_H_
#define _TGG_STRUCT_H_
#include <rte_build_config.h>
#include <netinet/in.h>
#include <rte_rwlock.h>

// #define CACHE_LINE_SIZE 64
#define MAX_CPU_COUNT 128
#define SECRET_KEY_LEN 32
#define WOKER_KEY_LEN 64

#define MAX_LCORE_COUNT 32    // 最大允许的 lcore个数 TODO 可以优化，需要改相关逻辑以支持更多核
                              // 目前最大吃吃16个gw收包进程和16个cli处理线程

#define TGG_CID_LEN 24  // 最优性能：按8字节对齐
#define TGG_UID_LEN 24
#define TGG_GID_LEN 24
#define TGG_BWWKKEY_LEN 64 // 标识唯一的bw的字符串长度,格式 ip(16进制的int):worker_key

#define TGG_IPPORT_LEN 12

#define TGG_FD_CLOSING -1
#define TGG_FD_CLOSED -2
#define TGG_FD_NOTEXIST -3

#define COMMON_PACKET_LEN 1024
#define MAX_PACKET_LEN 8192  // 目前已知的情况：需要在ws的GET请求中加入一些其他信息，再传给服务端，所以这里要比ws缓存数据大一些

#define BUFFER_PACKET_LEN 4096 // ws默认缓存是4k，超过4k的连接  10w个连接就是400M，
#define BUFFER_PACKET_MASK 4095 // 掩码，使取余计算性能更高（对4096取余）
#define MAX_WSDATA_LEN 10*1024*1024   // websocket最多缓存10M的数据  暂时不器用，后续如果真的有超过4096的数据包

#define ENQUEUE_TRY_TIMES 10000 // 入队列不能失败，又要防止死循环，这个数字设置足够大

// fdid:fd << 8 & coreid
// 从fdid中取出coreid和fd   // 确保不同进程中fd的唯一性
// 直接取出来fd，fd后面的coreid和cid都右移掉
#define GET_FD_FDID_MASK(X) (X >> 8)
// coreid直接从cid中取
#define GET_COREID_FDID_MASK(X) (X & 0xff)

// fdidcid: (fd << 8 & coreid) << 32 & cid
// 从fdidcid中取对应的数据   主要作为gid,uid,cid hash的value，确保连接的唯一性
// 直接取出来fdid，fd后面的coreid和cid都右移掉
#define GET_FDID_FDIDCID_MASK(X) (X >> 32)

// 直接取出来fd，fd后面的coreid和cid都右移掉
#define GET_FD_FDCID_MASK(X) (X >> 40)

// cid: idx << 8 & coreid
// cid中包含core_id
#define GET_CID_FDCID_MASK(X) (X & 0xffffffff)
#define GET_IDX_FDCID_MASK(X) ((X & 0xffffffff) >> 8)
// coreid直接从cid中取
#define GET_COREID_FDCID_MASK(X) (X & 0xff)

// 从cid中取idx
#define GET_IDX_CID_MASK(X) (X >> 8)


// #define GET_FDIDCID_MASK(core_id, fd, cid) ((((fd << 8) | core_id) << 32) | cid)


// 应用层协议类型
enum L4_TYPE
{
    L4_TYPE_HTTP,
    L4_TYPE_WEBSOCK    
};

enum AUTH_TYPE
{
    AUTH_TYPE_UNKNOWN = 0,
    AUTH_TYPE_CLIENTCONNECT,
    AUTH_TYPE_HANDLESHAKED
};

// 需要对fd进行的操作类型
enum FD_OPT
{
    FD_NEW = 1,
    FD_HANDLESHAKE = 2,
    FD_READ = 4,
    FD_WRITE = 8,
    FD_CLOSE = 16
};

// fd的状态
enum FD_STATUS
{
    FD_STATUS_READYFORCONNECT = 0,
    FD_STATUS_NEWSESSION = 1,
    FD_STATUS_CONNECTED = 2,    // 有这个状态才能发包
    FD_STATUS_BIND = 4,    // 有这个状态才能发包
    FD_STATUS_KEEP = 8,
    FD_STATUS_CLOSING = 16,  // gwrcv已发送close包给gwcliprc (只发送一次,发送过就不再发送)
    FD_STATUS_CLOSED = 32,    // 这个状态下或者为0才能接收新的连接
    FD_STATUS_DISCONNECTED = 64,  // 连接已断开
};

// websocket的缓存结构，新接入一个客户端连接时会创建
typedef struct st_ws_data {
    int read_pos;
    int write_pos;    // 偏移量，
                // 1、方便取数据的时候通过偏移量直接取到有效数据部分
                // 2、这里的data存放的是fd读取的数据，为减少拷贝，不能改变data的位置，所以加一个pos
    // int capacity;
    void* data;
} tgg_ws_data;

typedef struct st_send_data {
    void* data;
    struct st_send_data* tail;
    struct st_send_data* next;
} tgg_send_data;

// 客户端需要保留的信息  gwrcv维护和使用
// TODO:是否要考虑断线重连之后上一个连接的数据包会发送到新的连接中来的问题
typedef struct st_cli_info {
    int status;        // 连接是否已关闭
    int idx;        // 和fd一起标识唯一连接，(fd可能被重用了,但是处理方仍不知情)
                    // -1 标识关闭中，后续的数据包不再处理，0标识关闭完成并准备就绪
    int authorized; // 连接确认
    tgg_ws_data ws_data;    // 缓存websocket的数据，用于处理分包的情况下
    char ip_str[INET_ADDRSTRLEN];  // ws握手时需要打包发送给bw
    int ip;
    unsigned short port;
    int bwfdx;        // 绑定的bw
    void* thread;
    void* wdata;      // 发送给客户端返回eagain时，需要缓存起来
    tgg_send_data* send_datalist;
} __attribute__((aligned(RTE_CACHE_LINE_SIZE))) tgg_cli_info;

// 客户单信息中需要gwbwprc维护和使用的部分
typedef struct st_cli_bw_info {
    int cid;    // client id                             process 填充
    char uid[TGG_UID_LEN];    // user id                             process 填充
    char reserved[128];    // reserved    
} __attribute__((aligned(RTE_CACHE_LINE_SIZE))) tgg_cli_bw_info;

// BW连接信息
typedef struct st_bw_info {
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
    int idx;            // idx是全局唯一的，fd在不同的进程中可能相同
    unsigned int data_len;
    void* data;        // 携带的数据
    unsigned int peer_ip; // 远端ip
    unsigned short peer_port;
    // unsigned int cid;
} __attribute__((aligned(RTE_CACHE_LINE_SIZE))) tgg_read_data;

// list<fd>  hash<gid, list<fd>> 这些一个gid/uid有多个fd的hash表的value
typedef struct st_tgg_fd_list {
    int64_t fdidcid;// 存储在hash表中的是fdidcid，在线程或进程之间传递时是fd
    struct st_tgg_fd_list* next;
} tgg_fd_list;


typedef struct st_tgg_fdidcid_list {
    // int64_t fdidcid;// 存储在hash表中的是fdidcid，在线程或进程之间传递时是fd
    // int idx;
    rte_rwlock_t lock;// hash 表 value为list时，操作时需要锁
    struct st_tgg_fd_list* list;
} tgg_fd_hash_value;


// list<fd,idx>  下行数据同一份数据发送给多个客户端时使用
typedef struct st_tgg_fd_idx_list {
    int fdid;// 存储在hash表中的是fdid，在线程或进程之间传递时是fd
    int idx;
    struct st_tgg_fd_idx_list* next;
} __attribute__((aligned(RTE_CACHE_LINE_SIZE))) tgg_fd_id_list;


// 下行发送给客户端，即gwbwprc回传给gwrcv的数据结构
typedef struct st_write_data {
    tgg_fd_id_list* lst_fd;            // socket fd(可能存在同时发多个fd)
    int fd_opt;        // 对fd的操作类型(写/关闭)
    int idx;
    unsigned int data_len;
    void* data;        // 携带的数据
    int ref;
} __attribute__((aligned(RTE_CACHE_LINE_SIZE))) tgg_write_data;

// 上行透传发送给 gwcliprc 数据结构
typedef struct st_read_data tgg_trans_data;

// gwcliprc传给gwbwprc的数据结构
typedef struct st_read_data tgg_bw_data;

// gwbwprc需要gwcliprc处理的消息，是以rte_queue的方式传递的,目前是传递bw的fd和进程号，方便gwcliprc做负载均衡
// 处理的命令
enum BWFDX_CMD {
    BWFDX_CMD_ADD = 0,
    BWFDX_CMD_DELETE,
    BWFDX_CMD_UPDATEALL,
    BWFDX_CMD_CLEAN_BWHASH,
    BWFDX_CMD_PRINTWORKERS,
    BWFDX_CMD_PRINTWORKERCOUNT
};
// gwbwprc发送给gwcli命令的数据结构
typedef struct st_bwfdx_data {
    int bwfdx;
    int cmd;
} tgg_bwfdx_data;


enum MASTER_EXC_CMD {
    CMD_IP_FILTER_RELOAD = 0,
    CMD_PRINT_DATA_STATS,
};

// sendary发送给master的数据结构,暂时只用到了一个ip过滤从文件中reload的功能
typedef struct st_send_master_data {
    int cmd;
} tgg_send_master_data;

/// 统计入队列数据结构                   ------ 未实际使用
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


// 进程监控 gwbwprc的进程
// 进程信息
typedef struct st_pid_data {
    pid_t pid;        // 进程id                                    父进程写入
    uint64_t heart_beat;    // 心跳   防止进程无响应，队列无人消费            父进程写入，子进程通过信号通知并重置计数
    int idx;        // 索引   进程索引，标记进程能使用的队列        子进程写入和使用
} pid_data;


// gid hash data
typedef tgg_fd_hash_value tgg_gid_data;

// uid hash data
typedef tgg_fd_hash_value tgg_uid_data;


typedef struct st_list_iddata {
    char data[TGG_GID_LEN];
    struct st_list_iddata* next;
} tgg_list_id;

typedef struct st_cidgid_value {
    rte_rwlock_t lock;
    tgg_list_id* list;
} tgg_fd_hash_svalue;

typedef tgg_fd_hash_svalue tgg_gid_list;

// cid hash value
typedef struct st_tgg_cid_data {
    int fd;
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
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/fcntl.h>
#include <rte_errno.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <rte_atomic.h>
#include <rte_hash.h>
#include <rte_hash_crc.h>

#include "mt_api.h"
#include "dpdk_init.h"
#include "tgg_comm/tgg_struct.h"
#include "tgg_comm/tgg_lock.h"
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/tgg_conf.h"

const char* g_gateway_ip_str = "192.168.40.129";
ushort g_gateway_port = 80;
uint32_t g_gate_ip = 0;
int g_bwwkkey_len = 64; // ip最多为8个F，woker_key待定

// static const char* s_init_flag = "/run/lock/tgg_init";

// TODO 多个lcore的情况下，必须要保证一个连接必须在一个lcore中读写(保证读写不异常)，也必须在一个process中处理(保证处理顺序)
//		要分多个memzone存放，不同的lcore 不同的连接可能是相同的fd
// 		基于此，在ring中存储的结构需要增加标识入读队列的进程，以方便process在入写队列的时候做区分
// 解决方案，每个数据包增加唯一链接标识 fdid = (fd << 8) & (lcore_id & 0xf)
//     原因 lcore_id最大为cpu核数，左移8位即一个字节做多能标识256核，足够了
int g_core_id;// 记录当前core_id

/// 连接管理的fd数组
uint32_t g_fd_limit = 10*10000; // 单个进程10W 个fd
static uint32_t s_zone_size = g_fd_limit*sizeof(tgg_cli_info);  // 单个进程存储最多10w个fd
struct rte_memzone* g_fd_zones[MAX_LCORE_COUNT] = {NULL};
const char* fd_zone_name_prev = "tgg_fd_zone";

/// bw连接状态记录的fd数组
uint32_t g_bwfdx_limit = 10*10000; // 单个进程1W 个fd
static uint32_t s_bwzone_size = g_bwfdx_limit*sizeof(int);  // 单个进程存储最多10w个fd
struct rte_memzone* g_bwfdx_zones[MAX_LCORE_COUNT] = {NULL};
const char* bwfdx_zone_name_prev = "tgg_bwfd_zone";

/// 进程锁
struct rte_memzone* g_lock_zone = NULL;
const char* s_lock_zone_name = "tgg_lock_zone";

/// 五组队列
// 队列名
const char* s_read_ring_name = "tgg_read_ring";
const char* s_trans_ring_name = "tgg_trans_ring";
const char* write_ring_name_prev = "tgg_write_ring";
const char* cliprc_ring_name_prev = "tgg_cliprc_ring";
const char* bwrcv_ring_name_prev = "tgg_bwrcv_ring";
const char* bwsnd_ring_name_prev = "tgg_bwsnd_ring";
// 队列长度
static uint32_t s_ring_size = 1024*8;  // 缓冲队列的长度，得是2的幂
// 队列对象
struct rte_ring* g_ring_read = NULL;// lcore cli上行  暂时不用了
struct rte_ring* g_ring_bwrcvs[MAX_LCORE_COUNT] = {NULL};// BW上行  暂时不用

// 当前实际使用的队列
struct rte_ring* g_ring_cliprcs[MAX_LCORE_COUNT] = {NULL};// 客户端上行
struct rte_ring* g_ring_writes[MAX_LCORE_COUNT] = {NULL};// 客户端下行
struct rte_ring* g_ring_trans = NULL;// 上行透传
struct rte_ring* g_ring_bwsnds[MAX_LCORE_COUNT] = {NULL};// BW下行



/// 三个内存池
// 内存池名称
const char* s_pool_read_name = "tgg_pool_read_name";// 客户端上行 和 上行prc共用
const char* s_pool_write_name = "tgg_pool_write_name";// 客户端下行
const char* s_pool_bwrcv_name = "tgg_pool_bwrcv_name";// 客户端上行透传 和 bw上行共用
// 内存池大小
static uint32_t s_mempool_size = 10000;
// 每个内存池单个内存块儿的大小
static uint32_t s_mempool_read_cache = sizeof(struct st_read_data);// 单个缓存的大小待定
static uint32_t s_mempool_write_cache = sizeof(struct st_write_data);// 单个缓存的大小待定
static uint32_t s_mempool_bwrcv_cache = sizeof(tgg_bw_data);// 单个缓存的大小待定
// 内存池对下你给
struct rte_mempool* g_mempool_read = NULL;
struct rte_mempool* g_mempool_write = NULL;
struct rte_mempool* g_mempool_bwrcv = NULL;


/// 调用rte_malloc使用的名称
const char* g_rte_malloc_type = "tgg_dpdk_malloc";

/// 五个hash表
// 存储uid -> fd 的hash表 
// 在process入写队列时，方便通过uid直接找到fd
const char* s_gid_hash_name = "tgg_gid_hash";
const char* s_uid_hash_name = "tgg_uid_hash";
const char* s_cid_hash_name = "tgg_cid_hash";
const char* s_uidgid_hash_name = "tgg_uidgid_hash";
const char* s_idx_hash_name = "tgg_idx_hash";
const char* s_bwfdx_hash_name = "tgg_bwfdx_hash";
const char* s_bwwkkey_hash_name = "tgg_bwwkkey_hash";


struct rte_hash *g_gid_hash = NULL;
struct rte_hash *g_uid_hash = NULL;
struct rte_hash *g_cid_hash = NULL;
struct rte_hash *g_uidgid_hash = NULL;

struct rte_hash *g_idx_hash = NULL;  // 存放已使用的client idx，idx会在指定的数字内循环，直到找到一个可用的
									//  客户端的连接需要在不同的进程中保留状态码，而fd是可重用的
									/// 所以需要一个idx来代替fd作为唯一键，在判断状态的时候确定连接的唯一性
// bwserver持有
struct rte_hash *g_bwfdx_hash = NULL;  // 用于服务端连接的负载均衡，存放正在使用的bwfd, 确定客户端的数据要发送到哪个服务端
struct rte_hash *g_bwwkkey_hash = NULL;  // 存放正在使用的bw的worker key

// 创建全局唯一cid时使用的缓冲区，防止频繁申请和释放内存
char g_cid_str[21] = {0};  // 8位地址+4位端口+8位idx+1位结束符'\0'

static uint32_t convert_ip2int(const char* ip)
{
    struct in_addr ipaddr;
    if (inet_pton(AF_INET, ip, &ipaddr) != 1) {
        fprintf(stderr, "Invalid IP address format.\n");
        exit(-1);
    }
    if(big_endian()) {
    	return ntohl(ipaddr.s_addr);
    }
    return ipaddr.s_addr;
}
// 初始化cid的前缀  16进制的8位ip+4位port
static void init_cid_prefix(uint32_t ip, ushort port)
{
	char* ptr = (char*)g_cid_str;
	for (int j = 0; j < 6; j++) {
		if ( j < 4) {
			// ip
			sprintf(ptr, "%02X", (ip >> (24 - j * 8)) & 0xFF);
		} else {
			// 端口
			sprintf(ptr, "%02X", (port >> (8 - (j -4) * 8)) & 0xFF);
		}
		ptr += 2;
	}
}

static void init_cid()
{
	// ip和端口 是固定的，只需要初始化的时候赋值就可以了
	g_gate_ip = convert_ip2int(g_gateway_ip_str);
	init_cid_prefix(g_gate_ip, g_gateway_port);
	// 只要有一个进程初始化就可以了，这里选择primary进程做初始化
	if (rte_eal_process_type() == RTE_PROC_PRIMARY) {
		rte_atomic32_init(get_idx_lock());
	}
}
static void init_redis_flag()
{
	rte_atomic32_init(get_redis_init_lock());
}

static void set_redis_inited()
{
	rte_atomic32_inc(get_redis_init_lock());
}

static int get_redis_init_flag()
{
	return rte_atomic32_read(get_redis_init_lock());
}

static struct rte_memzone *
find_memzone(const char *name)
{
	unsigned int socket_id = rte_socket_id();
	char mz_name[RTE_MEMZONE_NAMESIZE];
	struct rte_memzone *memzone;

	snprintf(mz_name, RTE_MEMZONE_NAMESIZE, "%s_%u", name, socket_id);
	memzone = (struct rte_memzone *)rte_memzone_lookup(mz_name);
	if (!memzone) {
		RTE_LOG(ERR, USER1, "[%s]:[%d]memzone[%s] not found.\n", 
			 __func__, __LINE__, mz_name);
		return NULL;
	}
	return memzone;
}

static struct rte_memzone *
make_memzone(const char *name, size_t size)
{
	unsigned int socket_id = rte_socket_id();
	struct rte_memzone *memzone;
	char mz_name[RTE_MEMZONE_NAMESIZE];

	snprintf(mz_name, RTE_MEMZONE_NAMESIZE, "%s_%u", name, socket_id);
	memzone = (struct rte_memzone *)rte_memzone_lookup(mz_name);
	if (memzone != NULL && memzone->len != size) {
		memset(memzone->addr, 0, memzone->len);
		rte_memzone_free(memzone);
		memzone = NULL;
		RTE_LOG(ERR, USER1, "[%s][%d]memzone[%s] found, but len[%lu] not match[%lu]\n", 
			__func__, __LINE__, mz_name, memzone->len, size);
	}
	if (memzone == NULL) {
		memzone = (struct rte_memzone *)rte_memzone_reserve_aligned(mz_name, size, socket_id,
				RTE_MEMZONE_2MB, RTE_CACHE_LINE_SIZE);
		if (memzone == NULL){
			rte_exit(EXIT_FAILURE,
				"[%s][%d] Can't allocate memory zone %s, error:%s.\n", __func__, __LINE__,
				mz_name, rte_strerror(rte_errno));
		}
	}
	memset(memzone->addr, 0, size);
	RTE_LOG(INFO, USER1, "New zone allocated: %s.\n",
		mz_name);
	return memzone;
}

static struct rte_mempool *
find_mempool(const char *name)
{
	unsigned int socket_id = rte_socket_id();
	char mp_name[RTE_MEMPOOL_NAMESIZE];
	struct rte_mempool *mempool;

	snprintf(mp_name, RTE_MEMPOOL_NAMESIZE, "%s_%u", name, socket_id);
	mempool = rte_mempool_lookup(mp_name);
	return mempool;
}

static struct rte_mempool *
make_mempool(const char *name, size_t units, size_t unit_size)
{
	unsigned int socket_id = rte_socket_id();
	char mp_name[RTE_MEMPOOL_NAMESIZE];
	struct rte_mempool *mempool;

	snprintf(mp_name, RTE_MEMPOOL_NAMESIZE, "%s_%u", name, socket_id);
	mempool = rte_mempool_lookup(mp_name);
	if (mempool != NULL) {
		rte_mempool_free(mempool);
		mempool = NULL;
	}
	if (mempool == NULL) {
		mempool = rte_mempool_create(mp_name,
			units,
			unit_size,
			0,
			0, NULL, NULL, NULL, NULL,
			rte_socket_id(), 0);
		if (mempool == NULL) {
			rte_exit(EXIT_FAILURE,
				"Can't allocate memory pool %s:%s:%d\n",
				mp_name, __func__, __LINE__);
		}
	}
	RTE_LOG(INFO, USER1, "New mempool allocated: %s.\n",
		mp_name);
	return mempool;
}

static struct rte_ring *
find_ring(const char *name)
{
	unsigned int socket_id = rte_socket_id();
	char ring_name[RTE_RING_NAMESIZE];
	struct rte_ring *ring;

	snprintf(ring_name, RTE_RING_NAMESIZE, "%s_%u", name, socket_id);
	ring = rte_ring_lookup(ring_name);
	return ring;
}

static struct rte_ring *
make_ring(const char *name, size_t units)
{
	unsigned int socket_id = rte_socket_id();
	char ring_name[RTE_RING_NAMESIZE];
	struct rte_ring *ring;

	snprintf(ring_name, RTE_RING_NAMESIZE, "%s_%u", name, socket_id);
	ring = rte_ring_lookup(ring_name);
	if (ring != NULL) {
		rte_ring_free(ring);
		ring = NULL;
	}
	if (ring == NULL) {
		ring = rte_ring_create(ring_name,
			units,
			rte_socket_id(),
			0);
		if (ring == NULL){
			rte_exit(EXIT_FAILURE,
				"Can't allocate ring %s:%s:%d\n",
				ring_name, __func__, __LINE__);
		}
	}
	RTE_LOG(INFO, USER1, "New ring allocated: %s\n",
		ring_name);
	return ring;
}


struct rte_hash* get_hash_byname(const char* hash_name)
{
	return rte_hash_find_existing(hash_name);
	 
}

struct rte_hash* init_hash(const char* hash_name, uint32_t ent_cnt, uint32_t key_len)
{
	struct rte_hash* _hash = get_hash_byname(hash_name);
	if (_hash) {
		rte_hash_free(_hash);
		_hash = NULL;
	}

	struct rte_hash_parameters hash_params = {
		.name = hash_name,
		.entries = ent_cnt,
		.key_len = key_len,
		.hash_func = rte_hash_crc,
		.hash_func_init_val = 0,
		.socket_id = (int)rte_socket_id(),
		.extra_flag = RTE_HASH_EXTRA_FLAGS_EXT_TABLE | 
						RTE_HASH_EXTRA_FLAGS_MULTI_WRITER_ADD | 
						RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF |
						RTE_HASH_EXTRA_FLAGS_NO_FREE_ON_DEL, // 无锁并发+扩展桶
	};

	_hash = rte_hash_create(&hash_params);
	if (!_hash) {
		rte_exit(EXIT_FAILURE,
			"Failed to create hash table[%s]:%s:%d\n",
			hash_name, __func__, __LINE__);
	}
	RTE_LOG(INFO, USER1, "New hash created: %s\n",
		hash_name);
	return _hash;
}

// TODO master初始化完成之后，要等待process初始化完成才能启动收发包的线程
//     如果master完全启动后，process才启动，可能会导致刚开始的一段时间丢包
static void init_flag_for_master()
{
    // 尝试创建用于进程锁的文件
    // int fd = open(s_init_flag, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    // if (fd == -1) {
    //     // 如果文件已存在，说明锁已被其他进程获取
    //     if (errno != EEXIST) {
    //     	rte_exit(EXIT_FAILURE,
	// 			"Failed to open flag file[%s]:%s:%d\n",
	// 			s_init_flag, __func__, __LINE__);
    //     }
    // }
    // close(fd);
    // // 等待process启动完成
	// while(access(s_init_flag, F_OK) == 0) {
	//     usleep(10);
	// }
  	// return;
}
// process初始化完成后删除标记文件，让master开始接收新的连接
void init_flag_for_process()
{
	// if(access(s_init_flag, F_OK) == 0) {
	// 	if (!unlink(s_init_flag)) {
	// 		return;
	// 	}
	// }
	if (get_redis_init_flag() <= 0) {
    	prc_exit(EXIT_FAILURE,
			"[%s][%d] redis data didn't synced yet.\n",
			__func__, __LINE__);
	}
}

// 从redis读取数据更新uidgid的hash表
static void init_uidgid_from_redis()
{
	pid_t pid;
    int status;

    pid = fork();
    if (pid == -1) {
        perror("fork error.");
        exit(-1);
    } else if (pid == 0) {
        // 子进程
        char * const argv[] = {(char*)("/data/code/f-stack/adapter/micro_thread/tgg/gwredis"), NULL};
        if (execvp("/data/code/f-stack/adapter/micro_thread/tgg/gwredis", argv) == -1) {
            perror("execvp error.");
            exit(-1);
        }
        exit(0);
    } else {
        // 父进程
    	if (waitpid(pid, &status, 0) == -1) {
    		rte_exit(EXIT_FAILURE,
    			"[%s][%d]Failed to init redis data, status:%d.\n", 
    			 __func__, __LINE__, status);
    	}
    	if(!status) {
        	set_redis_inited();
    		printf("init uidgid from redis done : %d\n", status);
    	} else {
    		rte_exit(EXIT_FAILURE,
    			"[%s][%d]Failed to init redis data, status:%d.\n", 
    			 __func__, __LINE__, status);
    	}
    }
}

void tgg_master_init()
{
	RTE_LOG(INFO, USER1, "Init dpdk master for tgg...\n");
	// 100W个FD  32M的空间
	g_lock_zone = make_memzone(s_lock_zone_name, sizeof(tgg_lock));
	init_cid();
	for (uint32_t i = 0; i < rte_lcore_count(); i++) {
		char zone_name[256] = {};
		sprintf(zone_name, "%s_%d", fd_zone_name_prev, i);
		g_fd_zones[i] = make_memzone(zone_name, s_zone_size);
		for (uint32_t j = 0; j < g_fd_limit; j++) {
			// 所有fd的初始状态设置为
			tgg_set_cli_idx(i, j, TGG_FD_CLOSED);
		}

		// cli 处理队列
		char cliprc_ring_name[256] = {};
		sprintf(cliprc_ring_name, "%s_%d", cliprc_ring_name_prev, i);
		g_ring_cliprcs[i] = make_ring(cliprc_ring_name, s_ring_size);

		// cli 发送队列
		char write_ring_name[256] = {};
		sprintf(write_ring_name, "%s_%d", write_ring_name_prev, i);
		g_ring_writes[i] = make_ring(write_ring_name, s_ring_size);

		// bw 接收
		char bwrcv_ring_name[256] = {};
		sprintf(bwrcv_ring_name, "%s_%d", bwrcv_ring_name_prev, i);
		g_ring_bwrcvs[i] = make_ring(bwrcv_ring_name, s_ring_size);

	}
	for (uint32_t i = 0; i < TggConfigure::instance->get_bwsvr_count() ; i++) {
		// bwfd zone
		char zone_name[256] = {};
		sprintf(zone_name, "%s_%d", bwfdx_zone_name_prev, i);
		g_bwfdx_zones[i] = make_memzone(zone_name, s_bwzone_size);
		for (uint32_t j = 0; j < g_bwfdx_limit; j++) {
			// 所有fd的初始状态设置为0
			tgg_set_bwfdx(i, j, 0);
		}
		// bw 发送
		char bwsnd_ring_name[256] = {};
		sprintf(bwsnd_ring_name, "%s_%d", bwsnd_ring_name_prev, i);
		g_ring_bwsnds[i] = make_ring(bwsnd_ring_name, s_ring_size);
	}
	// cli 接收队列
	g_ring_read = make_ring(s_read_ring_name, s_ring_size);
	// cli上行透传
	g_ring_trans = make_ring(s_trans_ring_name, s_ring_size);

	g_mempool_read = make_mempool(s_pool_read_name, s_mempool_size, s_mempool_read_cache);
	g_mempool_write = make_mempool(s_pool_write_name, s_mempool_size, s_mempool_write_cache);
	g_mempool_bwrcv = make_mempool(s_pool_bwrcv_name, s_mempool_size, s_mempool_bwrcv_cache);
	g_gid_hash = init_hash(s_gid_hash_name, g_fd_limit, TGG_GID_LEN);
	g_uid_hash = init_hash(s_uid_hash_name, g_fd_limit, TGG_UID_LEN);
	g_cid_hash = init_hash(s_cid_hash_name, g_fd_limit, TGG_CID_LEN);
	g_uidgid_hash = init_hash(s_uidgid_hash_name, g_fd_limit, TGG_UID_LEN);
	g_idx_hash = init_hash(s_idx_hash_name, g_fd_limit, sizeof(int));
	g_bwfdx_hash = init_hash(s_bwfdx_hash_name, g_fd_limit, sizeof(int));
	g_bwwkkey_hash = init_hash(s_bwwkkey_hash_name, g_fd_limit, g_bwwkkey_len);
	init_redis_flag();
	init_uidgid_from_redis();
	init_flag_for_master();
	RTE_LOG(INFO, USER1, "Init dpdk master for tgg done.\n");
}

void tgg_master_uninit()
{
	for (uint32_t i = 0; i < rte_lcore_count(); i++) {
		rte_memzone_free(g_fd_zones[i]);
		g_fd_zones[i] = NULL;

		rte_ring_free(g_ring_writes[i]);
		g_ring_writes[i] = NULL;
		rte_ring_free(g_ring_cliprcs[i]);
		g_ring_writes[i] = NULL;
		rte_ring_free(g_ring_bwrcvs[i]);
		g_ring_writes[i] = NULL;
	}
	for (uint32_t i = 0; i < TggConfigure::instance->get_bwsvr_count(); i++) {
		rte_memzone_free(g_bwfdx_zones[i]);
		g_bwfdx_zones[i] = NULL;
		// bw 发送
		rte_ring_free(g_ring_bwsnds[i]);
		g_ring_bwsnds[i] = NULL;
	}
	rte_memzone_free(g_lock_zone);
	g_lock_zone = NULL;
	rte_mempool_free(g_mempool_read);
	g_mempool_read = NULL;
	rte_ring_free(g_ring_read);
	g_ring_read = NULL;
	rte_ring_free(g_ring_trans);
	g_ring_trans = NULL;
	rte_hash_free(g_uid_hash);
	g_uid_hash = NULL;
	rte_hash_free(g_gid_hash);
	g_gid_hash = NULL;
	rte_hash_free(g_cid_hash);
	g_cid_hash = NULL;
	rte_hash_free(g_uidgid_hash);
	g_uidgid_hash = NULL;
	rte_hash_free(g_idx_hash);
	g_idx_hash = NULL;
	rte_hash_free(g_bwfdx_hash);
	g_bwfdx_hash = NULL;
	rte_hash_free(g_bwwkkey_hash);
	g_bwwkkey_hash = NULL;
}

void init_multi_for_secondary()
{
	for (uint32_t i = 0; i < rte_lcore_count(); i++) {
		// 初始化cli数组的zones
		char zone_name[256] = {};
		sprintf(zone_name, "%s_%d", fd_zone_name_prev, i);
		g_fd_zones[i] = find_memzone(zone_name);
		// 初始化发送队列ring
		char ring_name[256] = {};
		sprintf(ring_name, "%s_%d", write_ring_name_prev, i);
		g_ring_writes[i] = find_ring(ring_name);
	}
	for (uint32_t i = 0; i < TggConfigure::instance->get_bwsvr_count(); i++) {
		// 初始化bwfdx数组的zones
		char zone_name[256] = {};
		sprintf(zone_name, "%s_%d", bwfdx_zone_name_prev, i);
		g_bwfdx_zones[i] = find_memzone(zone_name);
		// bw 发送
		char bwsnd_ring_name[256] = {};
		sprintf(bwsnd_ring_name, "%s_%d", bwsnd_ring_name_prev, i);
		g_ring_bwsnds[i] = find_memzone(bwsnd_ring_name, s_ring_size);
	}
}

void tgg_secondary_init()
{
	RTE_LOG(INFO, USER1, "Init dpdk secodary for tgg...\n");
	// 100W个FD  32M的空间
	init_multi_for_secondary();
	g_lock_zone = find_memzone(s_lock_zone_name);
	g_ring_read = find_ring(s_read_ring_name);
	g_ring_trans = find_ring(s_trans_ring_name);
	g_mempool_read = find_mempool(s_pool_read_name);
	g_mempool_write = find_mempool(s_pool_write_name);
	g_mempool_bwrcv = find_mempool(s_pool_bwrcv_name);
	g_gid_hash = get_hash_byname(s_gid_hash_name);
	g_uid_hash = get_hash_byname(s_uid_hash_name);
	g_cid_hash = get_hash_byname(s_cid_hash_name);
	g_uidgid_hash = get_hash_byname(s_uidgid_hash_name);
	g_idx_hash = get_hash_byname(s_idx_hash_name);
	g_bwfdx_hash = get_hash_byname(s_bwfdx_hash_name);
	g_bwwkkey_hash = get_hash_byname(s_bwwkkey_hash_name);
	init_cid();
}

void tgg_secondary_uninit()
{
	// rte_mempool_free(g_mempool_read);
	// rte_ring_free(g_ring_read);
	NS_MICRO_THREAD::mt_uninit_frame();
	rte_eal_cleanup();
}

void tgg_cliprc_init()
{
	for (uint32_t i = 0; i < rte_lcore_count(); i++) {
		char ring_name[256] = {};
		sprintf(ring_name, "%s_%d", cliprc_ring_name_prev, i);
		g_ring_cliprcs[i] = find_ring(ring_name);
	}
	tgg_secondary_init();
}

void tgg_cliprc_uninit()
{
	rte_eal_cleanup();
}

// bw 消息处理进程处理dpdk操作相关数据结构初始化
void tgg_bwprc_init(int bwcount)
{
	for (int i = 0; i < bwcount; i++) {
		char ring_name[256] = {};
		sprintf(ring_name, "%s_%d", bwrcv_ring_name_prev, i);
		g_ring_bwrcvs[i] = find_ring(ring_name);
	}
	//tgg_secondary_init();
}

void tgg_bwprc_uninit(int bwcount)
{
	rte_eal_cleanup();
}


void prc_exit(int exit_code, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	rte_vlog(RTE_LOG_ERR, RTE_LOGTYPE_USER1, fmt, ap);
	va_end(ap);
	tgg_secondary_uninit();
	exit(exit_code);
}
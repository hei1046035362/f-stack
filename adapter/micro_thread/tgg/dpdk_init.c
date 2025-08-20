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
#include <rte_rcu_qsbr.h>

#include "mt_api.h"
#include "dpdk_init.h"
#include "tgg_comm/tgg_struct.h"
#include "tgg_comm/tgg_lock.h"
#include "tgg_comm/tgg_common.h"
#include "tgg_comm/tgg_conf.h"
#include "comm/log.hpp"
#include "comm/common.hpp"

const char* g_gateway_ip_str = "192.168.40.129";
ushort g_gateway_port = 80;
uint32_t g_gate_ip = 0;


// fdid(coreid+idx)中的fd和cid(coreid+idx)中的idx 能够取的极限值是8388607  因为存放的时候只用了三个字节，有一个字节要用来存放coreid，
//                               三个字节的第一位是符号位，所以能表达的最大值是 7f ff ff

// static const char* s_init_flag = "/run/lock/tgg_init";

// TODO 多个lcore的情况下，必须要保证一个连接必须在一个lcore中读写(保证读写不异常)，也必须在一个process中处理(保证处理顺序)
//		要分多个memzone存放，不同的lcore 不同的连接可能是相同的fd
// 		基于此，在ring中存储的结构需要增加标识入读队列的进程，以方便process在入写队列的时候做区分
// 解决方案，每个数据包增加唯一链接标识 fdid = (fd << 8) & (lcore_id & 0xf)
//     原因 lcore_id最大为cpu核数，左移8位即一个字节做多能标识256核，足够了
int g_core_id;// 记录当前core_id

/// 连接管理的fd数组
uint32_t g_fd_limit = 200000; // 默认单个进程20W 个fd   可配置(TODO 超过20万会导致primary进程出现[kqueue_proxy.cpp][517 ][KqueueCtlAdd]kqfd ref add failed, log)
static uint32_t s_zone_size = g_fd_limit*sizeof(tgg_cli_info);
struct rte_memzone* g_fd_zones[MAX_LCORE_COUNT] = {NULL};
const char* fd_zone_name_prev = "tgg_fd_zone";


/// 连接管理的fd数组 bw使用
static uint32_t s_zone_bw_size = g_fd_limit*sizeof(tgg_cli_bw_info);
struct rte_memzone* g_fd_bw_zones[MAX_LCORE_COUNT] = {NULL};
const char* fd_bw_zone_name_prev = "tgg_fd_bw_zone";


/// bw连接状态记录的fd数组
uint32_t g_bwfdx_limit = 5000; // 单个进程5K 个fd 可配置
static uint32_t s_bwzone_size = g_bwfdx_limit*sizeof(tgg_bw_info);
struct rte_memzone* g_bwfdx_zones[MAX_LCORE_COUNT] = {NULL};
const char* bwfdx_zone_name_prev = "tgg_bwfd_zone";

// bw进程数组
struct rte_memzone* g_bwprc_zone = NULL;
const char* bwprc_zone_name = "tgg_bwprc_zone";

// gw进程数组  包含gwrcv gwcliprc register
struct rte_memzone* g_gw_monitor_zone = NULL;
const char* gw_monitor_zone_name = "tgg_gw_monitor_zone";


/// 进程锁
struct rte_memzone* g_lock_zone = NULL;
const char* s_lock_zone_name = "tgg_lock_zone";

/// 五组队列
// 队列名
const char* s_trans_ring_name = "tgg_trans_ring";
const char* s_bwfdx_ring_name = "tgg_bwfdx_ring";
const char* write_ring_name_prev = "tgg_write_ring";
const char* bwrcv_ring_name_prev = "tgg_bwrcv_ring";
const char* s_master_ring_name = "tgg_master_ring";
// 队列长度
static uint32_t s_bwfdx_ring_size = 1024;  // bwfdx添加删除队列(gwbwprc->gwcliprc)，这个数据本身就不大，且处理很快
static uint32_t s_trans_ring_size = 1024*32;  // 缓冲队列的长度，得是2的幂
static uint32_t s_write_ring_size = 1024*64;  // 下行写队列长度，得是2的幂
static uint32_t s_bwrcv_ring_size = 1024*128;  // bwprc可能处理不过来，需要长一点，得是2的幂
static uint32_t s_master_ring_size = 64;  // secondary 发送给master执行的命令并不频繁，目前只有一个ip过滤列表的reload命令

// 当前实际使用的队列
struct rte_ring* g_ring_writes[MAX_LCORE_COUNT] = {NULL};// 客户端下行
struct rte_ring* g_ring_trans = NULL;// 上行透传
struct rte_ring* g_ring_bwfdx = NULL;// bwprc 接收到新的/删除旧的 fd时 要通知透传线程
struct rte_ring* g_ring_bwrcvs[MAX_LCORE_COUNT] = {NULL};// BW下行

struct rte_ring* g_ring_master = NULL;// bwprc接收重新加载ip过滤列表的命令 要通知master去执行


/// 三个内存池
// 内存池名称
const char* s_pool_trans_name = "tgg_pool_trans_name";// 客户端上行透传 				 单队列
const char* s_pool_write_name = "tgg_pool_write_name";// 客户端下行     				 多队列
const char* s_pool_bwrcv_name = "tgg_pool_bwrcv_name";// 客户端上行透传 和 bw上行共用  多队列

// 网络数据实际使用缓存
const char* s_pool_read_data_name = "tgg_pl_rdata";// 客户端上行 和 上行prc共用
const char* s_pool_trans_data_name = "tgg_pl_tdata";// 客户端下行
const char* s_pool_write_data_name = "tgg_pl_wdata";// 客户端下行
const char* s_pool_bwrcv_data_name = "tgg_pl_bwdata";// 客户端上行透传 和 bw上行共用
const char* s_pool_large_data_name = "tgg_pl_large_data";// 客户端上行透传 和 bw上行共用
const char* s_pool_ws_buffer_name = "tgg_pl_ws_buffer";// 缓存ws大包使用(处理分包粘包)
const char* s_pool_clifdlist_data_name = "tgg_pl_fdlst_data";// 下行发送fd列表的队列

// 内存池大小 TODO 大小根据队列长度设置
static uint32_t s_trans_mempool_size;// 尽量设置成2^n 单个队列预留 上行透传内存
static uint32_t s_write_mempool_size;// 尽量设置成2^n  下行发送内存
static uint32_t s_bwrcv_mempool_size;// 尽量设置成2^n  上行发送内存

static uint32_t s_trans_data_mempool_size;// 尽量设置成2^n  上行透传 数据 内存
static uint32_t s_write_data_mempool_size;// 尽量设置成2^n  下行发送 数据 内存
static uint32_t s_bwrcv_data_mempool_size;// 尽量设置成2^n  上行发送 数据 内存

// 超过正常大小的数据，大包的情况，需要申请稍大的空间   单个缓存大小为8192，有些网络框架中最大mtu会设置到8192
static uint32_t s_large_data_mempool_size;// 尽量设置成2^n  上行发送 数据 内存

static uint32_t s_ws_buffer_mempool_size;// 尽量设置成2^n  ws缓存 数据 内存

// 每个内存池单个内存块儿的大小
static uint32_t s_mempool_trans_cache = sizeof(tgg_trans_data);// 单个缓存的大小待定
static uint32_t s_mempool_write_cache = sizeof(struct st_write_data);// 单个缓存的大小待定
static uint32_t s_mempool_bwrcv_cache = sizeof(tgg_bw_data);// 单个缓存的大小待定
// 内存池
// 队列存储的数据结构
struct rte_mempool* g_mempool_trans = NULL;
struct rte_mempool* g_mempool_write[MAX_LCORE_COUNT] = {NULL};
struct rte_mempool* g_mempool_bwrcv[MAX_LCORE_COUNT] = {NULL};

// 分配队列中的数据结构的data字段
struct rte_mempool* g_mempool_trans_data = NULL;
struct rte_mempool* g_mempool_write_data = NULL;
struct rte_mempool* g_mempool_bwrcv_data = NULL;

static uint32_t s_clifdlist_mempool_size = 1024*1024;
struct rte_mempool* g_mempool_clifdlist_data = NULL;

struct rte_mempool* g_mempool_large_data = NULL;

struct rte_mempool* g_mempool_ws_buffer = NULL;

/// 五个hash表
// 存储uid -> fd 的hash表 
// 在process入写队列时，方便通过uid直接找到fd
const char* s_gid_hash_name = "tgg_gid_hash";
const char* s_uid_hash_name = "tgg_uid_hash";
const char* s_cid_hash_name = "tgg_cid_hash";
const char* s_cidgid_hash_name = "tgg_cidgid_hash";
const char* s_idx_hash_name = "tgg_idx_hash";
const char* s_bwfdx_hash_name = "tgg_bwfdx_hash";
const char* s_bwwkkey_hash_name = "tgg_bwwkkey_hash";


// 涉及到的所有hash结构
struct rte_hash *g_gid_hash = NULL;// map[gid] = list{fdx}    每个gid，存放属于这个gid的cid对应的fdx列表
struct rte_hash *g_uid_hash = NULL;// map[uid] = list{fdx}    每个uid，存放属于这个uid的cid对应的fdx列表
struct rte_hash *g_cid_hash = NULL;// map[cid] = {fdx}        通过cid查找fd的map
struct rte_hash *g_cidgid_hash = NULL;// map[cid] = list{gid}    每个cid，存放这个cid所属的gid列表

struct rte_hash *g_idx_hash[MAX_LCORE_COUNT] = {NULL};  // 存放已使用的client idx，idx会在指定的数字内循环，直到找到一个可用的
									//  客户端的连接需要在不同的进程中保留状态码，而fd是可重用的
									/// 所以需要一个idx来代替fd作为唯一键，在判断状态的时候确定连接的唯一性
// bwserver持有
struct rte_hash *g_bwfdx_hash = NULL;  // 用于服务端连接的负载均衡，存放正在使用的bwfd, 确定客户端的数据要发送到哪个服务端
struct rte_hash *g_bwwkkey_hash = NULL;  // 存放正在使用的bw的worker key

struct rte_rcu_qsbr *g_gid_rcu = NULL;
struct rte_rcu_qsbr *g_uid_rcu = NULL;
struct rte_rcu_qsbr *g_cid_rcu = NULL;
struct rte_rcu_qsbr *g_cidgid_rcu = NULL;
struct rte_rcu_qsbr *g_bwfdx_rcu = NULL;
struct rte_rcu_qsbr *g_bwwkkey_rcu = NULL;

struct rte_memzone* g_rcu_zone = NULL;
const char* s_rcu_zone_name = "tgg_rcu_zone";

// 初始化锁
static void init_locks()
{
	// 只要有一个进程初始化就可以了，这里选择primary进程做初始化
	if (rte_eal_process_type() == RTE_PROC_PRIMARY) {
		rte_rwlock_init(get_bwfdxhsh_lock());
		rte_rwlock_init(get_bwwkkeyhsh_lock());
		rte_rwlock_init(get_idxhsh_lock());
		rte_rwlock_init(get_gidfd_lock());
		rte_rwlock_init(get_uidfd_lock());
		rte_rwlock_init(get_cidfd_lock());
		rte_rwlock_init(get_cidgid_lock());
		rte_spinlock_init(get_cli_lock());
		rte_spinlock_init(get_bwfdx_lock());
		rte_spinlock_init(get_bwprc_lock());
		rte_rwlock_init(get_gw_monitor_lock());
		rte_atomic32_init(get_idx_lock());
	}
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
		LOG_ERROR("memzone[%s] not found.", mz_name);
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
		LOG_ERROR("memzone[%s] found, but len[%lu] not match[%lu].", mz_name, memzone->len, size);
	}
	if (memzone == NULL) {
		memzone = (struct rte_memzone *)rte_memzone_reserve_aligned(mz_name, size, socket_id,
				RTE_MEMZONE_2MB, RTE_CACHE_LINE_SIZE);
		if (memzone == NULL){
			LOG_ERROR("Can't allocate memory zone %s, error:%s.", mz_name, rte_strerror(rte_errno));
			rte_exit(EXIT_FAILURE,
				"[%s][%d] Can't allocate memory zone %s, error:%s.\n", __FILE__, __LINE__,
				mz_name, rte_strerror(rte_errno));
		}
	}
	memset(memzone->addr, 0, size);
	LOG_INFO("New zone allocated: %s.",	mz_name);
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
			unit_size + RTE_CACHE_LINE_SIZE,
			RTE_MEMPOOL_CACHE_MAX_SIZE,
			0, NULL, NULL, NULL, NULL,
			rte_socket_id(), 0);
		if (mempool == NULL) {
			LOG_ERROR("Can't allocate memory pool %s.", mp_name);
			rte_exit(EXIT_FAILURE,
				"Can't allocate memory pool %s:%s:%d\n",
				mp_name, __FILE__, __LINE__);
		}
	}
	LOG_INFO("New mempool allocated: %s.", mp_name);
	return mempool;
}

static struct rte_ring *
find_ring(const char *name)
{
	unsigned int socket_id = rte_socket_id();
	char ring_name[RTE_RING_NAMESIZE] = {0};
	struct rte_ring *ring;

	snprintf(ring_name, RTE_RING_NAMESIZE, "%s_%u", name, socket_id);
	ring = rte_ring_lookup(ring_name);
	return ring;
}

static struct rte_ring *
make_ring(const char *name, size_t units)
{
	unsigned int socket_id = rte_socket_id();
	char ring_name[RTE_RING_NAMESIZE] = {0};
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
			LOG_ERROR("Can't allocate ring %s.", ring_name);
			rte_exit(EXIT_FAILURE,
				"Can't allocate ring %s:%s:%d\n",
				ring_name, __FILE__, __LINE__);
		}
	}
	LOG_INFO("New ring allocated: %s.", ring_name);
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
		.entries = ent_cnt*4,
		.key_len = RTE_ALIGN(key_len, 8),
		.hash_func = rte_hash_crc,
		.hash_func_init_val = 0,
		.socket_id = (int)rte_socket_id(),
		.extra_flag = RTE_HASH_EXTRA_FLAGS_EXT_TABLE | 
						RTE_HASH_EXTRA_FLAGS_MULTI_WRITER_ADD | 
						RTE_HASH_EXTRA_FLAGS_TRANS_MEM_SUPPORT |
						RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF 
						// RTE_HASH_EXTRA_FLAGS_NO_FREE_ON_DEL, // 无锁并发+扩展桶
	};

	_hash = rte_hash_create(&hash_params);
	if (!_hash) {
		LOG_ERROR("Failed to create hash table[%s]", hash_name);
		rte_exit(EXIT_FAILURE,
			"Failed to create hash table[%s]:%s:%d\n",
			hash_name, __FILE__, __LINE__);
	}
	LOG_INFO("New hash created: %s", hash_name);
	return _hash;
}

struct rte_memzone* init_rcu_zone(const char* rcu_zone_name, int size)
{
    struct rte_memzone* rcu_zone = make_memzone(s_rcu_zone_name, size);
    return rcu_zone;
}

// static void init_rcu(struct rte_rcu_qsbr *rcu, struct rte_hash *hash)
// {
//     if(rte_rcu_qsbr_init(rcu, RTE_MAX_LCORE)) {
//         rte_exit(EXIT_FAILURE, "Failed to init RCU, init qsbr failed:%s.\n", rte_strerror(rte_errno));
//     }

//     struct rte_hash_rcu_config rcu_cfg = {
//         .v = rcu,                // 传递 RCU 对象
//         .mode = RTE_HASH_QSBR_MODE_SYNC  // 同步模式
//     };
//     if (rte_hash_rcu_qsbr_add(hash, &rcu_cfg) != 0) { 
//         rte_exit(EXIT_FAILURE, "Failed to add RCU to hash,error:%s\n", rte_strerror(rte_errno));
//     }
// }

void tgg_master_init()
{
	LOG_INFO("Init dpdk master for tgg...");
	int lcore_count = count_ones(TggConfigure::getInstance()->get_lcore_mask());
	if(lcore_count < 0) {
		return;
	}
	s_trans_mempool_size = s_trans_ring_size;// 单个队列预留 上行透传内存
	s_write_mempool_size = s_write_ring_size;// 有多个内存池  下行发送
	s_bwrcv_mempool_size = s_bwrcv_ring_size;// 内存池有多个  上行发送内存

	s_trans_data_mempool_size = s_trans_ring_size;// 尽量设置成2^n  上行透传 数据 内存
	s_write_data_mempool_size = s_write_ring_size * lcore_count;  // 只有一个内存池
	s_bwrcv_data_mempool_size = s_bwrcv_ring_size * TggConfigure::getInstance()->get_bwsvr_count();// 只有一个内存池

	s_large_data_mempool_size = 1024*32*lcore_count;// 大块数据，本来就很少，大多是连接创建的时候会有，但是这个是上下行三个队列都会用到

	s_ws_buffer_mempool_size = 1024*32*lcore_count;// ws缓存，基于单个进程并发而定，暂时限定为3W一个进程

	s_write_mempool_size = s_write_ring_size * TggConfigure::getInstance()->get_lcore_mask();

	g_fd_limit = TggConfigure::getInstance()->get_gwrcv_fd_limit();
	s_zone_size = g_fd_limit*sizeof(tgg_cli_info);
	s_zone_bw_size = g_fd_limit*sizeof(tgg_cli_bw_info);

	g_bwfdx_limit = TggConfigure::getInstance()->get_gwbwprc_fd_limit();
	s_bwzone_size = g_bwfdx_limit*sizeof(tgg_bw_info);
	// 100W个FD  32M的空间
	g_lock_zone = make_memzone(s_lock_zone_name, sizeof(tgg_lock));
	init_locks();
	for (int i = 0; i < lcore_count; i++) {
		// if(!((1 << i) & TggConfigure::getInstance()->get_lcore_mask())) {
		// 	continue;
		// }
		char zone_name[RTE_MEMZONE_NAMESIZE] = {0};
		sprintf(zone_name, "%s_%d", fd_zone_name_prev, i);
		g_fd_zones[i] = make_memzone(zone_name, s_zone_size);
		for (uint32_t j = 0; j < g_fd_limit; j++) {
			// 所有fd的初始状态设置为
			tgg_set_cli_idx(i, j, TGG_FD_CLOSED);
		}

		char zone_fd_name[RTE_MEMZONE_NAMESIZE] = {0};
		sprintf(zone_fd_name, "%s_%d", fd_bw_zone_name_prev, i);
		g_fd_bw_zones[i] = make_memzone(zone_fd_name, s_zone_bw_size);
		for (uint32_t j = 0; j < g_fd_limit; j++) {
			// 所有fd的初始状态设置为
			tgg_set_cli_cid(i, j, -1);
		}

		// cli 发送队列
		char write_ring_name[RTE_RING_NAMESIZE] = {0};
		sprintf(write_ring_name, "%s_%d", write_ring_name_prev, i);
		g_ring_writes[i] = make_ring(write_ring_name, s_write_ring_size);

		char write_pool_name[RTE_MEMPOOL_NAMESIZE] = {0};
		sprintf(write_pool_name, "%s_%d", s_pool_write_name, i);
		g_mempool_write[i] = make_mempool(write_pool_name, s_write_mempool_size, s_mempool_write_cache);
	}
	for (uint32_t i = 0; i < TggConfigure::getInstance()->get_bwsvr_count() ; i++) {
		// bwfd zone
		char zone_name[RTE_MEMZONE_NAMESIZE] = {0};
		sprintf(zone_name, "%s_%d", bwfdx_zone_name_prev, i);
		g_bwfdx_zones[i] = make_memzone(zone_name, s_bwzone_size);
		for (uint32_t j = 0; j < g_bwfdx_limit; j++) {
			// 所有fd的初始状态设置为0
			tgg_set_bwfdx_status(i, j, 0);
		}
		// bw 发送
		char bwrcv_ring_name[RTE_RING_NAMESIZE] = {0};
		sprintf(bwrcv_ring_name, "%s_%d", bwrcv_ring_name_prev, i);
		g_ring_bwrcvs[i] = make_ring(bwrcv_ring_name, s_write_ring_size);

		char bwrcv_pool_name[RTE_MEMPOOL_NAMESIZE] = {0};
		sprintf(bwrcv_pool_name, "%s_%d", s_pool_bwrcv_name, i);
		g_mempool_bwrcv[i] = make_mempool(bwrcv_pool_name, s_bwrcv_mempool_size, s_mempool_bwrcv_cache);
	}
	// cli上行透传
	g_ring_trans = make_ring(s_trans_ring_name, s_trans_ring_size);
	g_ring_bwfdx = make_ring(s_bwfdx_ring_name, s_bwfdx_ring_size);
	g_ring_master = make_ring(s_master_ring_name, s_master_ring_size);

	g_mempool_trans = make_mempool(s_pool_trans_name, s_trans_mempool_size, s_mempool_trans_cache);
	g_gid_hash = init_hash(s_gid_hash_name, g_fd_limit, TGG_GID_LEN);
	g_uid_hash = init_hash(s_uid_hash_name, g_fd_limit, TGG_UID_LEN);
	g_cid_hash = init_hash(s_cid_hash_name, g_fd_limit, sizeof(int64_t));
	g_cidgid_hash = init_hash(s_cidgid_hash_name, g_fd_limit, sizeof(int64_t));
	for (int i = 0; i < lcore_count; ++i)
	{// idx hash是每个lcore进程独享的，进程之间不共享
		// if(!((1 << i) & TggConfigure::getInstance()->get_lcore_mask())) {
		// 	continue;
		// }
		char idx_hash_name[128] = {0};
		sprintf(idx_hash_name, "%s_%d", s_idx_hash_name, i);
		g_idx_hash[i] = init_hash(idx_hash_name, g_fd_limit, sizeof(int64_t));
		// g_idx_rcu[i] = rte_rcu_qsbr_create(rte_socket_id());
		// rte_hash_rcu_qsbr_add(g_idx_hash[i], g_idx_rcu);
	}
	g_bwfdx_hash = init_hash(s_bwfdx_hash_name, g_fd_limit, sizeof(int64_t));
	g_bwwkkey_hash = init_hash(s_bwwkkey_hash_name, g_fd_limit, TGG_BWWKKEY_LEN);
	g_bwprc_zone = make_memzone(bwprc_zone_name, TggConfigure::getInstance()->get_bwsvr_count()*sizeof(pid_data));
	g_gw_monitor_zone = make_memzone(gw_monitor_zone_name, (lcore_count+2)*sizeof(pid_data));

	// 初始化rcu
	// size_t rcu_sz = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
	// g_rcu_zone = init_rcu_zone(s_rcu_zone_name, 6*rcu_sz);// 有6个hash表需要使用rcu
	// g_gid_rcu = (struct rte_rcu_qsbr *)((char*)(g_rcu_zone->addr));
	// init_rcu(g_gid_rcu, g_gid_hash);
	// g_uid_rcu = (struct rte_rcu_qsbr *)(((char*)(g_rcu_zone->addr)) + rcu_sz);
	// init_rcu(g_uid_rcu, g_uid_hash);
	// g_cid_rcu = (struct rte_rcu_qsbr *)(((char*)(g_rcu_zone->addr)) + 2*rcu_sz);
	// init_rcu(g_cid_rcu, g_cid_hash);
	// g_cidgid_rcu = (struct rte_rcu_qsbr *)(((char*)(g_rcu_zone->addr)) + 3*rcu_sz);
	// init_rcu(g_cidgid_rcu, g_cidgid_hash);
	// g_bwfdx_rcu = (struct rte_rcu_qsbr *)(((char*)(g_rcu_zone->addr)) + 4*rcu_sz);
	// init_rcu(g_bwfdx_rcu, g_bwfdx_hash);
	// g_bwwkkey_rcu = (struct rte_rcu_qsbr *)(((char*)(g_rcu_zone->addr)) + 5*rcu_sz);
	// init_rcu(g_bwwkkey_rcu, g_bwwkkey_hash);

	g_mempool_trans_data = make_mempool(s_pool_trans_data_name, s_trans_data_mempool_size, COMMON_PACKET_LEN);
	g_mempool_write_data = make_mempool(s_pool_write_data_name, s_write_data_mempool_size, COMMON_PACKET_LEN);
	g_mempool_bwrcv_data = make_mempool(s_pool_bwrcv_data_name, s_bwrcv_data_mempool_size, COMMON_PACKET_LEN);
	g_mempool_large_data = make_mempool(s_pool_large_data_name, s_large_data_mempool_size, MAX_PACKET_LEN);
	g_mempool_clifdlist_data = make_mempool(s_pool_clifdlist_data_name, s_clifdlist_mempool_size, sizeof(tgg_fd_id_list));
	g_mempool_ws_buffer = make_mempool(s_pool_ws_buffer_name, s_ws_buffer_mempool_size, BUFFER_PACKET_LEN);

	LOG_INFO("Init dpdk master for tgg done.");
}

void tgg_master_uninit()
{
	int lcore_count = count_ones(TggConfigure::getInstance()->get_lcore_mask());
	for (int i = 0; i < lcore_count; i++) {
		// if(!((1 << i) & TggConfigure::getInstance()->get_lcore_mask())) {
		// 	continue;
		// }
		rte_memzone_free(g_fd_zones[i]);
		g_fd_zones[i] = NULL;

		rte_memzone_free(g_fd_bw_zones[i]);
		g_fd_bw_zones[i] = NULL;

		rte_ring_free(g_ring_writes[i]);
		g_ring_writes[i] = NULL;

		rte_mempool_free(g_mempool_write[i]);
		g_mempool_write[i] = NULL;
	}
	for (uint32_t i = 0; i < TggConfigure::getInstance()->get_bwsvr_count(); i++) {
		rte_memzone_free(g_bwfdx_zones[i]);
		g_bwfdx_zones[i] = NULL;
		// bw 发送
		rte_ring_free(g_ring_bwrcvs[i]);
		g_ring_bwrcvs[i] = NULL;
		rte_mempool_free(g_mempool_bwrcv[i]);
		g_mempool_bwrcv[i] = NULL;
	}
	rte_memzone_free(g_lock_zone);
	g_lock_zone = NULL;

	rte_mempool_free(g_mempool_trans);
	g_mempool_trans = NULL;

	rte_mempool_free(g_mempool_trans_data);
	g_mempool_trans_data = NULL;
	rte_mempool_free(g_mempool_write_data);
	g_mempool_write_data = NULL;
	rte_mempool_free(g_mempool_bwrcv_data);
	g_mempool_bwrcv_data = NULL;

	rte_mempool_free(g_mempool_clifdlist_data);
	g_mempool_clifdlist_data = NULL;

	rte_mempool_free(g_mempool_large_data);
	g_mempool_large_data = NULL;
	rte_mempool_free(g_mempool_ws_buffer);
	g_mempool_ws_buffer = NULL;

	rte_ring_free(g_ring_trans);
	g_ring_trans = NULL;
	rte_ring_free(g_ring_bwfdx);
	g_ring_bwfdx = NULL;

	rte_ring_free(g_ring_master);
	g_ring_master = NULL;

	rte_hash_free(g_uid_hash);
	g_uid_hash = NULL;
	rte_hash_free(g_gid_hash);
	g_gid_hash = NULL;
	rte_hash_free(g_cid_hash);
	g_cid_hash = NULL;
	rte_hash_free(g_cidgid_hash);
	g_cidgid_hash = NULL;
	for (int i = 0; i < lcore_count; i++) {
		// if(!((1 << i) & TggConfigure::getInstance()->get_lcore_mask())) {
		// 	continue;
		// }
		rte_hash_free(g_idx_hash[i]);
		g_idx_hash[i] = NULL;
	}
	rte_hash_free(g_bwfdx_hash);
	g_bwfdx_hash = NULL;

	rte_hash_free(g_bwwkkey_hash);
	g_bwwkkey_hash = NULL;

	rte_memzone_free(g_bwprc_zone);
	g_bwprc_zone = NULL;

	rte_memzone_free(g_gw_monitor_zone);
	g_gw_monitor_zone = NULL;

	rte_memzone_free(g_rcu_zone);
	g_rcu_zone = NULL;
}

void init_multi_for_secondary()
{
	g_fd_limit = TggConfigure::getInstance()->get_gwrcv_fd_limit();
	g_bwfdx_limit = TggConfigure::getInstance()->get_gwbwprc_fd_limit();
	int lcore_count = count_ones(TggConfigure::getInstance()->get_lcore_mask());
	for (int i = 0; i < lcore_count; i++) {
		// if(!((1 << i) & TggConfigure::getInstance()->get_lcore_mask())) {
		// 	continue;
		// }
		// 初始化cli数组的zones
		char zone_name[RTE_MEMZONE_NAMESIZE] = {0};
		sprintf(zone_name, "%s_%d", fd_zone_name_prev, i);
		g_fd_zones[i] = find_memzone(zone_name);

		char zone_name1[RTE_MEMZONE_NAMESIZE] = {0};
		sprintf(zone_name1, "%s_%d", fd_bw_zone_name_prev, i);
		g_fd_bw_zones[i] = find_memzone(zone_name1);

		// 初始化发送队列ring
		char ring_name[RTE_RING_NAMESIZE] = {0};
		sprintf(ring_name, "%s_%d", write_ring_name_prev, i);
		g_ring_writes[i] = find_ring(ring_name);

		char write_pool_name[RTE_MEMPOOL_NAMESIZE] = {0};
		sprintf(write_pool_name, "%s_%d", s_pool_write_name, i);
		g_mempool_write[i] = find_mempool(write_pool_name);

	}
	for (uint32_t i = 0; i < TggConfigure::getInstance()->get_bwsvr_count(); i++) {
		// 初始化bwfdx数组的zones
		char zone_name[RTE_MEMZONE_NAMESIZE] = {0};
		sprintf(zone_name, "%s_%d", bwfdx_zone_name_prev, i);
		g_bwfdx_zones[i] = find_memzone(zone_name);
		// bw 发送
		char bwrcv_ring_name[RTE_RING_NAMESIZE] = {0};
		sprintf(bwrcv_ring_name, "%s_%d", bwrcv_ring_name_prev, i);
		g_ring_bwrcvs[i] = find_ring(bwrcv_ring_name);

		char bwrcv_pool_name[RTE_MEMPOOL_NAMESIZE] = {0};
		sprintf(bwrcv_pool_name, "%s_%d", s_pool_bwrcv_name, i);
		g_mempool_bwrcv[i] = find_mempool(bwrcv_pool_name);
	}
}

void tgg_secondary_init()
{
	LOG_INFO("Init dpdk secodary for tgg...");
	// 100W个FD  32M的空间
	init_multi_for_secondary();
	g_lock_zone = find_memzone(s_lock_zone_name);
	g_ring_trans = find_ring(s_trans_ring_name);
	g_ring_bwfdx = find_ring(s_bwfdx_ring_name);
	g_ring_master = find_ring(s_master_ring_name);
	g_mempool_trans = find_mempool(s_pool_trans_name);
	g_mempool_trans_data = find_mempool(s_pool_trans_data_name);
	g_mempool_write_data = find_mempool(s_pool_write_data_name);
	g_mempool_bwrcv_data = find_mempool(s_pool_bwrcv_data_name);
	g_mempool_large_data = find_mempool(s_pool_large_data_name);
	g_mempool_clifdlist_data = find_mempool(s_pool_clifdlist_data_name);
	g_mempool_ws_buffer = find_mempool(s_pool_ws_buffer_name);
	g_gid_hash = get_hash_byname(s_gid_hash_name);
	g_uid_hash = get_hash_byname(s_uid_hash_name);
	g_cid_hash = get_hash_byname(s_cid_hash_name);
	g_cidgid_hash = get_hash_byname(s_cidgid_hash_name);
	// g_idx_hash = get_hash_byname(s_idx_hash_name);
	g_bwfdx_hash = get_hash_byname(s_bwfdx_hash_name);
	g_bwwkkey_hash = get_hash_byname(s_bwwkkey_hash_name);

	// 初始化rcu
	// size_t rcu_sz = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
	// g_rcu_zone = find_memzone(s_rcu_zone_name);
	// g_gid_rcu = (struct rte_rcu_qsbr *)((char*)(g_rcu_zone->addr));
	// g_uid_rcu = (struct rte_rcu_qsbr *)(((char*)(g_rcu_zone->addr)) + rcu_sz);
	// g_cid_rcu = (struct rte_rcu_qsbr *)(((char*)(g_rcu_zone->addr)) + 2*rcu_sz);
	// g_cidgid_rcu = (struct rte_rcu_qsbr *)(((char*)(g_rcu_zone->addr)) + 3*rcu_sz);
	// g_bwfdx_rcu = (struct rte_rcu_qsbr *)(((char*)(g_rcu_zone->addr)) + 4*rcu_sz);
	// g_bwwkkey_rcu = (struct rte_rcu_qsbr *)(((char*)(g_rcu_zone->addr)) + 5*rcu_sz);
	// 所有进程注册线程到 RCU
	// rte_rcu_qsbr_thread_register(g_gid_rcu, rte_lcore_id());
    // rte_rcu_qsbr_thread_online(g_gid_rcu, rte_lcore_id());
	// rte_rcu_qsbr_thread_register(g_uid_rcu, rte_lcore_id());
    // rte_rcu_qsbr_thread_online(g_uid_rcu, rte_lcore_id());
	// rte_rcu_qsbr_thread_register(g_cid_rcu, rte_lcore_id());
    // rte_rcu_qsbr_thread_online(g_cid_rcu, rte_lcore_id());
	// rte_rcu_qsbr_thread_register(g_cidgid_rcu, rte_lcore_id());
    // rte_rcu_qsbr_thread_online(g_cidgid_rcu, rte_lcore_id());
	// rte_rcu_qsbr_thread_register(g_bwfdx_rcu, rte_lcore_id());
    // rte_rcu_qsbr_thread_online(g_bwfdx_rcu, rte_lcore_id());
	// rte_rcu_qsbr_thread_register(g_bwwkkey_rcu, rte_lcore_id());
    // rte_rcu_qsbr_thread_online(g_bwwkkey_rcu, rte_lcore_id());

}

void tgg_secondary_uninit()
{
	rte_eal_cleanup();
}

void tgg_unregister_rcu()
{
	// rte_rcu_qsbr_thread_offline(g_gid_rcu, rte_lcore_id());
	// (void)rte_rcu_qsbr_thread_unregister(g_gid_rcu, rte_lcore_id());
	// rte_rcu_qsbr_thread_offline(g_uid_rcu, rte_lcore_id());
	// (void)rte_rcu_qsbr_thread_unregister(g_uid_rcu, rte_lcore_id());
	// rte_rcu_qsbr_thread_offline(g_cid_rcu, rte_lcore_id());
	// (void)rte_rcu_qsbr_thread_unregister(g_cid_rcu, rte_lcore_id());
	// rte_rcu_qsbr_thread_offline(g_cidgid_rcu, rte_lcore_id());
	// (void)rte_rcu_qsbr_thread_unregister(g_cidgid_rcu, rte_lcore_id());
	// rte_rcu_qsbr_thread_offline(g_bwfdx_rcu, rte_lcore_id());
	// (void)rte_rcu_qsbr_thread_unregister(g_bwfdx_rcu, rte_lcore_id());
	// rte_rcu_qsbr_thread_offline(g_bwwkkey_rcu, rte_lcore_id());
	// (void)rte_rcu_qsbr_thread_unregister(g_bwwkkey_rcu, rte_lcore_id());
}

void tgg_gwrcv_secondary_init()
{
	int lcore_count = count_ones(TggConfigure::getInstance()->get_lcore_mask());
	for (int i = 0; i < lcore_count; i++) {
		// if(!((1 << i) & TggConfigure::getInstance()->get_lcore_mask())) {
		// 	continue;
		// }
		char idx_hash_name[128] = {0};
		sprintf(idx_hash_name, "%s_%d", s_idx_hash_name, i);
		g_idx_hash[i] = get_hash_byname(idx_hash_name);
	}
	tgg_secondary_init();
	g_gw_monitor_zone = find_memzone(gw_monitor_zone_name);
}

void tgg_cliprc_init()
{
	tgg_secondary_init();
	g_gw_monitor_zone = find_memzone(gw_monitor_zone_name);
}

void tgg_cliprc_uninit()
{
	tgg_secondary_uninit();
}

// bw 消息处理进程处理dpdk操作相关数据结构初始化
void tgg_bwprc_init(int bwcount)
{
	tgg_secondary_init();
	g_bwprc_zone = find_memzone(bwprc_zone_name);
}

void tgg_bwprc_uninit(int bwcount)
{
	tgg_secondary_uninit();
}

// register
void tgg_register_init()
{
	tgg_secondary_init();
	g_bwprc_zone = find_memzone(bwprc_zone_name);// 监控bwserver进程组
	g_gw_monitor_zone = find_memzone(gw_monitor_zone_name);
}

void tgg_register_uninit()
{
	tgg_secondary_uninit();
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
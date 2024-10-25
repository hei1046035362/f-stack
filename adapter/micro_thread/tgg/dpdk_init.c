#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <rte_atomic.h>
#include <arpa/inet.h>
#include "dpdk_init.h"
#include "tgg_struct.h"

const char* g_gateway_ip_str = "192.168.40.129";
ushort g_gateway_port = 80;
uint32_t g_gate_ip = 0;

// TODO 多个lcore的情况下，必须要保证一个连接必须在一个lcore中读写(保证读写不异常)，也必须在一个process中处理(保证处理顺序)
//		要分多个memzone存放，不同的lcore 不同的连接可能是相同的fd
// 		基于此，在ring中存储的结构需要增加标识入读队列的进程，以方便process在入写队列的时候做区分
int g_fd_limit = 1000*1000;
static int s_zone_size = g_fd_limit*sizeof(tgg_cli_info);  // 存储最多100w个fd
const struct rte_memzone* g_fd_zone = NULL;
const char* s_fd_zone_name = "tgg_fd_zone";

const char* s_read_ring_name = "tgg_read_ring";
const char* s_write_ring_name = "tgg_write_ring";
static int s_ring_size = 1024*8;  // 缓冲队列的长度，得是2的幂
const struct rte_ring* g_ring_read = NULL;
const struct rte_ring* g_ring_write = NULL;

const char* s_pool_read_name = "tgg_pool_read_name";
const char* s_pool_read_name = "tgg_pool_write_name";
static int s_mempool_size = 10000;
static int s_mempool_read_cache = sizeof(struct st_read_data);// 单个缓存的大小待定
static int s_mempool_write_cache = sizeof(struct st_write_data);// 单个缓存的大小待定
const struct rte_mempool* g_mempool_read = NULL, g_mempool_write = NULL;

const char* g_rte_malloc_type = "tgg_dpdk_malloc";

// 存储uid -> fd 的hash表
// 在process入写队列时，方便通过uid直接找到fd
const char* s_gid_hash_name = "tgg_gid_hash";
const char* s_uid_hash_name = "tgg_uid_hash";
const char* s_cid_hash_name = "tgg_cid_hash";
static int s_id_hash_len = 20; // hash key 长度
const struct rte_hash *g_gid_hash = NULL;
const struct rte_hash *g_uid_hash = NULL;
const struct rte_hash *g_cid_hash = NULL;

rte_atomic32_t *shared_id_atomic_ptr = NULL;
const char[21] g_cid_str = {0};  // 8位地址+4位端口+8位idx+1位结束符'\0'

static uint32_t convert_ip2int(const char* ip)
{
    struct in_addr ipaddr;
    if (inet_pton(AF_INET, ip, &ipaddr) != 1) {
        fprintf(stderr, "Invalid IP address format.\n");
        exit(-1);
    }
    return ntohl(ipaddr.s_addr);
}
// 初始化cid的前缀  16进制的8位ip+4位port
static void init_cid_prefix(uint32_t ip, ushort port)
{
	const char* ptr = g_cid_str;
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

	// idx后续可能会多进程，使用共享内存+原子锁  保证idx不会重复
   shared_id_atomic_ptr = (rte_atomic32_t *)rte_malloc("shared_id_atomic", sizeof(rte_atomic32_t), 0);
   if (shared_id_atomic_ptr == NULL) {
       // 处理内存分配失败的情况
       rte_exit(EXIT_FAILURE, "Failed to allocate shared memory for atomic ID\n");
   }
   rte_atomic32_set(shared_id_atomic_ptr, 0);  // 初始化为0
}

static const struct rte_memzone *
find_memzone(const char *name, size_t)
{
	unsigned int socket_id = rte_socket_id();
	char mz_name[RTE_MEMZONE_NAMESIZE];
	const struct rte_memzone *memzone;

	snprintf(mz_name, RTE_MEMZONE_NAMESIZE, "%s_%u", name, socket_id);
	memzone = rte_memzone_lookup(mz_name);
	if (memzone != NULL && memzone->len != size) {
		RTE_LOG(ERR, USER1, "memzone[%s] found, but len[%d] not match[%d]::%s:%d", 
			mz_name, memzone->len, size, __func__, __LINE__);
		return NULL;
	}
	return memzone;
}

static const struct rte_memzone *
make_memzone(const char *name, size_t size)
{
	const struct rte_memzone *memzone;

	snprintf(mz_name, RTE_MEMZONE_NAMESIZE, "%s_%u", name, socket_id);
	memzone = rte_memzone_lookup(mz_name);
	if (memzone != NULL && memzone->len != size) {
		rte_memzone_free(memzone);
		memzone = NULL;
		RTE_LOG(ERR, USER1, "memzone[%s] found, but len[%d] not match[%d]::%s:%d", 
			mz_name, memzone->len, size, __func__, __LINE__);
	}
	if (memzone == NULL) {
		memzone = rte_memzone_reserve_aligned(mz_name, size, socket_id,
				RTE_MEMZONE_IOVA_CONTIG, RTE_CACHE_LINE_SIZE);
		if (memzone == NULL){
			rte_exit(EXIT_FAILURE,
				"Can't allocate memory zone %s:%s:%d\n",
				mz_name, __func__, __LINE__);
		}
	}
	RTE_LOG(INFO, USER1, "New zone allocated: %s",
		mz_name);
	return memzone;
}

static const struct rte_mempool *
find_mempool(const char *name)
{
	unsigned int socket_id = rte_socket_id();
	char mp_name[RTE_MEMPOOL_NAMESIZE];
	const struct rte_mempool *mempool;

	snprintf(mp_name, RTE_MEMPOOL_NAMESIZE, "%s_%u", name, socket_id);
	mempool = rte_mempool_lookup(mp_name);
	return mempool;
}

static const struct rte_mempool *
make_mempool(const char *name, size_t units, size_t unit_size)
{
	unsigned int socket_id = rte_socket_id();
	char mp_name[RTE_MEMPOOL_NAMESIZE];
	const struct rte_mempool *mempool;

	snprintf(mp_name, RTE_MEMPOOL_NAMESIZE, "%s_%u", name, socket_id);
	mempool = rte_mempool_lookup(mp_name);
	if (mempool != NULL) {
		rte_mempool_free(mempool);
		mempool = NULL;
	}
	if (mempool == NULL) {
		mempool = rte_pktmbuf_pool_create(name,
			units,
			unit_size, 0,
			0,
			rte_socket_id());
		if (mempool == NULL) {
			rte_exit(EXIT_FAILURE,
				"Can't allocate memory pool %s:%s:%d\n",
				mp_name, __func__, __LINE__);
		}
	}
	RTE_LOG(INFO, USER1, "New mempool allocated: %s",
		mp_name);
	return mempool;
}

static const struct rte_ring *
find_ring(const char *name)
{
	unsigned int socket_id = rte_socket_id();
	char ring_name[RTE_RING_NAMESIZE];
	const struct rte_ring *ring;

	snprintf(ring_name, RTE_MEMZONE_NAMESIZE, "%s_%u", name, socket_id);
	ring = rte_ring_lookup(ring_name);
	return ring;
}

static const struct rte_ring *
make_ring(const char *name, size_t units)
{
	unsigned int socket_id = rte_socket_id();
	char ring_name[RTE_RING_NAMESIZE];
	const struct rte_ring *ring;

	snprintf(ring_name, RTE_MEMZONE_NAMESIZE, "%s_%u", name, socket_id);
	ring = rte_ring_lookup(ring_name);
	if (ring != NULL) {
		rte_ring_free(ring);
		ring = NULL;
	}
	if (ring == NULL) {
		ring = rte_ring_create(name,
			units,
			rte_socket_id(),
			0);
		if (ring == NULL){
			rte_exit(EXIT_FAILURE,
				"Can't allocate ring %s:%s:%d\n",
				ring_name, __func__, __LINE__);
		}
	}
	RTE_LOG(INFO, USER1, "New ring allocated: %s",
		ring_name);
	return ring;
}


const struct rte_hash* get_hash_byname(const char* hash_name)
{
	return rte_hash_find_existing(hash_name);
	 
}

const struct rte_hash* init_hash(const char* hash_name, int ent_cnt, int key_len)
{
	const struct rte_hash* _hash = get_hash_byname(hash_name);
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
		.socket_id = rte_socket_id(),
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
	return _hash;
}

void* dpdk_rte_malloc(int size)
{
	void* pdata = rte_malloc("tgg_malloc", size, 0);
	if (!pdata)	{
		RTE_LOG(ERR, USER1, "malloc data failed.");
	}
	// TODO 这里需要把pdata管理起来，因dpdk的secondary进程出core而未释放时会导致大页内存泄漏
	// 		可以用链表管理起来，然后注册rte_service给master进程去管理，也可以放到定时任务管理
	return pdata;
}


void tgg_master_init()
{
	RTE_LOG(INFO, USER1, "Init dpdk master for tgg...");
	// 100W个FD  32M的空间
	g_fd_zone = make_memzone(s_fd_zone_name, s_zone_size);
	g_ring_read = make_ring(s_read_ring_name, s_ring_size);
	g_ring_write = make_ring(s_write_ring_name, s_ring_size);
	g_mempool_read = make_mempool(s_pool_read_name, s_mempool_size, s_mempool_read_cache);
	g_mempool_write = make_mempool(s_pool_write_name, s_mempool_size, s_mempool_write_cache);
	g_gid_hash = init_hash(s_gid_hash_name, g_fd_limit, s_id_hash_len);
	g_uid_hash = init_hash(s_uid_hash_name, g_fd_limit, s_id_hash_len);
	g_cid_hash = init_hash(s_cid_hash_name, g_fd_limit, s_id_hash_len);
	init_cid();
}

void tgg_master_uninit()
{
	rte_memzone_free(g_fd_zone);
	g_fd_zone = NULL;
	rte_mempool_free(g_mempool_read);
	g_mempool_read = NULL;
	rte_ring_free(g_ring_read);
	g_ring_read = NULL;
	rte_ring_free(g_ring_write);
	g_ring_write = NULL;
	rte_hash_free(g_uid_hash);
	g_uid_hash = NULL;
}

void tgg_secondary_init()
{
	RTE_LOG(INFO, USER1, "Init dpdk secodary for tgg...");
	// 100W个FD  32M的空间
	g_fd_zone = find_memzone(s_fd_zone_name, s_zone_size);
	g_ring_read = find_ring(s_read_ring_name);
	g_ring_write = find_ring(s_write_ring_name);
	g_mempool_read = find_mempool(s_pool_read_name);
	g_mempool_write = find_mempool(s_pool_write_name);
	g_uid_hash = get_hash_byname(s_uid_hash_name, g_fd_limit, s_uid_hash_len);
}
void tgg_secondary_uninit()
{
	// rte_memzone_free(g_fd_zone);
	// rte_mempool_free(g_mempool_read);
	// rte_ring_free(g_ring_read);
	// rte_ring_free(g_ring_write);
}
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

#include "dpdk_init.h"
#include "tgg_comm/tgg_struct.h"
#include "tgg_comm/tgg_lock.h"

const char* g_gateway_ip_str = "192.168.40.129";
ushort g_gateway_port = 80;
uint32_t g_gate_ip = 0;

static const char* s_init_flag = "/run/lock/tgg_init";

// TODO 多个lcore的情况下，必须要保证一个连接必须在一个lcore中读写(保证读写不异常)，也必须在一个process中处理(保证处理顺序)
//		要分多个memzone存放，不同的lcore 不同的连接可能是相同的fd
// 		基于此，在ring中存储的结构需要增加标识入读队列的进程，以方便process在入写队列的时候做区分
uint32_t g_fd_limit = 1000*1000; // 单台服务器100W 个
static uint32_t s_zone_size = g_fd_limit*sizeof(tgg_cli_info);  // 存储最多100w个fd
struct rte_memzone* g_fd_zone = NULL;
const char* s_fd_zone_name = "tgg_fd_zone";
struct rte_memzone* g_lock_zone = NULL;
const char* s_lock_zone_name = "tgg_lock_zone";

const char* s_read_ring_name = "tgg_read_ring";
const char* s_bwrcv_ring_name = "tgg_bwrcv_ring";
const char* s_write_ring_name = "tgg_write_ring";
static uint32_t s_ring_size = 1024*8;  // 缓冲队列的长度，得是2的幂
struct rte_ring* g_ring_read = NULL;
struct rte_ring* g_ring_write = NULL;
struct rte_ring* g_ring_bwrcv = NULL;

const char* s_pool_read_name = "tgg_pool_read_name";
const char* s_pool_write_name = "tgg_pool_write_name";
const char* s_pool_bwrcv_name = "tgg_pool_bwrcv_name";
static uint32_t s_mempool_size = 10000;
static uint32_t s_mempool_read_cache = sizeof(struct st_read_data);// 单个缓存的大小待定
static uint32_t s_mempool_write_cache = sizeof(struct st_write_data);// 单个缓存的大小待定
static uint32_t s_mempool_bwrcv_cache = sizeof(tgg_bw_data);// 单个缓存的大小待定
struct rte_mempool* g_mempool_read = NULL;
struct rte_mempool* g_mempool_write = NULL;
struct rte_mempool* g_mempool_bwrcv = NULL;

const char* g_rte_malloc_type = "tgg_dpdk_malloc";

// 存储uid -> fd 的hash表
// 在process入写队列时，方便通过uid直接找到fd
const char* s_gid_hash_name = "tgg_gid_hash";
const char* s_uid_hash_name = "tgg_uid_hash";
const char* s_cid_hash_name = "tgg_cid_hash";
const char* s_uidgid_hash_name = "tgg_uidgid_hash";
const char* s_idx_hash_name = "tgg_idx_hash";
static uint32_t s_id_hash_len = 20; // hash key 长度
struct rte_hash *g_gid_hash = NULL;
struct rte_hash *g_uid_hash = NULL;
struct rte_hash *g_cid_hash = NULL;
struct rte_hash *g_uidgid_hash = NULL;
struct rte_hash *g_idx_hash = NULL;  // 存放已使用的client idx


rte_atomic32_t *shared_id_atomic_ptr = NULL;
char g_cid_str[21] = {0};  // 8位地址+4位端口+8位idx+1位结束符'\0'

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

// master初始化完成之后，要等待process初始化完成才能启动收发包的线程
//     如果master完全启动后，process才启动，可能会导致刚开始的一段时间丢包
static void init_flag_for_master()
{
    // 尝试创建用于进程锁的文件
    int fd = open(s_init_flag, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        // 如果文件已存在，说明锁已被其他进程获取
        if (errno != EEXIST) {
        	rte_exit(EXIT_FAILURE,
				"Failed to open flag file[%s]:%s:%d\n",
				s_init_flag, __func__, __LINE__);
        }
    }
    close(fd);
    // 等待process启动完成
	while(access(s_init_flag, F_OK) == 0) {
	    usleep(10);
	}
  	return;
}
// process初始化完成后删除标记文件，让master开始接收新的连接
static void init_flag_for_process()
{
	if(access(s_init_flag, F_OK) == 0) {
		if (!unlink(s_init_flag)) {
			return;
		}
	}
    rte_exit(EXIT_FAILURE,
		"Failed to init flag file[%s]:%s:%d\n",
		s_init_flag, __func__, __LINE__);
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
    	printf("init uidgid from redis done : %d\n", status);
    }
}

void tgg_master_init()
{
	RTE_LOG(INFO, USER1, "Init dpdk master for tgg...\n");
	// 100W个FD  32M的空间
	g_fd_zone = make_memzone(s_fd_zone_name, s_zone_size);
	g_lock_zone = make_memzone(s_lock_zone_name, sizeof(tgg_lock));
	g_ring_read = make_ring(s_read_ring_name, s_ring_size);
	g_ring_write = make_ring(s_write_ring_name, s_ring_size);
	g_ring_bwrcv = make_ring(s_bwrcv_ring_name, s_ring_size);
	g_mempool_read = make_mempool(s_pool_read_name, s_mempool_size, s_mempool_read_cache);
	g_mempool_write = make_mempool(s_pool_write_name, s_mempool_size, s_mempool_write_cache);
	g_mempool_bwrcv = make_mempool(s_pool_bwrcv_name, s_mempool_size, s_mempool_bwrcv_cache);
	g_gid_hash = init_hash(s_gid_hash_name, g_fd_limit, s_id_hash_len);
	g_uid_hash = init_hash(s_uid_hash_name, g_fd_limit, s_id_hash_len);
	g_cid_hash = init_hash(s_cid_hash_name, g_fd_limit, s_id_hash_len);
	g_uidgid_hash = init_hash(s_uidgid_hash_name, g_fd_limit, s_id_hash_len);
	g_idx_hash = init_hash(s_idx_hash_name, g_fd_limit, sizeof(int));
	init_cid();
	init_uidgid_from_redis();
	init_flag_for_master();
	RTE_LOG(INFO, USER1, "Init dpdk master for tgg done.\n");
}

void tgg_master_uninit()
{
	rte_memzone_free(g_fd_zone);
	g_fd_zone = NULL;
	rte_memzone_free(g_lock_zone);
	g_lock_zone = NULL;
	rte_mempool_free(g_mempool_read);
	g_mempool_read = NULL;
	rte_ring_free(g_ring_read);
	g_ring_read = NULL;
	rte_ring_free(g_ring_write);
	g_ring_write = NULL;
	rte_ring_free(g_ring_bwrcv);
	g_ring_bwrcv = NULL;
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
}

void tgg_secondary_init()
{
	RTE_LOG(INFO, USER1, "Init dpdk secodary for tgg...\n");
	// 100W个FD  32M的空间
	g_fd_zone = find_memzone(s_fd_zone_name);
	g_lock_zone = find_memzone(s_lock_zone_name);
	g_ring_read = find_ring(s_read_ring_name);
	g_ring_write = find_ring(s_write_ring_name);
	g_ring_bwrcv = find_ring(s_bwrcv_ring_name);
	g_mempool_read = find_mempool(s_pool_read_name);
	g_mempool_write = find_mempool(s_pool_write_name);
	g_mempool_bwrcv = find_mempool(s_pool_bwrcv_name);
	g_gid_hash = get_hash_byname(s_gid_hash_name);
	g_uid_hash = get_hash_byname(s_uid_hash_name);
	g_cid_hash = get_hash_byname(s_cid_hash_name);
	g_uidgid_hash = get_hash_byname(s_uidgid_hash_name);
	g_idx_hash = get_hash_byname(s_idx_hash_name);
	init_cid();
	init_flag_for_process();
}
void tgg_secondary_uninit()
{
	// rte_memzone_free(g_fd_zone);
	// rte_mempool_free(g_mempool_read);
	// rte_ring_free(g_ring_read);
	// rte_ring_free(g_ring_write);
}
#include "tgg_common.h"
#include <rte_ring.h>
#include <rte_memzone.h>
#include <rte_mempool.h>
extern const struct rte_memzone* g_fd_zone;
extern int g_fd_limit;
extern const struct rte_ring* g_ring_read;
extern const struct rte_ring* g_ring_write;
extern const struct rte_mempool* g_mempool_read;

const tgg_stats g_tgg_stats = {0};


typedef void (*tgg_free_id_data)(void*);

tgg_cli_info* tgg_get_cli_data(int fd)
{
	if (fd < 0 || fd >= g_fd_limit)	{
		RTE_LOG(INFO, USER1, "given fd[%s] is invalid,[0,%d]",
			fd, g_fd_limit - 1);
		return NULL;
	}
	return &((tgg_cli_info*)g_fd_zone->addr)[fd];
}

int tgg_enqueue_read(tgg_read_data* data);
{
	return rte_ring_enqueue(g_ring_read, data)
}

int tgg_enqueue_write(tgg_write_data* data);
{
	return rte_ring_enqueue(g_ring_write, data)
}

int tgg_dequeue_read(tgg_read_data** data);
{
	if (rte_ring_empty(g_ring_read)) {
		return -ENOENT;
	}
	return rte_ring_dequeue(g_ring_read, (void**)data)
}

int tgg_dequeue_write(tgg_write_data** data);
{
	if (rte_ring_empty(g_ring_write)) {
		return -ENOENT;
	}
	return rte_ring_dequeue(g_ring_write, (void**)data)
}

int get_fd_by_uid(const char* uid)
{
	
}


static int tgg_hash_add_key(const rte_hash* hash, const char* key, const tgg_gid_data* data)
{
	if (strlen(key) != 20) {
		RTE_LOG(ERR, USER1, "[%s][%d]add key failed,check if key[%s] is correct.", __func__, __LINE__, key);
		return -EINVAL; 
	}
	if (!data) {
		RTE_LOG(ERR, USER1, "[%s][%d]add key[%s] failed, invalid data.", __func__, __LINE__, key);
		return -EINVAL; 
	}
	int ret = rte_hash_add_key_data(hash, key, data);
	if (ret < 0) {
		RTE_LOG(ERR, USER1, "[%s][%d]add key[%s] failed:%d.", __func__, __LINE__, key, ret);
		return ret;
	}
	return 0;
}

static void* tgg_hash_get_key(const rte_hash* hash, const char* key)
{
	int ret = rte_hash_lookup(hash, key);
	if (ret < 0) {
		RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed,hash key not exist:%d", __func__, __LINE__, key, ret);
		return NULL;
	}
	void* pdata = NULL;
	ret = rte_hash_lookup_data(hash, key, &pdata);
	if (ret < 0) {
		RTE_LOG(ERR, USER1, "[%s][%d]Get key[%s] data failed:%d", __func__, __LINE__, key, ret);
		return NULL;
	}
	return pdata;
}

static int tgg_hash_del_key(const rte_hash* hash, const char* key, tgg_free_id_data fp)
{
	tgg_gid_data* pdata = tgg_get_gid(key);
	if (!pdata)
		return -EINVAL;

	// 释放value的空间
	fp((void*)pdata);

	int ret = rte_hash_del_key(hash, key);
	if (ret > 0) {
		// 在并发情况下删除key之后，位置还在，需要删除位置信息，详情参考函数说明
		if (rte_hash_free_key_with_position(hash, ret) < 0) {
			RTE_LOG(ERR, USER1, "[%s][%d]Del key[%s] pos failed:%d", __func__, __LINE__, key, ret);
			return -EINVAL;
		}
	} else {
		RTE_LOG(ERR, USER1, "[%s][%d]Del key[%s] data failed:%d", __func__, __LINE__, key, ret);
		return -EINVAL;
	}
	return 0;
}
/// 增删查  gid
int tgg_add_gid(char* gid, tgg_gid_data* data)
{
	return tgg_hash_add_key(g_gid_hash, gid, data);
}
tgg_gid_data* tgg_get_gid(char* gid)
{
	return tgg_hash_get_key(g_gid_hash, gid);
}


static void iter_del_fdlist(tgg_fd_list* iddata)
{
	if (!iddata) {
		return;
	}
	tgg_fd_list* iter = iddata;// 第一个节点不存数据，先删除数据节点
	while(iter->next) {
		tgg_fd_list* tmp = iter->next;
		iter->next = iter->next->next;
		rte_free(tmp);
		tmp = NULL;
	}
	// 删除第一个节点
	rte_free(iddata);
	iddata = NULL;
}


int tgg_del_gid(char* gid)
{
	return tgg_hash_del_key(g_gid_hash, gid, iter_del_fdlist);
}

/// 增删查  uid
int tgg_add_uid(char* uid, tgg_uid_data* data)
{
	return tgg_hash_add_key(g_uid_hash, uid, data);
}

int tgg_del_uid(char* uid)
{
	return tgg_hash_del_key(g_uid_hash, uid, iter_del_fdlist);
}

tgg_uid_data* tgg_get_uid(char* uid);
{
	return tgg_hash_get_key(g_uid_hash, uid);
}

/// 增删查  cid
int tgg_add_cid(char* cid, tgg_cid_data* data);
{
	return tgg_hash_add_key(g_cid_hash, cid, data);
}

static void free_ciddata(void* data)
{
	tgg_cid_data* pdata = (tgg_cid_data*)data;
	rte_free(pdata);
	pdata = NULL;
}

int tgg_del_cid(char* cid);
{
	return tgg_hash_del_key(g_cid_hash, gid, free_ciddata);
}

tgg_cid_data* tgg_get_cid(char* cid);
{
	return tgg_hash_get_key(g_cid_hash, gid);
}






#include <sys/prctl.h>
void init_core(const char* dumpfile)
{
	// 设置 core 文件的路径
    prctl(PR_SET_DUMPABLE, 1);  // 确保程序可以生成 core 文件
    char core_path[256];
    snprintf(core_path, sizeof(core_path), "%s.%d", dumpfile, getpid());
    prctl(PR_SET_DUMPABLE, core_path, 0, 0, 0);
}

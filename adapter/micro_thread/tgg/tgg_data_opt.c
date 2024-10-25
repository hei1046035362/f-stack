#include "tgg_common.h"
#include "string.h"

static list_gid* get_gids_by_uid(const char* uid)
{// TODO 从redis读uid对应的gid列表
	return NULL;
}

static void iter_del_fd(tgg_fd_list* iddata, intfd)
{
	tgg_fd_list* iter = iddata;
	// 第一个节点为空，且只能向后遍历，所以只能操作iter->next
	while(iter && iter->next) {
		if (iter->next->fd == fd) {
			tgg_fd_list* tmp = iter->next;
			rte_free(tmp);
			tmp = NULL;
			iter->next = iter->next->next;
		} else {
	    	iter = iter->next;
	    }
	}
}

// #define TGG_ADD_ID(t, fd, data) \
// 		tgg_add_##t##id(fd, data)
typedef int (*tgg_add_id)(const char* id, tgg_fd_list* iddata);
typedef tgg_fd_list* (*tgg_get_id)(const char* id);

static int new_session_for_idhash(int fd, const char* id, tgg_get_id tgg_getid_fun, tgg_add_id tgg_addid_fun)
{
	tgg_fd_list* newdata = (tgg_fd_list*)dpdk_rte_malloc(sizeof(tgg_fd_list));
	newdata->fd = fd;
	newdata->next = NULL;

	tgg_fd_list* udata = tgg_getid_fun(id);
	if (!udata)
	{// 新的group
		// 头结点不存数据
		tgg_fd_list* head = (tgg_fd_list*)dpdk_rte_malloc(sizeof(tgg_fd_list));
		head->fd = -1;
		head->next = newdata;
		if (tgg_addid_fun(id, head) < 0) {
			rte_free(newdata);
			rte_free(head);
			head = NULL;
			newdata = NULL;
			return -1;
		}
	} else {
		// 新的fd添加到list<fd>末尾
		tgg_fd_list* iter = udata;
		while(iter->next) {
		    iter = iter->next;
		}
		iter->next = newdata;
	}
	return 0;

}
static int del_session_for_gidhash(int fd, const char* uid)
{
	list_gid* lstgid = get_gids_by_uid(uid);
	while (!lstgid) {
		tgg_gid_data* gdata = tgg_get_gid(gid);
		if (!gdata || !gdata->next) {// 节点value为空，或者list中没有fd了，就把整个gid删掉
			int ret = tgg_del_gid(gid);
			if (ret < 0) {
				RTE_LOG(ERR, USER1, "[%s][%d]del gid[%s] failed: %d.", __func__, __LINE__, gid, ret);
				// return -1;
			}
		}
		iter_del_fd(gdata, fd);
		if (!gdata->next) {// list中没有fd了，就把整个gid删掉
			int ret = tgg_del_gid(gid);
			if (ret < 0) {
				RTE_LOG(ERR, USER1, "[%s][%d]del gid[%s] failed: %d.", __func__, __LINE__, gid, ret);
				// return -1;
			}
		}
		lstgid = lstgid->next;
	}
	return 0;
}

static int new_session_for_gidhash(int fd, const char* uid)
{
	// TODO 从redis去读gid列表  get_gids_by_uid 未实现
	list_gid* lstgid = get_gids_by_uid(uid);
	int ret = 0;
	while (!lstgid) {
		ret = new_session_for_idhash(fd, lstgid->gid, tgg_get_gid, tgg_add_gid);
		if (ret < 0) {
			RTE_LOG(ERR, USER1, "[%s][%d]add session for gid[%s] failed: %d.", __func__, __LINE__, gid, ret);
			return -1;
		}
		lstgid = lstgid->next;
	}
	return 0;
}

static int del_session_for_uidhash(int fd, const char* uid)
{
	tgg_uid_data* udata = tgg_get_uid(uid);
	if (!udata) {
		int ret = tgg_del_uid(uid);
		if (ret < 0) {
			RTE_LOG(ERR, USER1, "[%s][%d]del uid[%s] failed: %d.", __func__, __LINE__, uid, ret);
			return -1;
		}
		return 0;
	}
	iter_del_fd(udata, fd);
	return 0;
}

static int new_session_for_uidhash(int fd, const char* uid)
{
	return new_session_for_idhash(fd, uid, tgg_get_uid, tgg_add_uid);
}

static int del_session_for_cidhash(int fd, const char* cid)
{
	tgg_cid_data* cdata = tgg_get_cid(cid);
	if (cdata) {
		rte_free(cdata);
	}
	int ret = tgg_del_cid(cid);
	if (ret < 0) {
		RTE_LOG(ERR, USER1, "[%s][%d]del uid[%s] failed: %d.", __func__, __LINE__, uid, ret);
		return -1;
	}
	return 0;
}

static int new_session_for_cidhash(int fd, const char* cid)
{
	tgg_cid_data* cdata = tgg_get_cid(cid);
	if (cdata) {// 已存在就直接修改fd就可以
		// TODO 这个fd大概率是失效了，后续怎么处理？先观察是否存在这种情况，后续再处理
		RTE_LOG(WARNING, USER1, "cid[%s] already exist.", cid);
		cdata->fd = fd;
		return 0;
	}
	tgg_cid_data* dcid = (tgg_cid_data*)dpdk_rte_malloc(sizeof(tgg_cid_data));
	dcid->fd = fd;
	if (tgg_add_cid(cid, dcid) < 0) {
		// 添加失败，回收内存
		rte_free(dcid);
		dcid = NULL;
		RTE_LOG(ERR, USER1, "add cid[%s] failed.", cid);
		return -1;
	}
	return 0;
}

// 执行bind   cid bind uid的时候需要执行这个函数
int tgg_bind_session(int fd, const char* uid, const char* cid)
{
	tgg_cli_info* cli = tgg_get_cli_data(fd);
	if (!cli)
		return -1;
	if (cli->status & FD_STATUS_CLOSING)
	{
		RTE_LOG(ERR, USER1, "session is closing, uid[%s] cid[%s].", uid, cid);
		// tgg_free_session(fd);
		return -1;
	}
	// 新增连接时需要对g_gid_hash进行的操作
	if (new_session_for_gidhash(fd, uid) < 0)
		return -1;
	if (new_session_for_uidhash(fd, uid) < 0) {
		// 添加uid失败，gid里面已经添加成功的要移除
		del_session_for_gidhash(fd, uid);
		return -1;
	}
	if (new_session_for_cidhash(fd, cid) < 0) {
		// 添加cid失败，gid和uid里面已经添加成功的要移除
		del_session_for_gidhash(fd, uid);
		del_session_for_uidhash(fd, uid);
		return -1;
	}
	cli->uid = (char*)dpdk_rte_malloc(sizeof(cli->uid));
	memcpy(cli->uid, uid, sizeof(cli->uid));
	cli->uid[sizeof(cli->uid)-1] = '\0';
	// 三个hash表都添加完成之后，就设置为已连接
	cli->status |= FD_STATUS_CONNECTED;
	return 0;
}

int tgg_free_session(int fd)
{
	tgg_cli_info* cli = tgg_get_cli_data(fd);
	if (!cli)
		return -1;
	if (cli->status & FD_STATUS_CLOSED) {
		RTE_LOG(WARNING, USER1, "session is already closed.");
		return 0;
	}
	cli->status |= FD_STATUS_CLOSING;
	// 从hash表中清除连接
	del_session_for_gidhash(fd, cli->uid);
	del_session_for_uidhash(fd, cli->uid);
	del_session_for_cidhash(fd, cli->cid);
	// 清空cli信息
	memset(cli->cid, 0, sizeof(cli->cid));
	memset(cli->uid, 0, sizeof(cli->uid));
	memset(cli->reserved, 0, sizeof(cli->reserved));
	cli->status |= FD_STATUS_CLOSED;
	return 0;

}

int tgg_join_group(const char* cid)
{
	tgg_cid_data* cdata = tgg_get_cid(cid);
	if (!cdata) {
		RTE_LOG(ERR, USER1, "[%s][%d] join group failed, cid[%s] not found.", __func__, __LINE__, cid);
		return -1;
	}
	tgg_cli_info* cli = tgg_get_cli_data(cdata->fd);
	if (!cli){
		RTE_LOG(ERR, USER1, "[%s][%d] join group failed, client data not found, cid[%s].", __func__, __LINE__, cid);
		return -1;
	}

	if (new_session_for_gidhash(cli->fd, cid) < 0) {
		RTE_LOG(ERR, USER1, "[%s][%d] join group failed, cid[%s].", __func__, __LINE__, cid);
		return -1;
	}
	return 0;
}


extern rte_atomic32_t *shared_id_atomic_ptr;

static uint32_t get_cid_idx()
{
	// TODO  后续要考虑自增id超过uint32_max了怎么处理，
	uint32_t current_id_atomic = rte_atomic32_read(shared_id_atomic_ptr);
	rte_atomic32_inc(shared_id_atomic_ptr);
	return current_id_atomic;
}

extern const char[21] g_cid_str;  // 8位地址+4位端口+8位idx+1位结束符'\0'


static void format_idx(uint32_t idx)
{
	char* ptr = g_cid_str[12];// 前面12个已经被占用了
	for (int j = 0; j < 4; j++) {
		sprintf(ptr, "%02X", (idx >> (24 - j * 8)) & 0xFF);
		ptr += 2;
	}
}

const char* get_valid_cid()
{
	uint32_t idx = get_cid_idx();
	format_idx(idx);
	return g_cid_str;
}
#include "tgg_common.h"
#include <rte_log.h>
#include "string.h"
#include <list>
#include "tgg_bw_cache.h"
#include "tgg_lock.h"

extern int g_fd_limit;

// 执行bind   cid bind uid的时候需要执行这个函数
int tgg_bind_session(int fd, const char* uid, const char* cid)
{
	int status = tgg_get_cli_status(fd);
	int idx = tgg_get_cli_idx(fd);
	if (status & FD_STATUS_CLOSING)
	{
		RTE_LOG(ERR, USER1, "[%s][%d]session is closing, uid[%s] cid[%s].", __func__, __LINE__, uid, cid);
		// tgg_free_session(fd);
		return -1;
	}
	std::list<std::string> lstgid = tgg_get_gidsbyuid(uid);
	std::list<std::string>::iterator itgid = lstgid.begin();
	while(itgid != lstgid.end()) {
	// 新增连接时需要对g_gid_hash进行的操作
		if (tgg_add_gid((*itgid).c_str(), fd, idx) < 0)
			goto bind_end;
		itgid++;
	}
	if (tgg_add_uid(uid, fd, idx) < 0) {
		goto bind_end;
	}
	if (tgg_add_cid(cid, fd) < 0) {
		goto bind_end;
	}
	tgg_set_cli_uid(fd, uid);
	// 三个hash表都添加完成之后，就设置为已连接
	status |= FD_STATUS_CONNECTED;
	tgg_set_cli_status(fd, status);
	return 0;

bind_end:
	itgid = lstgid.begin();
	while(itgid != lstgid.end()) {
		tgg_del_fd4gid((*itgid).c_str(), fd, idx);
		itgid++;
	}
	tgg_del_fd4uid(uid, fd, idx);
	tgg_del_cid(cid);
	return -1;
}

int tgg_free_session(int fd)
{
	int status = tgg_get_cli_status(fd);
	if (status < 0 || status & FD_STATUS_CLOSED) {
		RTE_LOG(WARNING, USER1, "session is already closed.");
		return 0;
	}
	// 从hash表中清除连接
	std::string uid = tgg_get_cli_uid(fd);
	int idx = tgg_get_cli_idx(fd);
	std::string cid = tgg_get_cli_cid(fd);
	std::list<std::string> lstgid = tgg_get_gidsbyuid(uid.c_str());
	std::list<std::string>::iterator itgid = lstgid.begin();
	while(itgid != lstgid.end()) {
		tgg_del_fd4gid((*itgid).c_str(), fd, idx);
		itgid++;
	}
	tgg_del_fd4uid(uid.c_str(), fd, idx);
	tgg_del_cid(cid.c_str());

	// 清空cli信息
	tgg_close_cli(fd);
	return 0;

}

int tgg_join_group(const char* gid, const char* cid)
{
	int fd = tgg_get_fdbycid(cid);
	int idx = tgg_get_cli_idx(fd);
	if (fd < 0 || idx < 0) {
		RTE_LOG(ERR, USER1, "[%s][%d] join group failed, cid[%s] not found.", __func__, __LINE__, cid);
		return -1;
	}
	if (tgg_add_gid(gid, fd, idx) < 0){
		RTE_LOG(ERR, USER1, "[%s][%d] join group failed, add gid not found, gid[%s] cid[%s].", 
			__func__, __LINE__, gid, cid);
		return -1;
	}
	std::string uid = tgg_get_cli_uid(fd);
	if (tgg_add_uidgid(uid.c_str(), gid) < 0) {
		RTE_LOG(ERR, USER1, "[%s][%d] join group failed, uid[%s] gid[%s] cid[%s].", 
			__func__, __LINE__, uid.c_str(), gid, cid);
		tgg_del_fd4gid(gid, fd, idx);
		return -1;
	}
	return 0;
}

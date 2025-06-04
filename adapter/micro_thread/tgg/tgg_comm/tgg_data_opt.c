#include "tgg_common.h"
#include <rte_log.h>
#include "string.h"
#include <list>
#include "tgg_bw_cache.h"
#include "comm/log.hpp"

// 执行bind   cid bind uid的时候需要执行这个函数
int tgg_bind_session(const char* uid, int cid)
{
    int fdid = tgg_get_fdbycid(cid);
    if(fdid < 0) {
        LOG_ERROR("get fd by cid[%d] failed.", cid);
        return -1;
    }
	int core_id = fdid & 0xf;
	int fd = fdid >> 8;
	int idx = tgg_get_cli_idx(core_id, fd);
	if (idx < 0) {
		LOG_ERROR("session is closing, uid[%s] cid[%d].", uid, cid);
		return -1;
	}
	if(strlen(uid) <= 0 || cid <= 0) {
		LOG_ERROR("uid[%s] and cid[%d] should not be empty.", uid, cid);
		return -1;
	}
	// 添加到 hash<uid, list<fd>>
	if (tgg_add_uid(uid, fdid) < 0) {
		LOG_ERROR("add uid[%s] fdid[%d] failed.", uid, fd);
		return -1;
	}
	tgg_set_cli_uid(core_id, fd, uid);
	return 0;

}

// 执行unbind
int tgg_unbind_session(int cid)
{
    int fdid = tgg_get_fdbycid(cid);
    if(fdid < 0) {
        LOG_ERROR("get fdid by cid[%d] failed.", cid);
        // TODO 有可能前面已经删除了，还需要观察
        return 0;
    }
	int core_id = fdid & 0xf;
	int fd = fdid >> 8;
	int idx = tgg_get_cli_idx(core_id, fd);
	if (idx < 0) {
		LOG_ERROR("session is closing, cid[%d].", cid);
		return -1;
	}
    std::string suid = tgg_get_cli_uid(core_id, fd);
	if(suid.length() <= 0) {
		// fdid 的 uid已经为空了
		LOG_ERROR("uid for cid[%d] already reseted.", cid);
	} else {
		// 清理
		tgg_set_cli_uid(core_id, fd, "");
	}
	// 从 hash<uid, list<fd>> 中删除
	if (tgg_del_fd4uid(suid.c_str(), fdid) < 0) {
		LOG_ERROR("add uid[%s] fdid[%d] failed.", suid.c_str(), fd);
		return -1;
	}
	return 0;

}

// 关闭一个客户端连接时要触发的释放内容
int tgg_free_session(int core_id, int fd)
{
	// 从hash表中清除连接
	int idx = tgg_get_cli_idx(core_id, fd);
	if(idx <= TGG_FD_CLOSED) {
		LOG_WARNING("session is already closed.");
		return 0;
	}
	std::string uid = tgg_get_cli_uid(core_id, fd);
	int cid = tgg_get_cli_cid(core_id, fd);
	int fdid = (fd << 8) | core_id;
	if(cid > 0) {
		std::list<std::string> lstgid;
		if (!tgg_get_gidsbycid(cid, lstgid)) {
			std::list<std::string>::iterator itgid = lstgid.begin();
			while(itgid != lstgid.end()) {
				// 清理hash<gid,list<fdx>>
				tgg_del_fd4gid((*itgid).c_str(), fdid);
				itgid++;
			}
		}
		// 清理hash<cid,fdx> 没有握手的cid到不了这里来但是也要删除，因此移到了外面去删除
		// tgg_del_cid(cid);
		// 清理hash<cid,list<gid>>
		tgg_del_cid_cidgid(cid);
	}
	// 清理hash<uid,list<fdx>>
	if(!uid.empty()) {
		tgg_del_fd4uid(uid.c_str(), fdid);
	}

	// 清空cli信息  这个信息在由master close以后再清理，这里只清理hash表，由process调用
	// tgg_close_cli(fd);
	return 0;

}

int tgg_join_group(const char* gid, int cid)
{
	int fdid = tgg_get_fdbycid(cid);
	int core_id = fdid & 0xf;
	int fd = fdid >> 8;
	int idx = tgg_get_cli_idx(core_id, fd);
	if (fd < 0 || idx < 0 || cid <= 0) {
		LOG_ERROR("join group failed, cid[%d] not found.", cid);
		return -1;
	}
	// 添加到 hash<gid, list<fdid>>
	if (tgg_add_gid(gid, fdid) < 0){
		LOG_ERROR("join group failed, add gid not found, gid[%s] cid[%d].", gid, cid);
		return -1;
	}
	// 添加到 hash<cid, list<gid>>
	if (tgg_add_cidgid(cid, gid) < 0) {
		LOG_ERROR("join group failed, gid[%s] cid[%d].", gid, cid);
		tgg_del_fd4gid(gid, fdid);// 添加失败时，前面hash<gid, list<fdid>>添加成功的要回退
		return -1;
	}
	return 0;
}

int tgg_exit_group(const char* gid, int cid)
{
	int fdid = tgg_get_fdbycid(cid);
	int core_id = fdid & 0xf;
	int fd = fdid >> 8;
	int idx = tgg_get_cli_idx(core_id, fd);
	if (fd < 0 || idx < 0) {
		LOG_ERROR("connection invalid, cid[%d] not found.", cid);
		return -1;
	}
	// 从 hash<gid, list<fdid>>移除cid对应的fd
	if (tgg_del_fd4gid(gid, fdid) < 0){
		LOG_ERROR("exit group failed, add gid not found, gid[%s] cid[%d].", gid, cid);
		return -1;
	}
	// 从 hash<cid, list<gid>> 中移除gid
	if (tgg_del_gid_cidgid(cid, gid) < 0) {
		LOG_ERROR("exit group failed, gid[%s] cid[%d].", gid, cid);
		return -1;
	}
	return 0;
}

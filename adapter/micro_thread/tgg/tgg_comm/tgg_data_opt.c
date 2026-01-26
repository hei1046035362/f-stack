#include "tgg_common.h"
#include <rte_log.h>
#include "string.h"
#include <vector>
#include "tgg_bw_cache.h"
#include "comm/log.hpp"

// 执行bind   cid bind uid的时候需要执行这个函数
int tgg_bind_session(const char* uid, uint32_t cid)
{
    int64_t fdidcid = tgg_get_fdbycid(cid);
    if(fdidcid < 0) {
        LOG_DEBUG("get fd by cid[%u] failed, cid may not exist.", cid);
        return -1;
    }
	int core_id = GET_COREID_FDCID_MASK(fdidcid);
	int fd = GET_FD_FDCID_MASK(fdidcid);

	std::string _uid = tgg_get_cli_uid(core_id, fd);
	if(!_uid.empty()) {
		LOG_ERROR("bind session failed, cid[%u] allready bind uid[%s]", cid, _uid.c_str());
		return -1;
	}

	if(strlen(uid) <= 0 || cid <= 0) {
		LOG_ERROR("uid[%s] and cid[%u] should not be empty.", uid, cid);
		return -1;
	}
	// 添加到 hash<uid, list<fd>>
	if (tgg_add_uid(uid, fdidcid) < 0) {
		LOG_ERROR("add uid[%s] fdidcid[%lld] failed.", uid, fdidcid);
		return -1;
	}
	tgg_set_cli_uid(core_id, fd, uid);
	return 0;

}

// 执行unbind
int tgg_unbind_session(uint32_t cid)
{
    int64_t fdidcid = tgg_get_fdbycid(cid);
    if(fdidcid < 0) {
        LOG_ERROR("get fdidcid by cid[%u] failed.", cid);
        // TODO 有可能前面已经删除了，还需要观察
        return 0;
    }
	int core_id = GET_COREID_FDCID_MASK(fdidcid);
	int fd = GET_FD_FDCID_MASK(fdidcid);
    std::string suid = tgg_get_cli_uid(core_id, fd);
	if(suid.length() <= 0) {
		// fdid 的 uid已经为空了
		LOG_ERROR("uid for cid[%u] already reseted.", cid);
	} else {
		// 清理
		tgg_set_cli_uid(core_id, fd, "");
	}
	// 从 hash<uid, list<fd>> 中删除
	if (tgg_del_fd4uid(suid.c_str(), fdidcid) < 0) {
		LOG_ERROR("add uid[%s] fdid[%d] failed.", suid.c_str(), fd);
		return -1;
	}
	return 0;

}

int tgg_init_session(int core_id, int fd, int idx)
{
    uint32_t cid = generate_cid(core_id, idx);
    int64_t fdidcid = generate_fdidcid(core_id, fd, cid);
    // 添加到 hash<cid, fd>
    if (tgg_add_cid(cid, fdidcid) < 0) {
        LOG_ERROR("add cid[%u] fdidcid[%ld] failed.", cid, fdidcid);
        return -1;
    }
    LOG_DEBUG("add cid[%u] for fdidcid[%ld] success.", cid, fdidcid);
    tgg_init_cli_bw(core_id, fd, cid);
    return 0;
}

// 关闭一个客户端连接时要触发的释放内容
int tgg_free_session(int core_id, int fd, uint32_t cid)
{
	std::string uid = tgg_get_cli_uid(core_id, fd);
	int64_t fdidcid = generate_fdidcid(core_id, fd, cid);
	if(cid > 0) {
		std::vector<std::string> lstgid;
		if (!tgg_get_gidsbycid(cid, lstgid)) {
			std::vector<std::string>::iterator itgid = lstgid.begin();
			while(itgid != lstgid.end()) {
				// 清理hash<gid,list<fdx>>
				tgg_del_fd4gid((*itgid).c_str(), fdidcid);
				itgid++;
			}
		}
		tgg_del_cid_cidgid(cid);
	}
	// 清理hash<uid,list<fdx>>
	if(!uid.empty()) {
		tgg_del_fd4uid(uid.c_str(), fdidcid);
	}
    if (tgg_del_cid(cid) < 0) {// 删除cid就代表客户端连接信息在bw侧的处理已经完全结束了
        LOG_ERROR("delete cid[%u] failed.", cid);
    }
	// 清空bw侧的cli信息
	tgg_close_cli_bw(core_id, fd);
	return 0;

}

int tgg_join_group(const char* gid, uint32_t cid, bool add_gid)
{
	int64_t fdidcid = tgg_get_fdbycid(cid);
	if (fdidcid <= 0) {
		LOG_ERROR("join group failed, cid[%u] not found.", cid);
		return -1;
	}
	// 添加到 hash<gid, list<fdid>>
	if (add_gid && tgg_add_gid(gid, fdidcid) < 0){
		LOG_ERROR("join group failed, add gid not found, gid[%s] cid[%u].", gid, cid);
		return -1;
	}
	// 添加到 hash<cid, list<gid>>
	if (tgg_add_cidgid(cid, gid) < 0) {
		LOG_ERROR("join group failed, gid[%s] cid[%u].", gid, cid);
		tgg_del_fd4gid(gid, fdidcid);// 添加失败时，前面hash<gid, list<fdid>>添加成功的要回退
		return -1;
	}
	return 0;
}

int tgg_exit_group(const char* gid, uint32_t cid, bool del_gid)
{
	int64_t fdidcid = tgg_get_fdbycid(cid);
	if (fdidcid <= 0) {
		LOG_INFO("connection invalid, cid[%u] not found.", cid);
		return -1;
	}
	// 从 hash<gid, list<fdid>>移除cid对应的fd
	if (del_gid && tgg_del_fd4gid(gid, fdidcid) < 0){
		LOG_DEBUG("exit group failed, gid not found, gid[%s] cid[%u].", gid, cid);
		return -1;
	}
	// 从 hash<cid, list<gid>> 中移除gid
	if (tgg_del_gid_cidgid(cid, gid) < 0) {
		LOG_DEBUG("exit group failed, del gid[%s] cid[%u].", gid, cid);
		return -1;
	}
	return 0;
}

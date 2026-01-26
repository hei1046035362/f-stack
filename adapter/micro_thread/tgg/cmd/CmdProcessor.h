#ifndef __CMD_PROCESSOR_H__
#define __CMD_PROCESSOR_H__

#include "tgg_comm/tgg_common.h"
// RapidJSON 头文件
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

std::string_view get_body_string(const rapidjson::Value& jdata);

// #include <list>
#include <map>
#include <set>
typedef tgg_vhash_list tgg_vhash_list;
class CmdBaseProcessor {
public:

	CmdBaseProcessor(int prc_id, int fd, void* data, const rapidjson::Document& jdata):
	prc_id(prc_id), fd(fd), data(data), jdata(jdata) { need_close = 0; }
    virtual int ExecCmd() = 0;
	virtual ~CmdBaseProcessor() {};
	int NeedClose() {return this->need_close;}
protected:
	static const std::set<uint32_t>& GetEmptySet() {
        static const std::set<uint32_t> empty;  // 单例空集合
        return empty;
    }
	// 发送给服务端，这时候this->fd 就是服务端的fd
	void Send2BW(const rapidjson::Value& data, bool serialize = true);

	// -1 退出， 0 其他进程处理，1本进程处理
	int AddGid4Prcid(const char* gid, std::map<int, tgg_vhash_list*>& mapGid);

	int PushSharecmd(uint32_t cmd, std::map<int, tgg_vhash_list*>& mapGid, bool wait = false,
	 uint32_t cid = 0, const std::string_view data = "", const std::set<uint32_t>& setExcept = CmdBaseProcessor::GetEmptySet());

	int WaitSharecmdRslt(std::vector<int64_t>& lst_fds);

protected:
	int prc_id;// bwprc的进程编号
    int fd;// bwprc的fd
    void* data;
    int need_close;
    const rapidjson::Document& jdata;
};

class CmdWorkerConnect : public CmdBaseProcessor
{
public:
	CmdWorkerConnect(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdWorkerConnect() {}
    int ExecCmd();
};

class CmdGatewayClientConnect : public CmdBaseProcessor
{
public:
	CmdGatewayClientConnect(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdGatewayClientConnect() {}
    int ExecCmd();
};

// class CmdGatewayClientConnect : public CmdBaseProcessor {
// public:

// 	CmdGatewayClientConnect(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
// 	~CmdGatewayClientConnect() {}
//     int ExecCmd() { return 0; }
// };

class CmdSendToOne : public CmdBaseProcessor {
public:

	CmdSendToOne(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdSendToOne() {}
    int ExecCmd();
};

class CmdKick : public CmdBaseProcessor {
public:

	CmdKick(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdKick() {}
    int ExecCmd();
};

class CmdDestroy : public CmdBaseProcessor {
public:

	CmdDestroy(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdDestroy() {}
    int ExecCmd();
};

class CmdSendToALL : public CmdBaseProcessor {
public:

	CmdSendToALL(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdSendToALL() {}
    int ExecCmd();
};

class CmdSelect : public CmdBaseProcessor {
public:

	CmdSelect(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdSelect() {}
    int ExecCmd();
private:
	// 返回字段的掩码  如果请求的field字段中有对应的字段，对应的位会设置为1，第一位是cid，第二位是uid，第三位是gid
	enum FieldsMask {
	    FIELD_CID = 1,
	    FIELD_UID = 2,
	    FIELD_GID = 4,
	};

	// 把需要的信息用json存起来，方便后面序列化
	// @param lst_fd    客户端连接链表
	// @param mask      输出信息掩码，目前之后cid,uid,gid
	// @param result    返回json对象
	void FormatResult(const std::vector<int64_t>& lst_fd, int mask, rapidjson::Document& result);
};

class CmdGetGroupIdList : public CmdBaseProcessor {
public:

	CmdGetGroupIdList(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdGetGroupIdList() {}
    int ExecCmd();
};


class CmdSetSession : public CmdBaseProcessor {
public:

	CmdSetSession(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdSetSession() {}
    int ExecCmd();
};

class CmdUpdateSession : public CmdBaseProcessor {
public:

	CmdUpdateSession(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdUpdateSession() {}
    int ExecCmd();
};

class CmdGetSessionByCid : public CmdBaseProcessor {
public:

	CmdGetSessionByCid(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdGetSessionByCid() {}
    int ExecCmd();
};

class CmdGetAllClientSession : public CmdBaseProcessor {
public:

	CmdGetAllClientSession(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdGetAllClientSession() {}
    int ExecCmd();
};

class CmdIsOnline : public CmdBaseProcessor {
public:

	CmdIsOnline(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdIsOnline() {}
    int ExecCmd();
};

class CmdBindUid : public CmdBaseProcessor {
public:

	CmdBindUid(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdBindUid() {}
    virtual int ExecCmd();
};

class CmdUnBindUid : public CmdBaseProcessor {
public:

	CmdUnBindUid(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdUnBindUid() {}
    int ExecCmd();
};

class CmdSendToUid : public CmdBaseProcessor {
public:

	CmdSendToUid(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdSendToUid() {}
    int ExecCmd();
};

class CmdJoinGroup : public CmdBaseProcessor {
public:

	CmdJoinGroup(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdJoinGroup() {}
    int ExecCmd();
};

class CmdLeaveGroup : public CmdBaseProcessor {
public:

	CmdLeaveGroup(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdLeaveGroup() {}
    int ExecCmd();
};

class CmdUnGroup : public CmdBaseProcessor {
public:

	CmdUnGroup(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdUnGroup() {}
    int ExecCmd();
};

class CmdSendToGroup : public CmdBaseProcessor {
public:

	CmdSendToGroup(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdSendToGroup() {}
    int ExecCmd();
};

class CmdGetClientSessionsByGroup : public CmdBaseProcessor {
public:

	CmdGetClientSessionsByGroup(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdGetClientSessionsByGroup() {}
    int ExecCmd();
};

class CmdGetClientCountByGroup : public CmdBaseProcessor {
public:

	CmdGetClientCountByGroup(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdGetClientCountByGroup() {}
    int ExecCmd();
};

class CmdGetClientIdByUid : public CmdBaseProcessor {
public:

	CmdGetClientIdByUid(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdGetClientIdByUid() {}
    int ExecCmd();
};

class CmdBatchGetClientIdByUid : public CmdBaseProcessor {
public:

	CmdBatchGetClientIdByUid(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata){}
	~CmdBatchGetClientIdByUid() {}
    int ExecCmd();
};

class CmdBatchGetClientCountByGroup : public CmdBaseProcessor {
public:

	CmdBatchGetClientCountByGroup(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdBatchGetClientCountByGroup() {}
    int ExecCmd() { return 0; }
};

class CmdFreeClientSession : public CmdBaseProcessor {
private:
	uint32_t cid;
public:

	CmdFreeClientSession(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
	~CmdFreeClientSession() {}
    int ExecCmd();
    void SetCid(uint32_t cid) {this->cid = cid;}
};


int exec_cmd_processor(int prc_id, int fd, void* data);

int exec_free_session(int prc_id, int fd, uint32_t cid);

#endif // __CMD_PROCESSOR_H__
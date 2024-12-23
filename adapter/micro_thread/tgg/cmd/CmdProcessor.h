#ifndef __CMD_PROCESSOR_H__
#define __CMD_PROCESSOR_H__

#include "tgg_comm/tgg_common.h"
#include "nlohmann/json.hpp"
#include <list>
class CmdBaseProcessor {
public:

	CmdBaseProcessor(int fd, void* data, const nlohmann::json& jdata):fd(fd), data(data), jdata(jdata) {}
    virtual int ExecCmd() = 0;
	virtual ~CmdBaseProcessor() {};
protected:
	// 发送给服务端，这时候this->fd 就是服务端的fd
	void Send2BW(const std::string& data);

protected:
    int fd;
    void* data;
    const nlohmann::json& jdata;
};

class CmdWorkerConnect : public CmdBaseProcessor
{
public:
	CmdWorkerConnect(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdWorkerConnect() {}
    int ExecCmd();
};

class CmdGatewayClientConnect : public CmdBaseProcessor {
public:

	CmdGatewayClientConnect(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdGatewayClientConnect() {}
    int ExecCmd() { return 0; }
};

class CmdSendToOne : public CmdBaseProcessor {
public:

	CmdSendToOne(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdSendToOne() {}
    int ExecCmd();
};

class CmdKick : public CmdBaseProcessor {
public:

	CmdKick(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdKick() {}
    int ExecCmd();
};

class CmdDestroy : public CmdBaseProcessor {
public:

	CmdDestroy(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdDestroy() {}
    int ExecCmd();
};

class CmdSendToALL : public CmdBaseProcessor {
public:

	CmdSendToALL(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdSendToALL() {}
    int ExecCmd();
};

class CmdSelect : public CmdBaseProcessor {
public:

	CmdSelect(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
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
	void FormatResult(const std::list<int>& lst_fd, int mask, nlohmann::json& result);
};

class CmdGetGroupIdList : public CmdBaseProcessor {
public:

	CmdGetGroupIdList(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdGetGroupIdList() {}
    int ExecCmd();
};


class CmdSetSession : public CmdBaseProcessor {
public:

	CmdSetSession(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdSetSession() {}
    int ExecCmd();
};

class CmdUpdateSession : public CmdBaseProcessor {
public:

	CmdUpdateSession(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdUpdateSession() {}
    int ExecCmd();
};

class CmdGetSessionByCid : public CmdBaseProcessor {
public:

	CmdGetSessionByCid(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdGetSessionByCid() {}
    int ExecCmd() {return 0;}
};

class CmdGetAllClientSession : public CmdBaseProcessor {
public:

	CmdGetAllClientSession(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdGetAllClientSession() {}
    int ExecCmd() {return 0;}
};

class CmdIsOnline : public CmdBaseProcessor {
public:

	CmdIsOnline(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdIsOnline() {}
    int ExecCmd();
};

class CmdBindUid : public CmdBaseProcessor {
public:

	CmdBindUid(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdBindUid() {}
    virtual int ExecCmd();
};

class CmdUnBindUid : public CmdBaseProcessor {
public:

	CmdUnBindUid(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdUnBindUid() {}
    int ExecCmd();
};

class CmdSendToUid : public CmdBaseProcessor {
public:

	CmdSendToUid(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdSendToUid() {}
    int ExecCmd();
};

class CmdJoinGroup : public CmdBaseProcessor {
public:

	CmdJoinGroup(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdJoinGroup() {}
    int ExecCmd();
};

class CmdLeaveGroup : public CmdBaseProcessor {
public:

	CmdLeaveGroup(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdLeaveGroup() {}
    int ExecCmd();
};

class CmdUnGroup : public CmdBaseProcessor {
public:

	CmdUnGroup(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdUnGroup() {}
    int ExecCmd();
};

class CmdSendToGroup : public CmdBaseProcessor {
public:

	CmdSendToGroup(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdSendToGroup() {}
    int ExecCmd();
};

class CmdGetClientSessionsByGroup : public CmdBaseProcessor {
public:

	CmdGetClientSessionsByGroup(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdGetClientSessionsByGroup() {}
    int ExecCmd();
};

class CmdGetClientCountByGroup : public CmdBaseProcessor {
public:

	CmdGetClientCountByGroup(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdGetClientCountByGroup() {}
    int ExecCmd();
};

class CmdGetClientIdByUid : public CmdBaseProcessor {
public:

	CmdGetClientIdByUid(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdGetClientIdByUid() {}
    int ExecCmd();
};

class CmdBatchGetClientIdByUid : public CmdBaseProcessor {
public:

	CmdBatchGetClientIdByUid(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata){}
	~CmdBatchGetClientIdByUid() {}
    int ExecCmd();
};

class CmdBatchGetClientCountByGroup : public CmdBaseProcessor {
public:

	CmdBatchGetClientCountByGroup(int fd, void* data, const nlohmann::json& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdBatchGetClientCountByGroup() {}
    int ExecCmd() { return 0; }
};



void exec_cmd_processor(int fd, void* data);

#endif // __CMD_PROCESSOR_H__
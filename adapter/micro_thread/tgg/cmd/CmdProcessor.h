#ifndef __CMD_PROCESSOR_H__
#define __CMD_PROCESSOR_H__

#include "nlohmann/json.hpp"
#include "string.h"
#include "tgg_comm/tgg_common.h"

class CmdBaseProcessor {
public:

	CmdBaseProcessor(int fd, void* data, const std::string& jdata):fd(fd), data(data), jdata(jdata) {}
    virtual int ExecCmd() = 0;
	virtual ~CmdBaseProcessor() { clean_bw_data((tgg_bw_data*)data); };

protected:
    int fd;
    void* data;
    const std::string& jdata;
};

class CmdWorkerConnect : public CmdBaseProcessor
{
public:
	CmdWorkerConnect(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdWorkerConnect() {}
    int ExecCmd();
};

class CmdGatewayClientConnect : public CmdBaseProcessor {
public:

	CmdGatewayClientConnect(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdGatewayClientConnect() {}
    int ExecCmd() { return 0; }
};

class CmdSendToOne : public CmdBaseProcessor {
public:

	CmdSendToOne(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdSendToOne() {}
    int ExecCmd() { return 0; }
};

class CmdKick : public CmdBaseProcessor {
public:

	CmdKick(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdKick() {}
    int ExecCmd() { return 0; }
};

class CmdDestroy : public CmdBaseProcessor {
public:

	CmdDestroy(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdDestroy() {}
    int ExecCmd() { return 0; }
};

class CmdSendToALL : public CmdBaseProcessor {
public:

	CmdSendToALL(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdSendToALL() {}
    int ExecCmd() { return 0; }
};

class CmdSelect : public CmdBaseProcessor {
public:

	CmdSelect(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdSelect() {}
    int ExecCmd() { return 0; }
};

class CmdGetGroupIdList : public CmdBaseProcessor {
public:

	CmdGetGroupIdList(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdGetGroupIdList() {}
    int ExecCmd() { return 0; }
};


class CmdSetSession : public CmdBaseProcessor {
public:

	CmdSetSession(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdSetSession() {}
    int ExecCmd() { return 0; }
};

class CmdUpdateSession : public CmdBaseProcessor {
public:

	CmdUpdateSession(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdUpdateSession() {}
    int ExecCmd() { return 0; }
};

class CmdGetSessionByCid : public CmdBaseProcessor {
public:

	CmdGetSessionByCid(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdGetSessionByCid() {}
    int ExecCmd() { return 0; }
};

class CmdGetAllClientSession : public CmdBaseProcessor {
public:

	CmdGetAllClientSession(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdGetAllClientSession() {}
    int ExecCmd() { return 0; }
};

class CmdIsOnline : public CmdBaseProcessor {
public:

	CmdIsOnline(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdIsOnline() {}
    int ExecCmd() { return 0; }
};

class CmdBindUid : public CmdBaseProcessor {
public:

	CmdBindUid(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdBindUid() {}
    virtual int ExecCmd() { return 0; }
};

class CmdUnBindUid : public CmdBaseProcessor {
public:

	CmdUnBindUid(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdUnBindUid() {}
    int ExecCmd() { return 0; }
};

class CmdSendToUid : public CmdBaseProcessor {
public:

	CmdSendToUid(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdSendToUid() {}
    int ExecCmd() { return 0; }
};

class CmdJoinGroup : public CmdBaseProcessor {
public:

	CmdJoinGroup(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdJoinGroup() {}
    int ExecCmd() { return 0; }
};

class CmdLeaveGroup : public CmdBaseProcessor {
public:

	CmdLeaveGroup(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdLeaveGroup() {}
    int ExecCmd() { return 0; }
};

class CmdUnGroup : public CmdBaseProcessor {
public:

	CmdUnGroup(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdUnGroup() {}
    int ExecCmd() { return 0; }
};

class CmdSendToGroup : public CmdBaseProcessor {
public:

	CmdSendToGroup(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdSendToGroup() {}
    int ExecCmd();
};

class CmdGetClientSessionsByGroup : public CmdBaseProcessor {
public:

	CmdGetClientSessionsByGroup(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdGetClientSessionsByGroup() {}
    int ExecCmd() { return 0; }
};

class CmdGetClientCountByGroup : public CmdBaseProcessor {
public:

	CmdGetClientCountByGroup(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdGetClientCountByGroup() {}
    int ExecCmd() { return 0; }
};

class CmdGetClientIdByUid : public CmdBaseProcessor {
public:

	CmdGetClientIdByUid(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdGetClientIdByUid() {}
    int ExecCmd() { return 0; }
};

class CmdBatchGetClientIdByUid : public CmdBaseProcessor {
public:

	CmdBatchGetClientIdByUid(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata){}
	~CmdBatchGetClientIdByUid() {}
    int ExecCmd() { return 0; }
};

class CmdBatchGetClientCountByGroup : public CmdBaseProcessor {
public:

	CmdBatchGetClientCountByGroup(int fd, void* data, const std::string& jdata):CmdBaseProcessor(fd, data, jdata) {}
	~CmdBatchGetClientCountByGroup() {}
    int ExecCmd() { return 0; }
};



CmdBaseProcessor* get_cmd_processor(int fd, void* data);

#endif // __CMD_PROCESSOR_H__
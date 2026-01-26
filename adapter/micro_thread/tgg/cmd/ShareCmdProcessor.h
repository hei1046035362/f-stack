#pragma once
#include "tgg_comm/tgg_struct.h"
class ShareCmdBaseProcessor {
public:

    ShareCmdBaseProcessor(int prc_id, bw_share_qdata* data):
    prc_id(prc_id), data(data) {need_close = 0;}
    virtual int ExecCmd() = 0;
    virtual ~ShareCmdBaseProcessor() {tgg_clean_bw_share_qdata(this->prc_id, this->data);};
    int NeedClose() {return this->need_close;}

protected:
    int prc_id;// bwprc的进程编号
    int fd;// bwprc的fd
    bw_share_qdata* data;
    int need_close;
};

class ShareCmdSelect : public ShareCmdBaseProcessor
{
public:
    ShareCmdSelect(int prc_id, bw_share_qdata* data):ShareCmdBaseProcessor(prc_id, data) {}
    ~ShareCmdSelect() {}
    int ExecCmd();
};

class ShareCmdJoinGroup : public ShareCmdBaseProcessor
{
public:
    ShareCmdJoinGroup(int prc_id, bw_share_qdata* data):ShareCmdBaseProcessor(prc_id, data) {}
    ~ShareCmdJoinGroup() {}
    int ExecCmd();
};

class ShareCmdLeaveGroup : public ShareCmdBaseProcessor
{
public:
    ShareCmdLeaveGroup(int prc_id, bw_share_qdata* data):ShareCmdBaseProcessor(prc_id, data) {}
    ~ShareCmdLeaveGroup() {}
    int ExecCmd();
};

class ShareCmdUnGroup : public ShareCmdBaseProcessor
{
public:
    ShareCmdUnGroup(int prc_id, bw_share_qdata* data):ShareCmdBaseProcessor(prc_id, data) {}
    ~ShareCmdUnGroup() {}
    int ExecCmd();
};

class ShareCmdSendToGroup : public ShareCmdBaseProcessor
{
public:
    ShareCmdSendToGroup(int prc_id, bw_share_qdata* data):ShareCmdBaseProcessor(prc_id, data) {}
    ~ShareCmdSendToGroup() {}
    int ExecCmd();
};

class ShareCmdGetClientSessionsByGroup : public ShareCmdBaseProcessor
{
public:
    ShareCmdGetClientSessionsByGroup(int prc_id, bw_share_qdata* data):ShareCmdBaseProcessor(prc_id, data) {}
    ~ShareCmdGetClientSessionsByGroup() {}
    int ExecCmd();
};

class ShareCmdGetClientCountByGroup : public ShareCmdBaseProcessor
{
public:
    ShareCmdGetClientCountByGroup(int prc_id, bw_share_qdata* data):ShareCmdBaseProcessor(prc_id, data) {}
    ~ShareCmdGetClientCountByGroup() {}
    int ExecCmd();
};

class ShareCmdPrintMemStats : public ShareCmdBaseProcessor
{
public:
    ShareCmdPrintMemStats(int prc_id, bw_share_qdata* data):ShareCmdBaseProcessor(prc_id, data) {}
    ~ShareCmdPrintMemStats() {}
    int ExecCmd();
};

int exec_sharequeue_cmd_processor(int prc_id);

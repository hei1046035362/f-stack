#ifndef __TGG_CMD_PROCESSOR_H__
#define __TGG_CMD_PROCESSOR_H__
#include "CmdProcessor.h"

class CmdTggGateway : public CmdBaseProcessor {
public:

    CmdTggGateway(int prc_id, int fd, void* data, const rapidjson::Document& jdata):CmdBaseProcessor(prc_id, fd, data, jdata) {}
    ~CmdTggGateway() {}
    int ExecCmd();
private:
    int ReloadIpFilter();
    int UpdateRealWorkers();

    int PrintAllGids();
    int PrintGidCount();
    int PrintGidCids(rapidjson::Document& body);
    int PrintAllUids();
    int PrintUidCount();
    int PrintUidCids(rapidjson::Document& body);
    int PrintAllCids();
    int PrintCidCount();
    int PrintAllIdxs();
    int PrintIdxCount();
    int PrintAllWorkerKeys();
    int PrintWorkerKeyCount();
    int PrintAllWorkers();
    int PrintWorkerCount();
    int PrintRealAllWorkers();
    int PrintRealWorkerCount();
    int CheckGidcidAvaliable();
    int CheckUidcidAvaliable();
    int CheckCidAvaliable();
private:
    std::string _print_path;
};


#endif  // __TGG_CMD_PROCESSOR_H__
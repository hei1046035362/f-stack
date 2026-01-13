#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include "tgg_comm/tgg_conf.h"
#include "tgg_comm/tgg_common.h"

#include "CommandProcessor.cpp"

const char* f_stack_ini = "/data/code/f-stack/config.ini";

int g_run = 1;
static const char* s_dump_file = "/var/corefiles/";//tgg_gw_cliprc_core

void signal_handler(int signum)
{
    SIG_PRINTF("tgg-cmd catched signal:%d\n", signum);
    if(signum == SIGINT || signum == SIGTERM) {
        if(g_run) {
            g_run = 0;
        }
    }
}

void tgg_sig_init()
{
    if (signal(SIGINT, signal_handler) == SIG_ERR) {
        LOG_ERROR("Error setting signal handler");
        prc_exit(-1, "Error setting signal handler");
    }
    if (signal(SIGTERM, signal_handler) == SIG_ERR) {
        LOG_ERROR("Error setting signal handler");
        prc_exit(-1, "Error setting signal handler");
    }

}

void tgg_process_init()
{
    tgg_sig_init();// 信号处理初始化
    tgg_secondary_init();// dpdk相关初始化
}

void tgg_process_uninit()
{
    int ret = rte_eal_cleanup();
    if (ret)
        printf("Error from rte_eal_cleanup(), %d\n", ret);
}

static void prc_dpdk_eal_init(int argc, char **argv)
{
    char c_flag[24] = {0};
    sprintf(c_flag, "-c2");
    char n_flag[] = "-n4";
    char mp_flag[] = "--proc-type=secondary";
    char log_flag[] = "--log-level=6";
    char *argp[argc + 4];
    // uint16_t nb_ports;

    argp[0] = argv[0];
    argp[1] = c_flag;
    argp[2] = n_flag;
    argp[3] = mp_flag;
    argp[4] = log_flag;

    for (int i = 1; i < argc; i++)
        argp[i + 4] = argv[i];

    argc += 4;

    int ret = rte_eal_init(argc, argp);
    if (ret < 0)
        rte_panic("Cannot init EAL\n");
}


int main(int argc, char* argv[]) {
    init_core(s_dump_file);
    if (tgg_init_config(argc, argv) < 0) {
        printf("init config error.\n");
        return -1;
    }
    if (AsyncLogger::getInstance().init(TggConfigure::getInstance()->get_log_path(), 
        TggConfigure::getInstance()->get_gateway_log_level()) < 0) {
        printf("init log error.\n");
        return -1;
    }
    prc_dpdk_eal_init(argc, argv);
    // mt_init_frame(argc, argv);
    LOG_INFO("-----------cliprc[pid:%d] start-----------", getpid());
    // 设置控制台编码（Windows）
    #ifdef _WIN32
        system("chcp 65001 > nul");  // 设置为UTF-8编码
    #endif
    tgg_process_init();
    CommandProcessor processor;
    
    try {
        processor.run();
    } catch (const exception& e) {
        cerr << "程序发生错误: " << e.what() << endl;
    } catch (...) {
        cerr << "程序发生未知错误" << endl;
    }

    tgg_process_uninit();

    return 0;
}
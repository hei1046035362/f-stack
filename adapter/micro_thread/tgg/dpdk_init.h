#ifndef _DPDK_INIT_H_
#define _DPDK_INIT_H_

void tgg_master_init();
void tgg_master_uninit();

void tgg_secondary_init();
void tgg_secondary_uninit();

void tgg_gwrcv_secondary_init();
void tgg_unregister_rcu();

void tgg_cliprc_init();
void tgg_cliprc_uninit();

void tgg_bwprc_init(int bwcount);
void tgg_bwprc_uninit(int bwcount);

void tgg_register_init();
void tgg_register_uninit();

void init_flag_for_process();
void prc_exit(int exit_code, const char* fmt, ...);

#endif //_DPDK_INIT_H_
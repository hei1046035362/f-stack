#ifndef _BWSERVER_H_
#define _BWSERVER_H_

int init_bwserver();
void tgg_process_bwrcv_data(void* arg);
void uninit_bwserver();

#endif   // _BWSERVER_H_
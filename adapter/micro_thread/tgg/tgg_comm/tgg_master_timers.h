#ifndef __TGG_MASTER_H__
#define __TGG_MASTER_H__
#include <rte_timer.h>
void init_timer();
void stop_timer();

void check_gw_monitor(struct rte_timer* tm, void* arg);

int check_if_all_child_up();
void kill_all_child();

#endif  //__TGG_MASTER_H__
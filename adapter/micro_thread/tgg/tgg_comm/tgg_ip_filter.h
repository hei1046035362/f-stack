#ifndef __H_IP_FILTER_HPP__
#define __H_IP_FILTER_HPP__

#include <rte_hash.h>

// primary 调用接口
bool init_ip_filter(bool is_primary, const char* ip_filter_path);
bool reload_ip_filter(const char* ip_filter_path);
void cleanup_ip_filter();

// secondary 调用接口
void sync_ip_filter();


// 接收，但是不进入websocket的流程
bool is_ip_exclude(uint64_t ip);

// 白名单中有，或者黑名单中没有，暂时没使用
bool is_ip_allowed(uint64_t ip);


#endif // __H_IP_FILTER_HPP__
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include "connection_pool.h"

// 连接池结构
static struct rte_hash *connection_map = NULL;

// 初始化连接池
ngx_int_t ws_connection_pool_init(ngx_log_t *log)
{
    ngx_log_error(NGX_LOG_INFO, log, 0, "try to create connection map, process type:%d", rte_eal_process_type());
    if(rte_eal_process_type() != RTE_PROC_PRIMARY) {
        connection_map = rte_hash_find_existing("ws_connection_map");
        return NGX_OK;
    }
    struct rte_hash_parameters params = {
        .name = "ws_connection_map",
        .entries = 1024 * 1024, // 支持百万连接
        .key_len = 64,
        .hash_func = rte_jhash,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id(),
    };
    
    connection_map = rte_hash_create(&params);
    if (connection_map == NULL) {
        ngx_log_error(NGX_LOG_EMERG, log, 0, "Failed to create connection map");
        return NGX_ERROR;
    }
    
    return NGX_OK;
}

// 添加连接
ngx_int_t ws_connection_pool_add(ngx_str_t client_id, ngx_connection_t *connection)
{
    int ret = rte_hash_add_key_with_hash_data(connection_map, client_id.data, rte_jhash(client_id.data, client_id.len, 0), connection);
    if (ret < 0) {
        return NGX_ERROR;
    }
    return NGX_OK;
}

// 查找连接
ngx_connection_t *ws_connection_pool_find(ngx_str_t client_id)
{
    void *data;
    int ret = rte_hash_lookup_with_hash_data(connection_map, client_id.data, rte_jhash(client_id.data, client_id.len, 0), &data);
    if (ret < 0) {
        return NULL;
    }
    return (ngx_connection_t *)data;
}

// 移除连接
ngx_int_t ws_connection_pool_remove(ngx_str_t client_id)
{
    int ret = rte_hash_del_key_with_hash(connection_map, client_id.data, rte_jhash(client_id.data, client_id.len, 0));
    if (ret < 0) {
        return NGX_ERROR;
    }
    return NGX_OK;
}
#ifndef NGX_HTTP_CONNECTION_POOL_H
#define NGX_HTTP_CONNECTION_POOL_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

// 函数声明
ngx_int_t ws_connection_pool_init(ngx_log_t *log);
ngx_int_t ws_connection_pool_add(ngx_str_t client_id, ngx_connection_t *connection);
ngx_connection_t *ws_connection_pool_find(ngx_str_t client_id);
ngx_int_t ws_connection_pool_remove(ngx_str_t client_id);

#endif // NGX_HTTP_CONNECTION_POOL_H
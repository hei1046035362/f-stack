#include "server.h"
#include "mt_api.h"
static client_context_t *create_client_context(int fd) {
    client_context_t *ctx = (client_context_t *)malloc(sizeof(client_context_t));
    if (!ctx) return NULL;
    
    ctx->fd = fd;
    ctx->buffer_len = 0;
    ctx->write_len = 0;
    ctx->total_read = 0;
    ctx->total_write = 0;
    ctx->uthread = NULL;
    memset(ctx->buffer, 0, BUFFER_SIZE);
    
    return ctx;
}

static void free_client_context(client_context_t *ctx) {
    if (ctx) {
        if (ctx->fd >= 0) {
            ff_close(ctx->fd);
        }
        free(ctx);
    }
}

server_t *server_create(int port) {
    server_t *server = (server_t *)malloc(sizeof(server_t));
    if (!server) return NULL;
    
    if (!reactor_create(MAX_CLIENTS + 10)) {
        free(server);
        return NULL;
    }

    server->port = port;
    server->listen_fd = -1;
    server->running = 0;
    
    return server;
}

void server_destroy(server_t *server) {
    if (!server) return;
    
    // if (server->reactor) {
    //     reactor_destroy(server->reactor);
    // }
    
    if (server->listen_fd >= 0) {
        ff_close(server->listen_fd);
    }
    
    free(server);
}

void accept_callback(int fd, event_type_t events, void *arg) {
    server_t *server = (server_t *)arg;
    
    if (events & EVENT_ERROR) {
        printf("Error on listen socket\n");
        return;
    }
    
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    int client_fd = ff_accept(fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
        printf("Accept failed: %d\n", client_fd);
        return;
    }
    
    // 设置非阻塞
    ff_fcntl(client_fd, F_SETFL, ff_fcntl(client_fd, F_GETFL, 0) | O_NONBLOCK);
    
    client_context_t *client_ctx = create_client_context(client_fd);
    if (!client_ctx) {
        ff_close(client_fd);
        return;
    }
    
    // 创建微线程处理客户端
    client_ctx->uthread = ff_uthread_create(UTHREAD_STACK_SIZE, 
                                          (ff_uthread_func_t)client_read_callback, 
                                          client_fd, EVENT_READ, client_ctx);
    
    if (!client_ctx->uthread) {
        free_client_context(client_ctx);
        return;
    }
    
    printf("New client connected: fd=%d\n", client_fd);
    
    // 添加到reactor监控读事件
    if (reactor_add_event(server->reactor, client_fd, EVENT_READ, 
                         client_read_callback, client_ctx) < 0) {
        ff_uthread_release(client_ctx->uthread);
        free_client_context(client_ctx);
    }
}

void client_read_callback(int fd, event_type_t events, void *arg) {
    client_context_t *ctx = (client_context_t *)arg;
    
    if (events & EVENT_ERROR) {
        printf("Client %d error, closing\n", fd);
        reactor_remove_event(ctx->uthread->reactor, fd);
        free_client_context(ctx);
        return;
    }
    
    if (!(events & EVENT_READ)) {
        return;
    }
    
    // 读取数据
    int n = ff_read(fd, ctx->buffer + ctx->buffer_len, BUFFER_SIZE - ctx->buffer_len - 1);
    if (n <= 0) {
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            printf("Read error from client %d, closing\n", fd);
        } else {
            printf("Client %d disconnected\n", fd);
        }
        reactor_remove_event(ctx->uthread->reactor, fd);
        free_client_context(ctx);
        return;
    }
    
    ctx->buffer_len += n;
    ctx->total_read += n;
    ctx->buffer[ctx->buffer_len] = '\0';
    
    printf("Received %d bytes from client %d: %.*s\n", n, fd, n, ctx->buffer + ctx->buffer_len - n);
    
    // 简单协议：收到换行符或达到一定长度就回复
    if (ctx->buffer_len > 0 && (ctx->buffer[ctx->buffer_len-1] == '\n' || 
                               ctx->buffer_len >= BUFFER_SIZE - 1)) {
        // 准备回复数据
        const char *response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nHello from F-Stack Reactor Server!\r\n";
        int response_len = strlen(response);
        
        if (ctx->buffer_len + response_len < BUFFER_SIZE) {
            memcpy(ctx->buffer + ctx->buffer_len, response, response_len);
            ctx->buffer_len += response_len;
        }
        
        // 修改为监控写事件
        reactor_modify_event(ctx->uthread->reactor, fd, EVENT_WRITE);
    }
}

void client_write_callback(int fd, event_type_t events, void *arg) {
    client_context_t *ctx = (client_context_t *)arg;
    
    if (events & EVENT_ERROR) {
        printf("Client %d error during write, closing\n", fd);
        reactor_remove_event(ctx->uthread->reactor, fd);
        free_client_context(ctx);
        return;
    }
    
    if (!(events & EVENT_WRITE)) {
        return;
    }
    
    // 发送数据
    int n = ff_write(fd, ctx->buffer + ctx->write_len, ctx->buffer_len - ctx->write_len);
    if (n <= 0) {
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            printf("Write error to client %d, closing\n", fd);
        }
        reactor_remove_event(ctx->uthread->reactor, fd);
        free_client_context(ctx);
        return;
    }
    
    ctx->write_len += n;
    ctx->total_write += n;
    
    printf("Sent %d bytes to client %d\n", n, fd);
    
    // 如果所有数据都发送完毕
    if (ctx->write_len >= ctx->buffer_len) {
        // 重置缓冲区，准备读取下一次请求
        ctx->buffer_len = 0;
        ctx->write_len = 0;
        memset(ctx->buffer, 0, BUFFER_SIZE);
        
        // 修改为监控读事件
        reactor_modify_event(ctx->uthread->reactor, fd, EVENT_READ);
    }
}

int server_start(server_t *server) {
    if (!server) return -1;
    
    // 创建监听socket
    server->listen_fd = ff_socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        printf("Create socket failed\n");
        return -1;
    }
    
    // 设置SO_REUSEADDR
    int reuse = 1;
    ff_setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // 绑定地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(server->port);
    
    if (ff_bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("Bind failed\n");
        ff_close(server->listen_fd);
        return -1;
    }
    
    // 监听
    if (ff_listen(server->listen_fd, BACKLOG) < 0) {
        printf("Listen failed\n");
        ff_close(server->listen_fd);
        return -1;
    }
    
    printf("Server listening on port %d\n", server->port);
    
    // 添加监听socket到reactor
    if (reactor_add_event(server->reactor, server->listen_fd, EVENT_READ, 
                         accept_callback, server) < 0) {
        printf("Add listen event failed\n");
        ff_close(server->listen_fd);
        return -1;
    }
    
    server->running = 1;
    
    // 运行reactor
    reactor_run(server->reactor);
    
    return 0;
}

void server_stop(server_t *server) {
    if (server) {
        server->running = 0;
        reactor_stop(server->reactor);
    }
}
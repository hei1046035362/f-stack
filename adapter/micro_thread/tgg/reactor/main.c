#include "server.h"
#include <signal.h>

static server_t *g_server = NULL;

void signal_handler(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    if (g_server) {
        server_stop(g_server);
    }
}

int main(int argc, char *argv[]) {
    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    int port = SERVER_PORT;
    if (argc > 1) {
        port = atoi(argv[1]);
    }
    
    printf("Starting F-Stack Reactor Server on port %d...\n", port);
    
    // 创建服务器
    g_server = server_create(port);
    if (!g_server) {
        printf("Failed to create server\n");
        return -1;
    }
    
    // 启动服务器
    if (server_start(g_server) < 0) {
        printf("Failed to start server\n");
        server_destroy(g_server);
        return -1;
    }
    
    // 清理资源
    server_destroy(g_server);
    
    printf("Server shutdown complete\n");
    return 0;
}
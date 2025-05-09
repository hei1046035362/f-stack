#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>

#define BUFFER_SIZE 1024

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <server ip> <port>" << std::endl;
        exit(EXIT_FAILURE);
    }
    
    const char* server_ip = argv[1];
    int port = std::atoi(argv[2]);
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket create failed");
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr) <= 0) {
        std::cerr << "invalid address" << std::endl;
        exit(EXIT_FAILURE);
    }
    
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect server failed");
        exit(EXIT_FAILURE);
    }
    
    // 发送数据到服务器
    std::string message = "Hello from client!";
    if (send(sock, message.c_str(), message.size(), 0) < 0) {
        perror("send message failed");
        close(sock);
        exit(EXIT_FAILURE);
    }
    std::cout << "send message to server success" << std::endl;
    
    char buffer[BUFFER_SIZE] = {0};
    int bytes_read = read(sock, buffer, BUFFER_SIZE);
    if (bytes_read > 0) {
        std::cout << "recv message from server: " << buffer << std::endl;
    } else if (bytes_read == 0) {
        std::cout << "server closed connection" << std::endl;
    } else {
        perror("recv message from server failed");
    }
    
    close(sock);
    return 0;
}


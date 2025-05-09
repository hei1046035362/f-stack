#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <sys/epoll.h>

#define MAX_EVENTS 10
#define BUFFER_SIZE 1024

void set_nonblocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " port" << std::endl;
        return EXIT_FAILURE;
    }

    short port = static_cast<short>(std::atoi(argv[1]));
    if (port <= 0) {
        std::cerr << "invalid listen port" << std::endl;
        return EXIT_FAILURE;
    }

    int server_fd, epoll_fd;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket create failed");
        return EXIT_FAILURE;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        return EXIT_FAILURE;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen failed");
        return EXIT_FAILURE;
    }

    set_nonblocking(server_fd);

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1 failed");
        return EXIT_FAILURE;
    }

    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);

    std::cout << "listen " << port << " successful ..." << std::endl;

    struct epoll_event events[MAX_EVENTS];

    while (true) {
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (num_events < 0) {
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < num_events; i++) {
            int event_fd = events[i].data.fd;

            if (event_fd == server_fd) {
                int client_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen);
                if (client_socket < 0) {
                    perror("accept failed");
                    continue;
                }

                std::cout << "accept new client connection: " << client_socket << std::endl;

                set_nonblocking(client_socket);

                struct epoll_event client_event;
                client_event.events = EPOLLIN | EPOLLET;
                client_event.data.fd = client_socket;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &client_event);
            } else {
                char buffer[BUFFER_SIZE] = {0};
                int bytes_read = read(event_fd, buffer, BUFFER_SIZE);
                
                if (bytes_read > 0) {
                    std::cout << "recved: " << buffer << std::endl;
                    send(event_fd, buffer, bytes_read, 0);
                } else {
                    std::cout << "recved fin tcp packet " << event_fd << std::endl;
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, event_fd, nullptr);
                    close(event_fd);
                }
            }
        }
    }

    close(server_fd);
    close(epoll_fd);
    return 0;
}


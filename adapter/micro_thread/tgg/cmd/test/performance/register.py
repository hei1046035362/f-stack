"""
    模拟注册中心，没有注册中心，网关自动启动时会起不来
"""
import socket

def tcp_server():
    # 创建TCP套接字
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    # 绑定地址和端口
    host = "0.0.0.0"  # 监听所有网络接口
    port = 12345
    server_socket.bind((host, port))

    # 开始监听（设置最大排队连接数）
    server_socket.listen(5)
    print(f"[*] 服务端已启动，正在监听 {host}:{port}")

    try:
        while True:
            # 接受客户端连接
            client_socket, client_addr = server_socket.accept()
            print(f"[+] 客户端连接来自: {client_addr[0]}:{client_addr[1]}")
            message = "hello"
            client_socket.send(message.encode('utf-8'))
            # 持续接收客户端数据
            while True:
                data = client_socket.recv(1024)
                if not data:
                    break  # 连接关闭时退出循环
                message = data.decode('utf-8').strip()
                print(f"[客户端 {client_addr[1]}] 消息内容: {message}")

            # 关闭当前客户端连接
            client_socket.close()
            print(f"[-] 客户端 {client_addr[1]} 连接已关闭")

    except KeyboardInterrupt:
        print("\n[!] 服务端正在关闭...")
    finally:
        server_socket.close()

if __name__ == "__main__":
    tcp_server()

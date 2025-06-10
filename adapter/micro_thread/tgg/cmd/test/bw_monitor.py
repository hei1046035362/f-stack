import socket
import time
import io
import threading
def receive_handler(client_socket):
    """独立线程持续接收服务器消息"""
    while True:
        try:
            data = client_socket.recv(1024)
            if not data:
                print("服务器关闭连接")
                break
            print(f"\n[来自服务器] {data.hex()})") # decode('utf-8')}")
        except (ConnectionResetError, BrokenPipeError):
            print("连接异常中断")
            break
        except Exception as e:
            print(f"接收错误: {str(e)}")
            break

def tcp_client():
    # 创建TCP套接字
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    # 服务器地址和端口
    server_address = ('192.168.40.128', 8021)

    try:
        # 连接服务器
        client_socket.connect(server_address)

        recv_thread = threading.Thread(
            target=receive_handler,
            args=(client_socket,),
            daemon=True  # 设为守护线程
        )
        recv_thread.start()
        # 要发送的数据
        # data_to_send = "Hello, Server!"
        hex_str = "00000045ca00000000000000000000000000000000010000000000007b227365637265745f6b6579223a22222c22776f726b65725f6b6579223a22727573745f73646b227d"
        data_to_send = bytes.fromhex(hex_str)

        # 发送数据到服务器   授权
        client_socket.send(data_to_send)


        # 接收服务器回显的数据
        #received_data = client_socket.recv(1024)

        #print(f"从服务器接收到的数据: {received_data.decode()}")

        while True:
            sendata = input("请输入要发送的数据:")
            print("send:", sendata, "\n")
            data_to_send = bytes.fromhex(sendata)
            client_socket.send(data_to_send)#sendata.encode())

    except Exception as e:
        print(f"发生错误: {e}")
    finally:
        # 关闭套接字
        client_socket.close()

if __name__ == "__main__":
    tcp_client()
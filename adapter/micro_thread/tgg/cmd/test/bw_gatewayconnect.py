"""
    1、主要是模拟bw接收连接，接收来自网关的数据，
        因为网关必须要有一个可用的bw，才能接收客户端连接
    2、也可以模拟bw发送数据给网关
        从pcap中赋值数据包的value直接粘贴到终端，以测试网关对bw数据的解析情况，单条消息测试的时候建议把日志级别调到DEBUG
"""
import socket
import time
import io
import threading
comm_cmd_map = None
tgg_cmd_map = {
    "ReloadIpFilter": "000000316400000000000000000000000000000001010000000000007b22636d64223a3130302c2264617461223a22227d",
    "UpdateRealWorkers": "000000316400000000000000000000000000000001010000000000007b22636d64223a3130312c2264617461223a22227d",
    "PrintAllGids": "000000316400000000000000000000000000000001010000000000007b22636d64223a3131302c2264617461223a22227d",
    "PrintGidCount": "000000316400000000000000000000000000000001010000000000007b22636d64223a3131312c2264617461223a22227d",
    "PrintGidCids": "{len}6400000000000000000000000000000001010000000000007b22636d64223a3131322c2264617461223a22{gid}227d",
    "PrintAllUids": "000000316400000000000000000000000000000001010000000000007b22636d64223a3132302c2264617461223a22227d",
    "PrintUidCount": "000000316400000000000000000000000000000001010000000000007b22636d64223a3132312c2264617461223a22227d",
    "PrintUidCids": "{len}6400000000000000000000000000000001010000000000007b22636d64223a3132322c2264617461223a22{uid}227d",
    "PrintAllCids": "000000316400000000000000000000000000000001010000000000007b22636d64223a3133302c2264617461223a22227d",
    "PrintCidCount": "000000316400000000000000000000000000000001010000000000007b22636d64223a3133312c2264617461223a22227d",
    "PrintAllIdxs": "000000316400000000000000000000000000000001010000000000007b22636d64223a3134302c2264617461223a22227d",
    "PrintIdxCount": "000000316400000000000000000000000000000001010000000000007b22636d64223a3134312c2264617461223a22227d",
    "PrintAllWorkerKeys": "000000316400000000000000000000000000000001010000000000007b22636d64223a3135302c2264617461223a22227d",
    "PrintWorkerKeyCount": "000000316400000000000000000000000000000001010000000000007b22636d64223a3135312c2264617461223a22227d",
    "PrintAllWorkers": "000000316400000000000000000000000000000001010000000000007b22636d64223a3135322c2264617461223a22227d",
    "PrintWorkerCount": "000000316400000000000000000000000000000001010000000000007b22636d64223a3135332c2264617461223a22227d",
    "PrintRealAllWorkers": "000000316400000000000000000000000000000001010000000000007b22636d64223a3135342c2264617461223a22227d",
    "PrintRealWorkerCount": "000000316400000000000000000000000000000001010000000000007b22636d64223a3135352c2264617461223a22227d",
    "CheckGidcidAvaliable": "000000316400000000000000000000000000000001010000000000007b22636d64223a3230302c2264617461223a22227d",
    "CheckUidcidAvaliable": "000000316400000000000000000000000000000001010000000000007b22636d64223a3230312c2264617461223a22227d",
    "CheckCidAvaliable": "000000316400000000000000000000000000000001010000000000007b22636d64223a3230322c2264617461223a22227d"
}

def receive_handler(client_socket):
    """独立线程持续接收服务器消息"""
    while True:
        try:
            data = client_socket.recv(1024)
            if not data:
                print("服务器关闭连接")
                break
            #print(f"\n[来自服务器] {data.hex()})") # decode('utf-8')}")
        except (ConnectionResetError, BrokenPipeError):
            print("连接异常中断")
            break
        except Exception as e:
            print(f"接收错误: {str(e)}")
            break

def print_help(client_socket):
    print("可以输入命令或16进制的字符串")
    print("当前支持的通用命令：")
    for key in comm_cmd_map:
        print(f"\t{key}")
    print("当前支持的TGG命令：")
    for key in tgg_cmd_map:
        print(f"\t{key}")

def exec_all_tggcmd(client_socket):
    for key in tgg_cmd_map:
        print(f"\t{key}")
        if key == "PrintGidCids" or key == "PrintUidCids":
            print(f"skip cmd:{key}")
            continue
        reload_str = tgg_cmd_map.get(key)
        print("data:", reload_str)
        data_to_send = bytes.fromhex(reload_str)
        client_socket.send(data_to_send)

comm_cmd_map = {
    "help": print_help,
    "testall": exec_all_tggcmd,
}

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
        print_help(client_socket)

        while True:
            sendata = input("请输入命令或要发送的数据:")
            if len(sendata) > 0:
                print("input:", sendata)
            else:
                continue
            reload_str = ""
            if sendata in comm_cmd_map:
                comm_cmd_map.get(sendata)(client_socket)
                continue;
            if sendata in tgg_cmd_map:
                reload_str = tgg_cmd_map.get(sendata)
                if sendata == "PrintGidCids":
                    text = input("请输入gid:")
                    gid = text.encode('ascii').hex()
                    print(len(text))
                    gid_len = len(text) + 0x31;
                    print(gid_len)
                    reload_str = reload_str.replace("{gid}", str(gid))
                    reload_str = reload_str.replace("{len}", format(gid_len, '08x'))
                elif sendata == "PrintUidCids":
                    text = input("请输入uid:")
                    uid = text.encode('ascii').hex()
                    uid_len = len(text) + 0x31;
                    reload_str = reload_str.replace("{uid}", str(uid))
                    reload_str = reload_str.replace("{len}", format(uid_len, '08x'))
            else:
                reload_str = sendata
            print("send:", reload_str)
            data_to_send = bytes.fromhex(reload_str)
            client_socket.send(data_to_send)#sendata.encode())

    except Exception as e:
        print(f"发生错误: {e}")
    finally:
        # 关闭套接字
        client_socket.close()

if __name__ == "__main__":
    tcp_client()
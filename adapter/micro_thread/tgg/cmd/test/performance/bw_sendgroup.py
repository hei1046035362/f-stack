"""
    模拟bw批量joingroup和sendgroup
"""
import struct
import time
import socket
import threading
import queue
import logging
from collections import deque

# 高性能日志配置
logging.basicConfig(level=logging.INFO, format='%(asctime)s.%(msecs)03d [%(threadName)s] %(message)s', datefmt='%H:%M:%S')
import ctypes
MAX_PACKET_SIZE = 4096
gid_base = 62205555455823872
gid_count = 1000


# group_cids = dict({gid_base: 0})

# True 同一个群里面 5000个人，一千个人同时发消息   也就是一次性sendgroup一千次
# False 5000个群，每个群一千个人，每个群同时sendgroup一次
distinct_user_in_group = True

# 发送次数  distinct_user_in_group为True才使用
send_times = 1000

class TggBwProtocal(ctypes.Structure):
    # 1字节对齐（等效C的__attribute__((packed))）
    _pack_ = 1  
    _fields_ = [
        ("pack_len", ctypes.c_uint32),     # 数据包总长度（含包头）
        ("cmd", ctypes.c_uint8),           # 命令标识
        ("local_ip", ctypes.c_uint32),     # 网关对内IP
        ("local_port", ctypes.c_uint16),   # 网关对内端口
        ("client_ip", ctypes.c_uint32),     # 客户端IP
        ("client_port", ctypes.c_uint16),  # 客户端端口
        ("connection_id", ctypes.c_uint32),# 客户端CID索引
        ("flag", ctypes.c_uint8),          # 数据格式转换标志
        ("gateway_port", ctypes.c_uint16), # 网关对外端口
        ("ext_len", ctypes.c_uint32),       # 扩展数据长度
        # 柔性数组占位符（数据起始位置）
        ("data", ctypes.c_ubyte * 0)        
    ]
    
    # 获取结构体实例的二进制表示
    def pack(self) -> bytes:
        return bytes(self)

    def pack_network(self) -> bytes:
        """返回网络字节序的二进制数据"""
        # 使用 struct 按网络字节序打包每个字段
        return struct.pack(
            ">I B I H I H I B H I",  # > 表示大端序
            self.pack_len,
            self.cmd,
            self.local_ip,
            self.local_port,
            self.client_ip,
            self.client_port,
            self.connection_id,
            self.flag,
            self.gateway_port,
            self.ext_len
        )

    def create_full_packet(self, payload: bytes) -> bytes:
        """创建完整数据包"""
        self.ext_len = len(payload)
        self.pack_len = ctypes.sizeof(self) + self.ext_len
        return self.pack_network() + payload
    # 从二进制数据解析结构体
    @classmethod
    def unpack(cls, data: bytes):
        return cls.from_buffer_copy(data)
    
    # IP地址转换方法（将32位整数转换为点分十进制）
    @property
    def local_ip_str(self) -> str:
        return socket.inet_ntoa(self.local_ip.to_bytes(4, 'little'))
    
    @local_ip_str.setter
    def local_ip_str(self, value: str):
        self.local_ip = int.from_bytes(socket.inet_aton(value), 'little')

class HighPerfForwarder:
    def __init__(self, server_addr, buffer_size=8192):
        self.server_addr = server_addr
        self.buffer_size = buffer_size
        self.send_queue = queue.Queue(maxsize=1000)  # 发送队列
        self.recv_queue = deque(maxlen=500)          # 接收队列（双端队列）
        self.lock = threading.Lock()
        self.active = True
        self.recv_buffer = bytearray()

    def _packet_handler(self, data_batch):
        """核心包处理逻辑（替代原C代码）"""
        # 合并所有接收批次
        if not data_batch:  
            return []
        valid_data = [d for d in data_batch if isinstance(d, bytes)]
        merged_data = b''.join(valid_data)
        if not merged_data:
            return []

        # 追加到缓冲区
        self.recv_buffer.extend(merged_data)
        processed_data = []

        while len(self.recv_buffer) >= 4:  # 至少要有包头长度
            # 解析包头长度 (网络字节序转主机字节序)
            pack_len = struct.unpack('!I', self.recv_buffer[:4])[0]
            
            # 1. 验证包长度有效性
            if pack_len < ctypes.sizeof(TggBwProtocal) or pack_len > MAX_PACKET_SIZE:
                logging.error(f"Invalid packet len[{pack_len}]")
                logging.error(f"Bin data: {self.recv_buffer[:64].hex()}")
                self.active = False
                return []
            
            # 2. 检查是否收到完整包
            if len(self.recv_buffer) < pack_len:
                break  # 等待更多数据
            
            # 3. 提取完整包数据
            packet_data = bytes(self.recv_buffer[:pack_len])
            
            # # 4. 构建处理结构体 (模拟C的tgg_bw_data)
            # bwdata = TggBwData(
            #     fd=self.sock.fileno(),
            #     coreid=os.getpid(),
            #     bwfdx=(self.sock.fileno() << 8) | os.getpid(),
            #     fd_opt=FD_WRITE,
            #     idx=0,  # 需实现tgg_get_bwfdx_idx
            #     data_len=pack_len,
            #     data=ctypes.addressof(ctypes.c_char_p(packet_data)),
            #     peer_ip=0,  # 实际需获取对端IP
            #     peer_port=0  # 实际需获取对端端口
            # )
            
            # # 5. 处理业务逻辑
            # if tgg_process_bwrcv_data(bwdata) < 0:
            #     logging.error("Packet processing failed")

            parsed_header = TggBwProtocal.unpack(packet_data)
            if parsed_header.cmd == 1:
                # print("new connection:\n")
                # print(parsed_header.data)
                new_header = TggBwProtocal()
                new_header.cmd = 20
                new_header.connection_id = socket.htonl(parsed_header.connection_id)
                new_header.flag = parsed_header.flag
                new_header.ext_len = 17
                new_header.pack_len = ctypes.sizeof(TggBwProtocal) + new_header.ext_len
                # print(new_header.connection_id)
                global gid_base
                if distinct_user_in_group :
                    payload = str(gid_base).encode('utf-8')
                    header_data = new_header.create_full_packet(payload)
                    processed_data.append(header_data)
                else:
                    gid = gid_base
                    for x in range(gid_count):
                        # parsed_header.data = str(gid).encode('utf-8')
                        gid += 1
                        payload = str(gid).encode('utf-8')
                        header_data = new_header.create_full_packet(payload)

                        # processed_packets.append(header_data)
                        processed_data.append(header_data)
            
            # 7. 移除已处理数据
            del self.recv_buffer[:pack_len]
            
            # 8. 处理粘包：检查剩余数据是否包含新包头
            if len(self.recv_buffer) >= 4:
                pack_len = struct.unpack('!I', self.recv_buffer[:4])[0]
            else:
                break
        return b''.join(processed_data)
    def _connect(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)  # 禁用Nagle算法
        self.sock.connect(self.server_addr)
        logging.info(f"Connected to {self.server_addr}")

    def _auth(self):
        auth_data = bytes.fromhex("00000045c800000000000000000000000000000000010000000000007b227365637265745f6b6579223a22222c22776f726b65725f6b6579223a22727573745f73646b227d")  # 您的授权数据
        self.sock.sendall(auth_data)

    def _recv_thread(self):
        """专用接收线程 - 持续接收原始数据"""
        while self.active:
            try:
                data = self.sock.recv(self.buffer_size)
                if not data: 
                    break
                # 零拷贝入队（避免内存复制）
                with self.lock:
                    self.recv_queue.append(data)
            except (socket.timeout, BlockingIOError):
                continue
            except OSError as e:
                logging.error(f"Recv error: {e}")
                break

    def _process_thread(self):
        """专用处理线程 - 流水线处理数据"""
        while self.active:
            try:
                # 批量获取数据（减少锁竞争）
                with self.lock:
                    batch = list(self.recv_queue)
                    self.recv_queue.clear()
                
                if not batch:
                    time.sleep(0.001)
                    continue
                
                # 示例处理逻辑（替换为您的业务逻辑）
                processed = self._custom_process(batch)
                # print(processed.hex())
                # print(processed)
                self.send_queue.put(processed)  # 入发送队列
                
            except Exception as e:
                logging.error(f"Process error: {e}")

    def _send_thread(self):
        """专用发送线程 - 持续发送处理结果"""
        while self.active:
            try:
                data = self.send_queue.get(timeout=0.1)
                self.sock.sendall(data)
            except queue.Empty:
                continue
            except OSError as e:
                logging.error(f"Send error: {e}")
                break

    def _custom_process(self, data_batch):
        """自定义处理逻辑示例（关键！）"""
        # 示例：合并数据包并添加头尾标记

        merged = self._packet_handler(data_batch)
        return merged

    def start(self):
        self._connect()
        self._auth()
        
        # 启动三核心线程
        threading.Thread(target=self._recv_thread, name="Recv", daemon=True).start()
        threading.Thread(target=self._process_thread, name="Process", daemon=True).start()
        threading.Thread(target=self._send_thread, name="Send", daemon=True).start()
        
        # 用户输入线程（独立不阻塞）
        threading.Thread(target=self._user_input, daemon=True).start()
        
        while any(t.is_alive() for t in threading.enumerate()):
            time.sleep(1)
    def _get_sendgroup_frame(self, group):
        gid_hex = str(gid_base).encode('utf-8').hex()
        cmd_data = f"0000017b1600000000000000000000000000000000010000000000397b226578636c756465223a7b223737383234223a37373832347d2c2267726f7570223a5b22{gid_hex}225d7dfffe0000000001260000000300000001000002011d90bd2f044118c617915c79515ef546a9d82fc72dbdc4754aedeccebb5fd999593bb3b7d9504c234a85c61542e13f4048508952a15610c9150a8942a5615cfdfc9e5f9e3c1dadef6e5f8f5e0e66bbfa4cff3c7e3ef3de2e151281f056708426250a9480b2202d0cdff5984122048521304211b2694833c204a71089baa0909291e92157108bca6052612521ac1548254aa45357c613a00c9851c4907183a0e128584b7b1db3e4fbe1fc6263613ec51279af3bb9be848ff1154c6e4ee0ebf4f870713b55aa94eb8e437dbfd9a12acf99f09ad68e0a51d3b8125cd91c95933192a074c888285239cbe8fa83d05ff58370e0876e8064cdebbb1e462b2e09fa6e60e76512ecbffdde3ffdffb1696d597a66ee0f"
        #cmd_data = f"0000008a16000000000000000000000000000000000100000000002e7b226578636c756465223a6e756c6c2c2267726f7570223a5b22{gid_hex}225d7d7b22636d64223a31372c2264617461223a223061313430383830613038306638633363653837353631303830613038306238656161656336363831383031227d"
        return cmd_data
    def _user_input(self):
        """用户输入处理（非阻塞）"""
        global gid_base
        while self.active:
            hex_str = input("输入HEX数据: ").strip()
            if hex_str.lower() == "exit":
                self.active = False
                break
            elif hex_str.lower() == "sendgroup":            
                try:
                    logging.info("sendgroup start")
                    if distinct_user_in_group :
                        global send_times
                        for n in range(send_times):
                            sendata = self._get_sendgroup_frame(gid_base)
                            data = bytes.fromhex(sendata)
                            self.send_queue.put(data)
                        logging.info(f"send times:{send_times} for group:{gid_base}")
                    else :
                        gid_base = 62205555455823872
                        for x in range(gid_count):
                            # parsed_header.data = str(gid_base).encode('utf-8')
                            gid_base += 1
                            sendata = self._get_sendgroup_frame(gid_base)
                            data = bytes.fromhex(sendata)
                            self.send_queue.put(data)
                        logging.info(f"sended for group count:{gid_count}")
                    logging.info("sendgroup end")
                except ValueError:
                    logging.error("Invalid HEX format")
if __name__ == "__main__":
    # 配置服务器地址
    SERVER_ADDR = ('192.168.40.128', 8021)  # 请替换为实际服务器地址
    forwarder = HighPerfForwarder(SERVER_ADDR)
    forwarder.start()
"""
    模拟批量客户端同时发起连接（带消息到达追踪）
"""
import asyncio
import websockets
import logging
import time
import os
from collections import deque
from multiprocessing import Process, cpu_count

# 配置日志
logging.basicConfig(level=logging.INFO, format='%(asctime)s.%(msecs)03d [%(threadName)s] %(message)s', datefmt='%H:%M:%S')
logger = logging.getLogger("websocket_client")

# 每个进程的状态管理
class ProcessMessageTracker:
    def __init__(self):
        self.msg_count = 0
        self.first_msg_time = None
        self.last_msg_time = None
        self.timeout_handle = None
        self.lock = asyncio.Lock()
        self.batch_counter = 0  # 当前批次的消息计数

    async def record_message(self):
        """记录消息到达"""
        current_time = time.time()
        
        async with self.lock:
            # 第一条消息记录
            if self.first_msg_time is None:
                self.first_msg_time = current_time
                logger.info(f"第一条消息到达! 时间: {current_time}")
            
            # 更新最后一条消息时间
            self.last_msg_time = current_time
            self.msg_count += 1
            self.batch_counter += 1
            
            # 每100万条消息记录一次
            if self.msg_count % 1000000 == 0:
                logger.info(f"已收到消息总数: {self.msg_count}")

            # 重置超时检查
            if self.timeout_handle:
                self.timeout_handle.cancel()
            self.timeout_handle = asyncio.create_task(self.check_timeout())

    async def check_timeout(self):
        """检查消息超时"""
        await asyncio.sleep(0.1)  # 100ms等待
        
        async with self.lock:
            current_time = time.time()
            # 100ms内没有新消息到达
            if self.last_msg_time and (current_time - self.last_msg_time) >= 0.1:
                if self.batch_counter > 0:
                    logger.info(f"最后一条消息到达! 时间: {self.last_msg_time}")
                    logger.info(f"本批次收到消息: {self.batch_counter}条")
                    logger.info(f"当前累计消息总数: {self.msg_count}")
                
                # 重置批次计数
                self.batch_counter = 0

async def websocket_client(client_id: int, uri: str, semaphore: asyncio.Semaphore, tracker: ProcessMessageTracker):
    """单个 WebSocket 客户端连接任务"""
    async with semaphore:
        try:
            async with websockets.connect(uri, ping_interval=15) as websocket:
                logger.info(f"Client {client_id} connected")

                # 创建消息接收任务
                receive_task = asyncio.create_task(receive_messages(websocket, client_id, tracker))

                # 心跳任务：每5秒发送PING
                try:
                    while True:
                        # 发送PING信令
                        # await websocket.send("PING")
                        # logger.debug(f"Client {client_id} sent PING")

                        # 等待25秒
                        await asyncio.sleep(25)
                except asyncio.CancelledError:
                    logger.info(f"Client {client_id} heartbeat stopped")
                except websockets.ConnectionClosed:
                    logger.info(f"Client {client_id} connection closed")
                finally:
                    # 取消接收任务
                    receive_task.cancel()
                    try:
                        await receive_task
                    except asyncio.CancelledError:
                        pass
        except Exception as e:
            logger.error(f"Client {client_id} connection failed: {str(e)}")

async def receive_messages(websocket, client_id: int, tracker: ProcessMessageTracker):
    """接收服务器消息的异步任务"""
    try:
        async for message in websocket:
            if message == "PONG":
                logger.debug(f"Client {client_id} received PONG")
            else:
                await tracker.record_message()
    except websockets.ConnectionClosed:
        logger.info(f"Client {client_id} receive connection closed")

async def client_process_worker(start_id: int, end_id: int, uri: str):
    """单个进程的工作函数 (创建异步事件循环)"""
    semaphore = asyncio.Semaphore(7000)  # 每进程并发控制
    tracker = ProcessMessageTracker()  # 进程内消息追踪器
    
    # 启动超时检查任务
    asyncio.create_task(tracker.check_timeout())
    
    tasks = []
    for i in range(start_id, end_id):
        task = asyncio.create_task(websocket_client(i, uri, semaphore, tracker))
        tasks.append(task)
    await asyncio.gather(*tasks)
    
    # 进程结束时报告最终状态
    logger.info(f"进程结束 - 总消息数: {tracker.msg_count}")

def run_process(start_id: int, end_id: int, uri: str):
    """启动进程的同步入口"""
    asyncio.run(client_process_worker(start_id, end_id, uri))

async def main():
    # 配置参数
    SERVER_URI = "ws://192.168.40.129:8058?locale=zh-CN&client_properties=eyJvcyI6ImlvcyIsInZlcnNpb24iOiIxLjAuMCIsImJ1aWxkX251bWJlciI6Ijc2MyIsImRldmljZV9pZCI6IjVEMTc1NEUxLTAzNUMtNDQ1My1BOEJDLUJBN0Q1ODdGNTQ4NyJ9&authorization=a58b6a4695401b1c1522c322dedac4730dc3ab73c9247155e412defb6be6b2ad56f5c117848523f46ecaf7f18dd99c340286425160a77616bf0b1fa3b42cd1235a3e5afc9671f625a5948f4c7c720d89b7f9a60ac83e9d8d6c4f677b5a0ae420a199ea76b55d1a89f829a3bf9328df50&token=a58b6a4695401b1c1522c322dedac4730dc3ab73c9247155e412defb6be6b2ad56f5c117848523f46ecaf7f18dd99c340286425160a77616bf0b1fa3b42cd1235a3e5afc9671f625a5948f4c7c720d89b7f9a60ac83e9d8d6c4f677b5a0ae420a199ea76b55d1a89f829a3bf9328df50"
    TOTAL_CLIENTS = 55000  # 总连接数
    MAX_CONCURRENT = 25000   # 最大并发连接数（根据系统调整）

    num_cores = cpu_count()  # 获取CPU核心数
    clients_per_process = TOTAL_CLIENTS // num_cores

    # 创建并启动多进程
    processes = []
    for i in range(num_cores):
        start_id = i * clients_per_process
        end_id = (i + 1) * clients_per_process
        # 最后一个进程处理剩余客户端
        if i == num_cores - 1:
            end_id = TOTAL_CLIENTS
        
        p = Process(
            target=run_process,
            args=(start_id, end_id, SERVER_URI)
        )
        p.start()
        processes.append(p)
        logger.info(f"启动进程 {i+1}/{num_cores} 处理客户端 {start_id}-{end_id}")
    
    # 等待所有进程完成
    for p in processes:
        p.join()

if __name__ == "__main__":
    asyncio.run(main())
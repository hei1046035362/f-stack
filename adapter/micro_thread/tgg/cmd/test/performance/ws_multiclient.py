"""
    模拟批量客户端同时发起连接
"""
import asyncio
import websockets
import logging
import time
from collections import deque

# 配置日志
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("websocket_client")

async def websocket_client(client_id: int, uri: str, semaphore: asyncio.Semaphore):
    """单个 WebSocket 客户端连接任务"""
    async with semaphore:
        try:
            async with websockets.connect(uri, ping_interval=None) as websocket:
                logger.info(f"Client {client_id} connected") #{uri}")

                # 创建消息接收任务
                receive_task = asyncio.create_task(receive_messages(websocket, client_id))

                # 心跳任务：每5秒发送PING
                try:
                    while True:
                        # 发送PING信令
                        await websocket.send("PING")
                        logger.debug(f"Client {client_id} sent PING")

                        # 等待5秒
                        await asyncio.sleep(5)
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

async def receive_messages(websocket, client_id: int):
    """接收服务器消息的异步任务"""
    try:
        async for message in websocket:
            if message == "PONG":
                logger.debug(f"Client {client_id} received PONG")
            else:
                logger.info(f"Client {client_id} received: {message}")
    except websockets.ConnectionClosed:
        logger.info(f"Client {client_id} receive connection closed")

async def main():
    # 配置参数
    SERVER_URI = "ws://192.168.40.129:8086?locale=zh-CN&client_properties=eyJvcyI6ImlvcyIsInZlcnNpb24iOiIxLjAuMCIsImJ1aWxkX251bWJlciI6Ijc2MyIsImRldmljZV9pZCI6IjVEMTc1NEUxLTAzNUMtNDQ1My1BOEJDLUJBN0Q1ODdGNTQ4NyJ9&authorization=a58b6a4695401b1c1522c322dedac4730dc3ab73c9247155e412defb6be6b2ad56f5c117848523f46ecaf7f18dd99c340286425160a77616bf0b1fa3b42cd1235a3e5afc9671f625a5948f4c7c720d89b7f9a60ac83e9d8d6c4f677b5a0ae420a199ea76b55d1a89f829a3bf9328df50&token=a58b6a4695401b1c1522c322dedac4730dc3ab73c9247155e412defb6be6b2ad56f5c117848523f46ecaf7f18dd99c340286425160a77616bf0b1fa3b42cd1235a3e5afc9671f625a5948f4c7c720d89b7f9a60ac83e9d8d6c4f677b5a0ae420a199ea76b55d1a89f829a3bf9328df50"  # 替换为实际地址
    TOTAL_CLIENTS = 100000  # 总连接数
    MAX_CONCURRENT = 50000   # 最大并发连接数（根据系统调整）

    # 创建信号量控制并发
    semaphore = asyncio.Semaphore(MAX_CONCURRENT)

    # 创建客户端任务
    tasks = []
    for i in range(TOTAL_CLIENTS):
        task = asyncio.create_task(websocket_client(i, SERVER_URI, semaphore))
        tasks.append(task)
        # 小延迟避免瞬间创建过多连接
        if i % 100 == 0:
            await asyncio.sleep(0.1)

    # 等待所有任务完成
    await asyncio.gather(*tasks)

if __name__ == "__main__":
    asyncio.run(main())

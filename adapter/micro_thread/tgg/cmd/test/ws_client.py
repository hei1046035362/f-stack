"""
    模拟单个客户端连接
"""
import websocket
import threading
import time
import binascii

# GatewayWorker的WebSocket服务监听地址
gatewayworker_ws_url = "ws://52.198.225.167:8058?locale=zh-CN&client_properties=eyJvcyI6ImlvcyIsInZlcnNpb24iOiIxLjAuMCIsImJ1aWxkX251bWJlciI6Ijc2MyIsImRldmljZV9pZCI6IjVEMTc1NEUxLTAzNUMtNDQ1My1BOEJDLUJBN0Q1ODdGNTQ4NyJ9&authorization=a58b6a4695401b1c1522c322dedac4730dc3ab73c9247155e412defb6be6b2ad56f5c117848523f46ecaf7f18dd99c340286425160a77616bf0b1fa3b42cd1235a3e5afc9671f625a5948f4c7c720d89b7f9a60ac83e9d8d6c4f677b5a0ae420a199ea76b55d1a89f829a3bf9328df50&token=a58b6a4695401b1c1522c322dedac4730dc3ab73c9247155e412defb6be6b2ad56f5c117848523f46ecaf7f18dd99c340286425160a77616bf0b1fa3b42cd1235a3e5afc9671f625a5948f4c7c720d89b7f9a60ac83e9d8d6c4f677b5a0ae420a199ea76b55d1a89f829a3bf9328df50"
# "ws://192.168.40.129:8086" #"ws://54.177.33.57:9282" #"ws://192.168.40.129:80"
#gatewayworker_ws_url = "ws://192.168.40.129:8058?locale=zh-Hans-CN&client_properties=eyJvcyI6ImFuZHJvaWQiLCJ2ZXJzaW9uIjoiMS4yLjAiLCJidWlsZF9udW1iZXIiOiIyMTMiLCJkZXZpY2VfaWQiOiJlYjJhZjMwN2U1YWY2YjQwIn0%3D&authorization=e19f747a4e71457c1e120b71dc5f2fbd38664213f6ba9876b3c987de55230e63fd0aae7a20d0f613a5f3cc40e20811842fd6daa80d7a66b90ad0ef53c58c07872c0c458759e5bcb5aa3b784c16c28c904956dc291831e2280ff76e7f2af650a3215403da5d527f45ffc118db5d04e81d&token=e19f747a4e71457c1e120b71dc5f2fbd38664213f6ba9876b3c987de55230e63fd0aae7a20d0f613a5f3cc40e20811842fd6daa80d7a66b90ad0ef53c58c07872c0c458759e5bcb5aa3b784c16c28c904956dc291831e2280ff76e7f2af650a3215403da5d527f45ffc118db5d04e81d"
def on_message(ws, message):
    print(f"Received message: {message}")
    if isinstance(message, bytes) and len(message) >= 1:
        if (message[0] & 0x0F) == websocket.ABNF.OPCODE_CLOSE:
            print("收到关闭帧，执行优雅关闭")
            ws.close()
        else:
            print("Got Message:")
            print(message)

def on_error(ws, error):
    print(f"Error: {error}")

def on_close(ws, message, error):
    print("### Closed ###")
    print(message)
    print(error)

def on_open(ws):
    print("### Opened ###")
    # 发送一条测试消息
    i = 0
    # uid = 46233956298788864
#    data = "fffe00000000010500000001000000020001010115c84b6a43310c00c0bb689d8064c9b6945df3e9b25708b22d43689b17f242a184dcbd7496f384c7f21957d881599e98cc427a1b688a2919b14c2bc3849dd22835cd96942c17d53eb8154dd31bb5a2b96215660b57eba263443473c79254a55297947bf131985887b5c96cd9709ab4e414552a21b6dca9a116d5917538a5428d7d8ea8bdf7e08ce1ad4e436fca895ddaac854206a1a75906d2ece686d559a56978d45c6d16219354496003b7fb728bfbe3122bec9eb0acb083cbb2c206d6dff511dfe7afa5fb57c00ee27a3e7cc00646fc5cfa7fecf3f1c4ef87fdb666b6adb0e4ada1d8f6b83f16d997373c5585d7eb0f"
    # uid = 8241110519808
    # data = "fffe0000000000f3000000010000000200010101158ec16e02310c44ff25675672622776b875597aec2fa03871a455298b0055aa10ffde707923bdc3cc3cdd63fbb68bdbbb0e166b6264c69ef49d2db047f3a1b5d2bbf42c41925831c0689ebb8a050e4d2405c05aaa4f49896974348e95a28fc0822d1957cc02841da0732da825378f125408a030b60c902a49f5a2d8099b32b02965cd12450cc70926efb566ad1685380335299984454bcbb525f56ee7aeb7ed6ab7c76a77b77fba6dd0ad833b77ffbb3fece774de6a39dbb076391dbe866ff6bbd6b798e372c4cfc33c71c43c11529cc6469e96794934a70f38b2b8d7eb1f"
    data = "fffe000000000104000000010000000200010101158e416e42310c05ef923548b19d3836bb7ea0cb5e01258e23a1b67c04a85285b87bd3cd48339bf79ee1b17efa25ec826a1e11553d59eb5125222a501aca5d135588cc94bc514d2a0418d986b5c8d5c5a7f54a0e54a5c128a3c0a8c6c3dcc455988d6cf468258b978ed87b8ba26d54c58e8958454d41bc412e5d310f51690d04bc7945892547ec3a1559085911b865a835b654495a4cf3222318e65cc0b228979e067a724a5a5ac905b95bd884eb6dbdfaed71f67bd83dc33a19ce939b70ffbd3ffcfbf4b55afdf259fd72da7fccdefde76cff61c98723bdef976dc9a4db44296f75ee6e0fcb81d3c26ff15824bc5e7f"
    #while i < 3:
    #    i = i + 1
    #    data += "Hello, GatewayWorker!"
    binary_data = binascii.unhexlify(data)
    ws.send(binary_data, opcode=websocket.ABNF.OPCODE_BINARY)

    def heartbeat():
        while True:
            try:
                ws.send("ping", websocket.ABNF.OPCODE_PING)
                time.sleep(10)
            except:
                break
    threading.Thread(target=heartbeat).start()
#    while True:
#        sleep(10)
#    ws.send(binascii.unhexlify("fffe0000000000160000000000000001000101010300"), opcode=websocket.ABNF.OPCODE_BINARY)
    #ws.send("111")

def ws_thread():
    try:
        ws.run_forever()
    except websockets.exceptions.ConnectionClosedError:
        print("WebSocket连接已关闭")



if __name__ == "__main__":
    websocket.enable_window = False
    headers = {"Cache-Control": "no-cache", "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36","nobody":"jussdadfjlasjdflasdjflasdnflasjdflasdfkjlasdjf;lkasdjf;laksdfj;asdkflaksdjf;alskdfjal;skdjflaskdfjlaksdjfasdfkajsdlf;asldkfjalksdjflaksdjflkasjdfkjasdlkfjalskdfj;asldkf"}
    ws = websocket.WebSocketApp(gatewayworker_ws_url,header=headers,
                                on_open=on_open,
                                on_message=on_message,
                                on_error=on_error,
                                on_close=on_close)
    # 创建并启动WebSocket连接线程
    ws_thread_obj = threading.Thread(target=ws_thread)
    ws_thread_obj.start()
    while 1:
        time.sleep(5)
        #ws.send(binascii.unhexlify("fffe0000000000160000000000000001000101010300"), opcode=websocket.ABNF.OPCODE_BINARY)
#    ws.run_forever()
#ifndef __GATEWAY_PROTOCAL_H__
#define __GATEWAY_PROTOCAL_H__

enum GatewayProtocal {
	    // 发给worker，gateway有一个新的连接
    CMD_ON_CONNECT = 1,

    // 发给worker的，客户端有消息
    CMD_ON_MESSAGE = 3,

    // 发给worker上的关闭链接事件
    CMD_ON_CLOSE = 4,

    // 发给gateway的向单个用户发送数据
    CMD_SEND_TO_ONE = 5,

    // 发给gateway的向所有用户发送数据
    CMD_SEND_TO_ALL = 6,

    // 发给gateway的踢出用户
    // 1、如果有待发消息，将在发送完后立即销毁用户连接
    // 2、如果无待发消息，将立即销毁用户连接
    CMD_KICK = 7,

    // 发给gateway的立即销毁用户连接
    CMD_DESTROY = 8,

    // 发给gateway，通知用户session更新
    CMD_UPDATE_SESSION = 9,

    // 获取在线状态
    CMD_GET_ALL_CLIENT_SESSIONS = 10,

    // 判断是否在线
    CMD_IS_ONLINE = 11,

    // client_id绑定到uid
    CMD_BIND_UID = 12,

    // 解绑
    CMD_UNBIND_UID = 13,

    // 向uid发送数据
    CMD_SEND_TO_UID = 14,

    // 根据uid获取绑定的clientid
    CMD_GET_CLIENT_ID_BY_UID = 15,

    // 批量获取uid列表批量获取绑定的clientid
    CMD_BATCH_GET_CLIENT_ID_BY_UID = 16,

    // 加入组
    CMD_JOIN_GROUP = 20,

    // 离开组
    CMD_LEAVE_GROUP = 21,

    // 向组成员发消息
    CMD_SEND_TO_GROUP = 22,

    // 获取组成员
    CMD_GET_CLIENT_SESSIONS_BY_GROUP = 23,

    // 获取组在线连接数
    CMD_GET_CLIENT_COUNT_BY_GROUP = 24,

    // 按照条件查找
    CMD_SELECT = 25,

    // 获取在线的群组ID
    CMD_GET_GROUP_ID_LIST = 26,

    // 取消分组
    CMD_UNGROUP = 27,

    // 批量获取群组ID内客户端个数
    CMD_BATCH_GET_CLIENT_COUNT_BY_GROUP = 28,

    // worker连接gateway事件
    CMD_WORKER_CONNECT = 200,

    // 心跳
    CMD_PING = 201,

    // GatewayClient连接gateway事件
    CMD_GATEWAY_CLIENT_CONNECT = 202,

    // 根据client_id获取session
    CMD_GET_SESSION_BY_CLIENT_ID = 203,

    // 发给gateway，覆盖session
    CMD_SET_SESSION = 204,

    // 当websocket握手时触发，只有websocket协议支持此命令字
    CMD_ON_WEBSOCKET_CONNECT = 205,

    // 包体是标量
    FLAG_BODY_IS_SCALAR = 0x01,

    // 通知gateway在send时不调用协议encode方法，在广播组播时提升性能
    FLAG_NOT_CALL_ENCODE = 0x02

};

enum ClientProtocal {
        // 发给worker，gateway有一个新的连接
    CMD_ON_CLIENT_CONNECT = 1,

    // 发给worker的，客户端有消息
    CMD_ON_CLIENT_MESSAGE = 2,

    // 发给worker上的关闭链接事件
    CMD_ON_CLIENT_CLOSE = 4,

};
#endif // __GATEWAY_PROTOCAL_H__
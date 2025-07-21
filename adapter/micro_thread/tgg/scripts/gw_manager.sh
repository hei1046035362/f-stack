#!/bin/bash
# 进程管理脚本 - 支持多级进程树终止
# 作者：Shell助手（2025-07-17）

# ========== 配置区 ==========
PROCESS_NAME="gwrcv"                 # 主进程名（用于过滤）
START_CMD="/data/code/tgg_gateway/adapter/micro_thread/tgg/gwrcv --proc-id=0"  # 启动命令
PID_FILE="/tmp/gwrcv_master.pid"     # 主进程PID存储文件
LOG_FILE="/var/log/gwrcv_manager.log" # 操作日志
GRACEFUL_TIMEOUT=5                    # 优雅终止等待时间（秒）
FORCE_TIMEOUT=10                      # 强制终止等待时间（秒）

# ========== 函数定义 ==========
log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" | tee -a $LOG_FILE
}

# 查找进程树的所有PID（主进程+子进程+孙子进程）
find_process_tree() {
    local root_pid=$1
    local pids="$root_pid"
    
    # 递归查找子进程
    local children=$(pgrep -P $root_pid)
    for child in $children; do
        pids+=" $(find_process_tree $child)"
    done
    echo $pids
}

# 启动进程
start_process() {
    if [ -f $PID_FILE ]; then
        log "⚠️  进程已在运行（PID: $(cat $PID_FILE)）"
        return 1
    fi
    
    log "🚀 启动进程: $START_CMD"
    $START_CMD &
    master_pid=$!
    echo $master_pid > $PID_FILE
    log "✅ 主进程已启动 | PID: $master_pid"
}

# 停止进程（分阶段终止）
stop_process() {
    # 获取所有 gwrcv 进程的 PID 并排序
    pids=$(pidof gwrcv | tr ' ' '\n' | sort -n)
    
    # 检查是否找到进程
    if [ -z "$pids" ]; then
        echo "未找到 gwrcv 进程，直接启动新进程"
        return 1
    fi
    
    # 提取最小 PID
    min_pid=$(echo "$pids" | head -n 1)
    echo "即将终止最小 PID 进程: $min_pid"
    
    # 终止目标进程
    kill "$min_pid"

    # 等待进程完全退出 (最多等待 10 秒)
    timeout=10
    while kill -0 "$min_pid" 2>/dev/null && [ $timeout -gt 0 ]; do
        sleep 0.5
        ((timeout--))
    done

    # 检查是否超时
    if kill -0 "$min_pid" 2>/dev/null; then
        echo "警告: 进程 $min_pid 未在预期时间内退出"
        # 然后开始逐个kill
    else
        echo "进程 $min_pid 已终止"
        return 0
    fi


    master_pid=$(cat $PID_FILE)
    all_pids=$(find_process_tree $master_pid)
    log "🔍 发现进程树: $all_pids"

    # 阶段1：发送优雅终止信号
    log "⏳ 发送SIGTERM（优雅终止）..."
    kill -TERM $all_pids 2>/dev/null
    
    # 等待优雅退出
    local count=0
    while kill -0 $master_pid 2>/dev/null && [ $count -lt $GRACEFUL_TIMEOUT ]; do
        sleep 1
        ((count++))
    done

    # 阶段2：检查并强制终止
    if kill -0 $master_pid 2>/dev/null; then
        log "⏳ 发送SIGKILL（强制终止）..."
        kill -KILL $all_pids 2>/dev/null
        
        count=0
        while kill -0 $master_pid 2>/dev/null && [ $count -lt $FORCE_TIMEOUT ]; do
            sleep 1
            ((count++))
        done
    fi

    # 最终清理
    if kill -0 $master_pid 2>/dev/null; then
        log "❌ 无法终止进程树！请手动检查"
        return 2
    else
        rm -f $PID_FILE
        log "✅ 进程已终止"
    fi
}

# 重启进程
restart_process() {
    stop_process
    sleep 2  # 确保资源释放
    start_process
}

# ========== 信号处理 ==========
trap 'log "⚠️  脚本被中断！执行清理..."; stop_process; exit 1' SIGINT SIGTERM

# ========== 主逻辑 ==========
case $1 in
    start)
        start_process
        ;;
    stop)
        stop_process
        ;;
    restart)
        restart_process
        ;;
    status)
        if [ -f $PID_FILE ]; then
            master_pid=$(cat $PID_FILE)
            if kill -0 $master_pid 2>/dev/null; then
                log "🟢 进程运行中 | PID: $master_pid"
                log "📜 进程树: $(find_process_tree $master_pid)"
            else
                log "🔴 PID文件存在但进程未运行！清理中..."
                rm -f $PID_FILE
            fi
        else
            log "🔴 进程未运行"
        fi
        ;;
    *)
        echo "用法: $0 {start|stop|restart|status}"
        exit 1
        ;;
esac
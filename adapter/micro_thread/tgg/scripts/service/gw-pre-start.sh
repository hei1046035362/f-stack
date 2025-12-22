#!/bin/bash
# 检查并终止遗留进程（最多等待5秒）

# 定义进程名称列表
PROCESSES=("gwrcv" "gwbwprc" "gwcliprc" "gwregister")

for proc in "${PROCESSES[@]}"; do
    # 查找并杀死进程
    pids=$(pgrep -f "$proc")
    if [ -n "$pids" ]; then
        echo "Found existing $proc processes: $pids"
        
        # 先尝试优雅终止
        kill -TERM $pids
        
        # 等待最多5秒
        counter=0
        while [ $counter -lt 10 ] && pgrep -f "$proc" > /dev/null; do
            sleep 1
            counter=$((counter+1))
        done
        
        # 超时后强制杀死
        if pgrep -f "$proc" > /dev/null; then
            echo "Force killing $proc after 5s timeout"
            kill -9 $(pgrep -f "$proc")
        fi
    fi
done

# 最终确认无遗留进程
for proc in "${PROCESSES[@]}"; do
    if pgrep -f "$proc" > /dev/null; then
        echo "ERROR: Failed to kill all $proc processes!"
        exit 1
    fi
done

# dpdk前置设置  大页内存，驱动加载，网口绑定
/usr/local/tgg_gateway/bin/dpdk_init.sh

exit 0
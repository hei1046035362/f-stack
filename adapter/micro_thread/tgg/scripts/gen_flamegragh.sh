#!/bin/bash

if [ $# -lt 1 ]; then
    echo "用法: $0 <程序名称> [采样时间(秒)]"
    echo "示例: $0 nginx 30"
    exit 1
fi

PROGRAM_NAME=$1
SAMPLING_TIME=${2:-30}  # 默认采样30秒

# 检查FlameGraph工具
FLAMEGRAPH_DIR="/data/code/FlameGraph"
if [ ! -d "$FLAMEGRAPH_DIR" ]; then
    echo "FlameGraph path error..."
    exit 1
fi

# 获取进程ID
PID=$(pgrep -o -x "$PROGRAM_NAME")
if [ -z "$PID" ]; then
    echo "错误: 找不到运行中的程序 '$PROGRAM_NAME'"
    exit 2
fi

if [ "'$PROGRAM_NAME'" = "'nginx'" ];then
    PID=$(ps --ppid $(cat /usr/local/nginx_fstack/logs/nginx.pid 2>/dev/null || pgrep -f "nginx: master") -o pid= | head -1)
fi

# 1. 使用perf记录性能数据[1,3,7](@ref)
echo "步骤1/4: 使用perf采集性能数据..."
sudo perf record -o ${PROGRAM_NAME}_perf.data -F 99 -p $PID -g -- sleep $SAMPLING_TIME

# 2. 转换数据格式[2,4](@ref)
echo "步骤2/4: 转换性能数据格式..."
sudo perf script -i ${PROGRAM_NAME}_perf.data > "${PROGRAM_NAME}_perf.script"

# 3. 折叠调用栈[2,9](@ref)
echo "步骤3/4: 折叠调用栈信息..."
$FLAMEGRAPH_DIR/stackcollapse-perf.pl "${PROGRAM_NAME}_perf.script" > "${PROGRAM_NAME}_perf.folded"

# 4. 生成火焰图[1,6](@ref)
echo "步骤4/4: 生成SVG火焰图..."
$FLAMEGRAPH_DIR/flamegraph.pl "${PROGRAM_NAME}_perf.folded" > "${PROGRAM_NAME}.svg"

# 清理临时文件
rm -f ${PROGRAM_NAME}_perf.data ${PROGRAM_NAME}_perf.script ${PROGRAM_NAME}_perf.folded

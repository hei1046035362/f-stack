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
    echo "FlameGraph路径错误: $FLAMEGRAPH_DIR 不存在"
    exit 1
fi

# 获取所有匹配进程ID
PIDS=$(pgrep -x "$PROGRAM_NAME")
if [ -z "$PIDS" ]; then
    echo "错误: 找不到运行中的程序 '$PROGRAM_NAME'"
    exit 2
fi

# 删除旧的火焰图
rm -f ./*.svg

# 创建临时目录存储数据
TMP_DIR=$(mktemp -d)
echo "创建临时目录: $TMP_DIR"

# 启动所有perf record进程（并行采样）
declare -a PERF_PIDS
for PID in $PIDS; do
    echo "启动采样进程 PID: $PID"
    FILE_PREFIX="${TMP_DIR}/${PROGRAM_NAME}_${PID}"
    
    # 后台运行perf record
    sudo perf record -o ${FILE_PREFIX}.data -F 99 -p $PID -g -- sleep $SAMPLING_TIME &
    PERF_PIDS+=($!)
done

# 等待所有perf record完成
echo "等待所有采样进程完成..."
wait ${PERF_PIDS[@]}

echo "所有采样完成，开始处理数据..."

# 处理每个进程的数据
for PID in $PIDS; do
    FILE_PREFIX="${TMP_DIR}/${PROGRAM_NAME}_${PID}"
    
    # 检查数据文件是否存在
    if [ ! -f "${FILE_PREFIX}.data" ]; then
        echo "警告: 未找到进程 $PID 的数据文件，跳过处理"
        continue
    fi
    
    echo "处理进程 PID: $PID"
    
    # 2. 转换数据格式
    echo "步骤2/4: 转换性能数据格式..."
    sudo perf script -i ${FILE_PREFIX}.data > ${FILE_PREFIX}.script
    
    # 3. 折叠调用栈
    echo "步骤3/4: 折叠调用栈信息..."
    $FLAMEGRAPH_DIR/stackcollapse-perf.pl ${FILE_PREFIX}.script > ${FILE_PREFIX}.folded
    
    # 4. 生成火焰图
    echo "步骤4/4: 生成SVG火焰图..."
    $FLAMEGRAPH_DIR/flamegraph.pl ${FILE_PREFIX}.folded > ${PROGRAM_NAME}_${PID}.svg
    
    echo "已生成火焰图: ${PROGRAM_NAME}_${PID}.svg"
    echo "--------------------------------------"
    
    # 清理临时文件
    rm -f ${FILE_PREFIX}.data ${FILE_PREFIX}.script ${FILE_PREFIX}.folded
done

# 清理临时目录
rmdir $TMP_DIR

echo "所有进程的火焰图生成完成！"
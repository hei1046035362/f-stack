#!/bin/bash
# 启动主进程（根据您的实际路径调整）
nohup /usr/local/tgg_gateway/bin/gwrcv > /var/log/tgg_gateway/gwrcv.log 2>&1 &
echo $! > /var/run/gwrcv.pid  # 保存主进程PID
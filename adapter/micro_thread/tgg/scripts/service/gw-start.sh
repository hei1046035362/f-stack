#!/bin/bash
# 启动主进程（根据您的实际路径调整）
nohup /usr/local/tgg_gateway/bin/gwrcv_reactor | awk '{ print strftime("[%Y-%m-%d %H:%M:%S]"), $0 }' >> /var/log/tgg_gateway/gwrcv_reactor.log 2>&1 &
echo $! > /var/run/gwrcv_reactor.pid  # 保存主进程PID
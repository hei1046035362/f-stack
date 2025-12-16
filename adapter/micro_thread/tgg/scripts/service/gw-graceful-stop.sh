#!/bin/bash
# 向父进程发送SIGTERM
if [ -f /var/run/gwrcv_reactor.pid ]; then
    PID=$(cat /var/run/gwrcv_reactor.pid)
    kill -TERM $PID
else
    echo "PID file not found, trying to find process by name"
    pkill -f "gwrcv_reactor"
fi
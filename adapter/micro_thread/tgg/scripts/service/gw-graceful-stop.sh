#!/bin/bash
# 向父进程发送SIGTERM
if [ -f /var/run/gwrcv.pid ]; then
    PID=$(cat /var/run/gwrcv.pid)
    kill -TERM $PID
else
    echo "PID file not found, trying to find process by name"
    pkill -f "gwrcv"
fi
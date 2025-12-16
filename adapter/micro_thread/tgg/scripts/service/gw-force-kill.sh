#!/bin/bash
# 强制杀死所有相关进程
pkill -9 -f "gwrcv_reactor"
pkill -9 -f "gwrcv_reactor --proc-id"
pkill -9 -f "gwbwprc"
pkill -9 -f "gwcliprc"
pkill -9 -f "gwregister"
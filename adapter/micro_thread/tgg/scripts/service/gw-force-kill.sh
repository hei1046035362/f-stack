#!/bin/bash
# 强制杀死所有相关进程
pkill -9 -f "gwrcv"
pkill -9 -f "gwrcv --proc-id"
pkill -9 -f "gwbwprc"
pkill -9 -f "gwcliprc"
pkill -9 -f "gwregister"
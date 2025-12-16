#! /bin/bash
nohup sh ./gen_flamegragh.sh gwrcv_reactor > /dev/null 2>&1 &
nohup sh ./gen_flamegragh.sh gwcliprc > /dev/null 2>&1 &
nohup sh ./gen_flamegragh.sh gwbwprc > /dev/null 2>&1 &


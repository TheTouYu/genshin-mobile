#!/bin/bash
# 紧急松开：把所有虚拟手指抬起来（映射卡住时用）
cd "$(dirname "$0")" || exit 1
DLL=${1:-mobui18}
LOG=build/$DLL.log
if [ ! -f "$LOG" ]; then echo "找不到 $LOG"; exit 1; fi
echo 'release' >> "build/$DLL.cmds"
for i in $(seq 1 20); do grep -q 'RELEASE:' "$LOG" && { grep 'RELEASE:' "$LOG" | tail -1; exit 0; }; sleep 0.2; done
echo "已发送 release（未见回执，游戏可能没在跑）"

#!/bin/bash
# 通过 vts（虚拟多点触控屏）发手势。用法:
#   ./touch-test.sh tap <x> <y>
#   ./touch-test.sh two <x1> <y1> <x2> <y2>          # 两指同时按下（多点触控判据）
#   ./touch-test.sh swipe <x> <y> <dx> <dy> [steps]  # 单指拖动（相机/摇杆）
#   ./touch-test.sh pinch <x> <y> [d]                 # 双指反向拖动
set -u
F=/home/h/dsh-plugins/.scratch/genshin-mobile/vts/cmd.fifo
[ -p "$F" ] || { echo "vts fifo 不存在（虚拟触控屏没在跑）"; exit 1; }
cmd() { printf '%s\n' "$1" > "$F"; }
case "${1:-}" in
  tap)   x=$2; y=$3; cmd "d 0 100 $x $y"; sleep 0.08; cmd "u 0"; sleep 0.05; cmd "z" ;;
  two)   cmd "d 0 201 $2 $3"; sleep 0.05; cmd "d 1 202 $4 $5"; sleep ${6:-1.2};
         cmd "u 0"; cmd "u 1"; sleep 0.05; cmd "z" ;;
  swipe) x=$2; y=$3; dx=$4; dy=$5; n=${6:-10};
         cmd "d 0 301 $x $y"; sleep 0.05
         i=1; while [ "$i" -le "$n" ]; do
           nx=$((x + dx*i/n)); ny=$((y + dy*i/n)); cmd "m 0 $nx $ny"; sleep 0.02; i=$((i+1))
         done
         cmd "u 0"; sleep 0.05; cmd "z" ;;
  pinch) x=$2; y=$3; d=${4:-300}; n=10;
         cmd "d 0 401 $((x-d)) $y"; cmd "d 1 402 $((x+d)) $y"; sleep 0.1
         i=1; while [ "$i" -le "$n" ]; do
           cmd "m 0 $((x - d + d*i/n)) $y"; cmd "m 1 $((x + d - d*i/n)) $y"; sleep 0.03; i=$((i+1))
         done
         cmd "u 0"; cmd "u 1"; sleep 0.05; cmd "z" ;;
  *) echo "usage: $0 tap|two|swipe|pinch ..."; exit 2 ;;
esac
echo "[touch-test] $* sent"

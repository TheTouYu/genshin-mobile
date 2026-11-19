#!/bin/bash
# 多点触控判定序列（必须在游戏窗口处于前台时跑；由 ys-gui-slot 抬窗口后调用）
set -u
cd /home/h/dsh-plugins/.scratch/genshin-mobile || exit 1
export DISPLAY=:1 XDG_RUNTIME_DIR=/run/user/1000
F=vts/cmd.fifo
SHOT() { timeout 15 spectacle --background --nonotify --output "$1" >/dev/null 2>&1; }
echo "[mt] baseline"; SHOT /tmp/mt-0.png
echo "[mt] A: 单指点小地图"; ./touch-test.sh tap 285 210; sleep 1.2; SHOT /tmp/mt-1-map.png
echo "[mt] A2: 关地图"; ./touch-test.sh tap 2440 85; sleep 1.2; SHOT /tmp/mt-2-closed.png
echo "[mt] C: 重开地图 + 双指捏合（缩放=多点判据）"; ./touch-test.sh tap 285 210; sleep 1.5; SHOT /tmp/mt-3-map2.png
./touch-test.sh pinch 1280 720 300; sleep 1.0; SHOT /tmp/mt-4-pinch.png
echo "[mt] C2: 地图内双指反向捏合（缩小）"; ./touch-test.sh pinch 1280 720 120; sleep 1.0; SHOT /tmp/mt-5-pinch2.png
echo "[mt] D: 摇杆+镜头同时（按住两指）"
printf 'd 0 601 400 1100\n' > $F; sleep 0.1
printf 'd 1 602 1800 700\n' > $F; sleep 0.2
printf 'm 0 420 1000\n' > $F; printf 'm 1 1980 700\n' > $F; sleep 0.8
SHOT /tmp/mt-6-two.png
printf 'm 0 440 960\n' > $F; printf 'm 1 2060 730\n' > $F; sleep 0.8
SHOT /tmp/mt-7-two-later.png
printf 'u 0\n' > $F; printf 'u 1\n' > $F; printf 'z\n' > $F
sleep 0.3; SHOT /tmp/mt-8-after.png
echo "[mt] done"; ls -la /tmp/mt-*.png | awk '{print $5, $9}'

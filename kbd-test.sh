#!/bin/bash
# 键盘移动判定：等游戏窗口成为活动窗 → 按住 W → 截图 → 松开 → 截图
set -u
export DISPLAY=:1 XDG_RUNTIME_DIR=/run/user/1000
SHOT() { timeout 15 spectacle --background --nonotify --output "$1" >/dev/null 2>&1; }
YS=/home/h/genshin-ts/tools/ys-editor/ys-wins
for i in $(seq 1 200); do
  if timeout 10 "$YS" 2>/dev/null | grep '原神' | grep -q 'ACTIVE'; then
    echo "[kbd] game focused at ${i}s"
    SHOT /tmp/kbd-0-before.png
    xdotool keydown w; sleep 1.3; SHOT /tmp/kbd-1-hold.png
    xdotool keyup w; sleep 0.4; SHOT /tmp/kbd-2-after.png
    xdotool keydown w; sleep 2.2; SHOT /tmp/kbd-3-hold2.png; xdotool keyup w
    echo "[kbd] done, shots written"
    exit 0
  fi
  sleep 1
done
echo "[kbd] timeout: 游戏窗口一直不是活动窗"
exit 1

#!/bin/bash
# 等游戏窗口成为活动窗 → 发双指 → 读桥接日志（判定 Unity 是否真的消费了 WM_TOUCH）
set -u
cd /home/h/dsh-plugins/.scratch/genshin-mobile
export DISPLAY=:1 XDG_RUNTIME_DIR=/run/user/1000
YS=/home/h/genshin-ts/tools/ys-editor/ys-wins
LOG=build/mobui11.log
OUT=/tmp/bridge-test.log
: > "$OUT"
for i in $(seq 1 240); do
  if timeout 10 "$YS" 2>/dev/null | grep '原神' | grep -q 'ACTIVE'; then
    echo "[bridge] game focused at ${i}s" | tee -a "$OUT"
    F=vts/cmd.fifo
    printf 'd 0 1001 700 700\n' > $F; sleep 0.25
    printf 'd 1 1002 1700 800\n' > $F; sleep 0.4
    printf 'm 0 720 680\n' > $F; printf 'm 1 1680 820\n' > $F; sleep 0.4
    printf 'u 0\n' > $F; printf 'u 1\n' > $F; printf 'z\n' > $F
    sleep 1.0
    timeout 15 spectacle --background --nonotify --output /tmp/bridge-shot.png >/dev/null 2>&1
    echo "=== BRIDGE log (tail) ===" | tee -a "$OUT"
    grep -E "WM_TOUCH|GetTouchInputInfo|RegisterTouchWindow" "$LOG" | tail -20 | tee -a "$OUT"
    echo "=== counts ===" | tee -a "$OUT"
    echo "wm_touch=$(grep -c 'BRIDGE WM_TOUCH' "$LOG") gti=$(grep -c 'BRIDGE GetTouchInputInfo' "$LOG") rtw=$(grep -c 'BRIDGE RegisterTouchWindow' "$LOG")" | tee -a "$OUT"
    exit 0
  fi
  sleep 1
done
echo "[bridge] timeout: 游戏窗口一直不是活动窗" | tee -a "$OUT"
exit 1

#!/bin/bash
# 晚注入实验：游戏起到登录页（管理单例已初始化）后再注入，缩短"打补丁→反作弊扫到"的暴露窗口
# 用法: ./late-inject.sh <dll-host> <wait-seconds-before-inject> <watch-seconds>
set -u
DLL=${1:?dll}
DELAY=${2:-12}
WATCH=${3:-40}
SLOG=/tmp/late-inject.log
: > "$SLOG"
exec >>"$SLOG" 2>&1

export WINEPREFIX=/home/h/.local/share/anime-game-launcher/prefix
export WINEDEBUG=-all WINEFSYNC=0 WINENTSYNC=0
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 DISPLAY=:1 XDG_SESSION_TYPE=wayland
R=/home/h/.local/share/anime-game-launcher/runners/spritz-wine-tkg-staging-wow64-11.14-1
W=$R/bin/wine
I=/home/h/dsh-plugins/.scratch/genshin-mobile/build/inject.exe

echo "[$(date +%H:%M:%S)] kill session"
"$R/bin/wineserver" -k >/dev/null 2>&1
for i in $(seq 1 30); do ps -eo args | grep -qi '[Y]uanShen.exe' || break; sleep 0.5; done
cd /home/h/.ys || exit 1
echo "[$(date +%H:%M:%S)] launch"
nohup "$W" YuanShen.exe -platform_type CLOUD_THIRD_PARTY_PC >/tmp/late-game.log 2>&1 &
for i in $(seq 1 60); do "$W" "$I" --list 2>/dev/null | grep -qi YuanShen && break; sleep 0.5; done
echo "[$(date +%H:%M:%S)] process up, waiting ${DELAY}s before inject"
sleep "$DELAY"
echo "[$(date +%H:%M:%S)] inject $DLL"
"$W" "$I" --name YuanShen.exe "$DLL" 2>&1
echo "[$(date +%H:%M:%S)] inject exit=$?"
for i in $(seq 1 "$WATCH"); do
  ps -eo args | grep -qi '[Y]uanShen.exe' || { echo "[$(date +%H:%M:%S)] DIED at ${i}s after inject"; break; }
  sleep 1
done
ps -eo args | grep -qi '[Y]uanShen.exe' && echo "[$(date +%H:%M:%S)] SURVIVED ${WATCH}s"
echo "=== game log tail ==="
grep -iE 'NtRaiseHardError|LoadDriver failed' /tmp/late-game.log | tail -2

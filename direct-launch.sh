#!/bin/bash
# 直接起游戏（绕过 launcher GUI），起来后立刻注入探针。全过程写 /tmp/mobui-direct.log
set -u
LOG=/tmp/mobui-direct.log
: > "$LOG"
exec >>"$LOG" 2>&1
echo "[$(date +%H:%M:%S)] direct launch start"
export WINEPREFIX=/home/h/.local/share/anime-game-launcher/prefix
export WINEDEBUG=-all WINEFSYNC=0 WINENTSYNC=0
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 DISPLAY=:1 XDG_SESSION_TYPE=wayland
export __GL_SHADER_DISK_CACHE=1 __GL_SHADER_DISK_CACHE_PATH=/tmp/mobui-nvcache
R=/home/h/.local/share/anime-game-launcher/runners/spritz-wine-tkg-staging-wow64-11.14-1
W=$R/bin/wine
I=/home/h/dsh-plugins/.scratch/genshin-mobile/build/inject.exe
DLL='Z:\home\h\dsh-plugins\.scratch\genshin-mobile\build\mobui_probe.dll'
mkdir -p /tmp/mobui-nvcache
cd /home/h/.ys || exit 1
nohup "$W" YuanShen.exe -platform_type CLOUD_THIRD_PARTY_PC >/tmp/mobui-game-stdout.log 2>&1 &
echo "[$(date +%H:%M:%S)] game wine pid=$!"
injected=0
for i in $(seq 1 240); do
  if "$W" "$I" --list 2>/dev/null | grep -qi 'YuanShen.exe'; then
    echo "[$(date +%H:%M:%S)] YuanShen.exe visible (iter $i)"
    if [ "$injected" = 0 ]; then
      "$W" "$I" --name YuanShen.exe "$DLL" 2>&1
      echo "[$(date +%H:%M:%S)] inject exit=$?"
      injected=1
    fi
    if [ "$i" -ge 8 ]; then exit 0; fi
  fi
  sleep 1
done
echo "[$(date +%H:%M:%S)] TIMEOUT waiting YuanShen.exe"
tail -20 /tmp/mobui-game-stdout.log
exit 1

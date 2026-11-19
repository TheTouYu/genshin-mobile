#!/bin/bash
# 启动原神（PC 入口）+ 立刻注入 mobui_probe.dll，全过程写 /tmp/mobui-launch.log
set -u
LOG=/tmp/mobui-launch.log
: > "$LOG"
exec >>"$LOG" 2>&1
echo "[$(date +%H:%M:%S)] launch start"
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 DISPLAY=:1 XDG_SESSION_TYPE=wayland GTK_THEME=Adwaita
cd /home/h
nohup an-anime-game-launcher --just-run-game >/tmp/aagl-pc.log 2>&1 &
echo "[$(date +%H:%M:%S)] launcher pid=$!"
export WINEPREFIX=/home/h/.local/share/anime-game-launcher/prefix
export WINEDEBUG=-all
W=/home/h/.local/share/anime-game-launcher/runners/spritz-wine-tkg-staging-wow64-11.14-1/bin/wine
I=/home/h/dsh-plugins/.scratch/genshin-mobile/build/inject.exe
DLL='Z:\home\h\dsh-plugins\.scratch\genshin-mobile\build\mobui_probe.dll'
for i in $(seq 1 300); do
  if "$W" "$I" --list 2>/dev/null | grep -qi 'YuanShen.exe'; then
    echo "[$(date +%H:%M:%S)] YuanShen.exe visible at iteration $i"
    sleep 3
    "$W" "$I" --name YuanShen.exe "$DLL" 2>&1
    echo "[$(date +%H:%M:%S)] inject exit=$?"
    exit 0
  fi
  sleep 1
done
echo "[$(date +%H:%M:%S)] TIMEOUT: YuanShen.exe never appeared"
exit 1

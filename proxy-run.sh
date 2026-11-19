#!/bin/bash
# 装代理 → 启动 → 等登录页 → 注入 mobui5 并切移动 UI → 打印代理 STAT 摘要
set -u
cd /home/h/dsh-plugins/.scratch/genshin-mobile || exit 1
PROXY=${1:-build/version_bridge2.dll}
export XDG_RUNTIME_DIR=/run/user/1000 DISPLAY=:1 WAYLAND_DISPLAY=wayland-1 XDG_SESSION_TYPE=wayland
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
GLOG='/home/h/.local/share/anime-game-launcher/prefix/drive_c/users/h/AppData/LocalLow/miHoYo/原神/output_log.txt'
for p in $(pgrep -x YuanShen.exe); do kill "$p"; done
for i in $(seq 1 40); do pgrep -x YuanShen.exe >/dev/null || break; sleep 0.3; done
cp -f "$PROXY" /home/h/.ys/version.dll || { echo "copy proxy failed"; exit 1; }
: > /home/h/.ys/mobui-bridge.log 2>/dev/null || true
U=mobui-px-$(date +%H%M%S)
systemd-run --user --collect --unit=$U --setenv=GTK_THEME=Adwaita --setenv=WINEDLLOVERRIDES=version=n,b \
   /usr/sbin/an-anime-game-launcher --just-run-game >/tmp/px-unit.log 2>&1 || exit 1
echo "[px] unit=$U"
for i in $(seq 1 120); do grep -q 'bridge installed' /home/h/.ys/mobui-bridge.log 2>/dev/null && break; sleep 1; done
grep -E 'attached|installed|RegisterTouchWindow' /home/h/.ys/mobui-bridge.log | tail -3
OFF=$(stat -c %s "$GLOG" 2>/dev/null || echo 0)
for i in $(seq 1 240); do
  tail -c +"$OFF" "$GLOG" 2>/dev/null | grep -q 'LoginMainPageContext AfterOpenDoor' && break
  pgrep -x YuanShen.exe >/dev/null || { echo "[px] game died before login"; exit 3; }
  sleep 1
done
echo "[px] login page reached"
./mobui-switch.sh mobui5.dll 2>&1 | tail -2
echo "[px] proxy STAT lines:"
grep 'STAT ' /home/h/.ys/mobui-bridge.log | tail -4

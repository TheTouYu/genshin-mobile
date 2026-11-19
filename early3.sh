#!/bin/bash
# 预挂注入器（进程内轮询）→ 游戏进程一出现就注入
set -u
cd /home/h/dsh-plugins/.scratch/genshin-mobile || exit 1
DLL=${1:-mobui8.dll}; BASE=${DLL%.dll}
export WINEPREFIX=/home/h/.local/share/anime-game-launcher/prefix WINEDEBUG=-all
export XDG_RUNTIME_DIR=/run/user/1000 DISPLAY=:1 WAYLAND_DISPLAY=wayland-1 XDG_SESSION_TYPE=wayland
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
W=/home/h/.local/share/anime-game-launcher/runners/spritz-wine-tkg-staging-wow64-11.14-1/bin/wine
UNIT=mobui-early-$$
for p in $(pgrep -x YuanShen.exe); do kill "$p"; done
for i in $(seq 1 40); do pgrep -x YuanShen.exe >/dev/null || break; sleep 0.3; done
rm -f build/${BASE}.log build/${BASE}.cmds
# 先挂注入器（后台等待）
( $W build/inject.exe --wait YuanShen.exe "Z:\\home\\h\\dsh-plugins\\.scratch\\genshin-mobile\\build\\${DLL}" >/tmp/early3-inject.log 2>&1 & )
sleep 2
systemctl --user reset-failed "$UNIT.service" 2>/dev/null
systemd-run --user --collect --unit="$UNIT" --setenv=GTK_THEME=Adwaita \
   /usr/sbin/an-anime-game-launcher --just-run-game >/tmp/early3-unit.log 2>&1 || exit 1
for i in $(seq 1 60); do grep -q 'attached' build/${BASE}.log 2>/dev/null && break; sleep 0.5; done
cat /tmp/early3-inject.log | tail -2
grep -E 'attached|spoof installed|BRIDGE ready|BRIDGE hooked|BRIDGE WM_TOUCH' build/${BASE}.log | head -8

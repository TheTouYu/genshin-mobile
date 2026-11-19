#!/bin/bash
# 极早注入：用宿主 pgrep 轮询（毫秒级），进程一出现就注入
set -u
cd /home/h/dsh-plugins/.scratch/genshin-mobile || exit 1
DLL=${1:-mobui8.dll}; BASE=${DLL%.dll}
export WINEPREFIX=/home/h/.local/share/anime-game-launcher/prefix WINEDEBUG=-all
export XDG_RUNTIME_DIR=/run/user/1000 DISPLAY=:1 WAYLAND_DISPLAY=wayland-1 XDG_SESSION_TYPE=wayland
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
W=/home/h/.local/share/anime-game-launcher/runners/spritz-wine-tkg-staging-wow64-11.14-1/bin/wine
I=build/inject.exe
UNIT=mobui-early-$$
for p in $(pgrep -x YuanShen.exe); do kill "$p"; done
for i in $(seq 1 40); do pgrep -x YuanShen.exe >/dev/null || break; sleep 0.3; done
rm -f build/${BASE}.log build/${BASE}.cmds
systemctl --user reset-failed "$UNIT.service" 2>/dev/null
systemd-run --user --collect --unit="$UNIT" --setenv=GTK_THEME=Adwaita \
   /usr/sbin/an-anime-game-launcher --just-run-game >/tmp/early2-unit.log 2>&1 || exit 1
t0=$(date +%s%N)
for i in $(seq 1 6000); do pgrep -f 'YuanShen.exe' >/dev/null && break; sleep 0.02; done
t1=$(date +%s%N)
"$W" "$I" --name YuanShen.exe "Z:\\home\\h\\dsh-plugins\\.scratch\\genshin-mobile\\build\\${DLL}" 2>&1 | tail -1
t2=$(date +%s%N)
echo "[early2] 进程出现=${(( (t1-t0)/1000000 ))}ms 注入完成=${(( (t2-t1)/1000000 ))}ms"
for i in $(seq 1 40); do grep -q 'BRIDGE WM_POINTER' build/${BASE}.log 2>/dev/null && break; sleep 0.3; done
grep -E 'attached|spoof installed|BRIDGE ready|BRIDGE WM_TOUCH' build/${BASE}.log | head -6

#!/bin/bash
# 早注入：从进程出现到注入 ≤1s，让 Unity 初始化输入时就已经看到伪造的 SM_DIGITIZER
set -u
cd /home/h/dsh-plugins/.scratch/genshin-mobile || exit 1
DLL=${1:-mobui6.dll}
BASE=${DLL%.dll}
export WINEPREFIX=/home/h/.local/share/anime-game-launcher/prefix WINEDEBUG=-all
export XDG_RUNTIME_DIR=/run/user/1000 DISPLAY=:1 WAYLAND_DISPLAY=wayland-1 XDG_SESSION_TYPE=wayland
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
W=/home/h/.local/share/anime-game-launcher/runners/spritz-wine-tkg-staging-wow64-11.14-1/bin/wine
I=build/inject.exe
UNIT=mobui-launcher-early-$$

for p in $(pgrep -x YuanShen.exe); do kill "$p"; done
for i in $(seq 1 30); do pgrep -x YuanShen.exe >/dev/null || break; sleep 0.5; done
rm -f build/${BASE}.log build/${BASE}.cmds
systemctl --user reset-failed "$UNIT.service" 2>/dev/null
systemd-run --user --collect --unit="$UNIT" --setenv=GTK_THEME=Adwaita \
   /usr/sbin/an-anime-game-launcher --just-run-game >/tmp/early-unit.log 2>&1 || { echo "systemd-run failed"; exit 1; }
t0=$(date +%s%N)
for i in $(seq 1 600); do
  if "$W" "$I" --list 2>/dev/null | grep -qi YuanShen; then break; fi
  sleep 0.1
done
t1=$(date +%s%N)
"$W" "$I" --name YuanShen.exe "Z:\\home\\h\\dsh-plugins\\.scratch\\genshin-mobile\\build\\${DLL}" 2>&1 | tail -1
echo "[early] 进程出现到注入完成: $(( (t1 - t0) / 1000000 )) ms (注入前)"
for i in $(seq 1 30); do grep -q 'spoof installed\|spoof FAILED' build/${BASE}.log 2>/dev/null && break; sleep 0.3; done
grep -E 'attached|spoof|trampoline' build/${BASE}.log | head -5

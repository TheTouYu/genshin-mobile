#!/bin/bash
# 实验台：重启游戏 → 注入指定 DLL → 观察 N 秒存活状态 → 汇总
# 用法: ./probe-run.sh <dll-host-path> <watch-seconds> [extra-game-args...]
set -u
DLL_HOST=${1:?dll}
WATCH=${2:-30}
shift 2 || true
EXTRA="$*"
GLOG=/tmp/probe-run-game.log
SLOG=/tmp/probe-run-status.log
: > "$SLOG"
exec >>"$SLOG" 2>&1

export WINEPREFIX=/home/h/.local/share/anime-game-launcher/prefix
export WINEDEBUG=${WINEDEBUG:-err+seh}
export WINEFSYNC=0 WINENTSYNC=0
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 DISPLAY=:1 XDG_SESSION_TYPE=wayland
R=/home/h/.local/share/anime-game-launcher/runners/spritz-wine-tkg-staging-wow64-11.14-1
W=$R/bin/wine
I=/home/h/dsh-plugins/.scratch/genshin-mobile/build/inject.exe

echo "[$(date +%H:%M:%S)] stop existing wine session"
"$R/bin/wineserver" -k >/dev/null 2>&1
for i in $(seq 1 30); do ps -eo args | grep -qi '[Y]uanShen.exe' || break; sleep 0.5; done

echo "[$(date +%H:%M:%S)] launch YuanShen.exe $EXTRA"
cd /home/h/.ys || exit 1
nohup "$W" YuanShen.exe -platform_type CLOUD_THIRD_PARTY_PC $EXTRA >"$GLOG" 2>&1 &
game_pid=$!

for i in $(seq 1 60); do
  "$W" "$I" --list 2>/dev/null | grep -qi 'YuanShen.exe' && break
  sleep 0.5
done
echo "[$(date +%H:%M:%S)] inject $DLL_HOST"
"$W" "$I" --name YuanShen.exe "$DLL_HOST" 2>&1
echo "[$(date +%H:%M:%S)] inject exit=$?"

alive=0
for i in $(seq 1 "$WATCH"); do
  if ps -eo args | grep -qi '[Y]uanShen.exe'; then alive=$i; else echo "[$(date +%H:%M:%S)] DIED after ${i}s"; break; fi
  sleep 1
done
if [ "$alive" != 0 ] && ps -eo args | grep -qi '[Y]uanShen.exe'; then
  echo "[$(date +%H:%M:%S)] SURVIVED ${WATCH}s"
fi
echo "=== game stdout/stderr tail ==="
tail -25 "$GLOG"

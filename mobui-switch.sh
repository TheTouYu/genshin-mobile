#!/bin/bash
# 把正在运行的原神切到移动端 UI（幂等：探针没在就注入，等登录页就位后再切）
set -u
cd /home/h/dsh-plugins/.scratch/genshin-mobile || exit 1
DLL=${1:-mobui27.dll}
BASE=${DLL%.dll}
export WINEPREFIX=/home/h/.local/share/anime-game-launcher/prefix WINEDEBUG=-all
export XDG_RUNTIME_DIR=/run/user/1000 DISPLAY=:1 WAYLAND_DISPLAY=wayland-1 XDG_SESSION_TYPE=wayland
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
W=/home/h/.local/share/anime-game-launcher/runners/spritz-wine-tkg-staging-wow64-11.14-1/bin/wine
LOG=build/$BASE.log
CMDS=build/$BASE.cmds
GLOG='/home/h/.local/share/anime-game-launcher/prefix/drive_c/users/h/AppData/LocalLow/miHoYo/原神/output_log.txt'
[ -f build/$BASE.cfg ] || printf 'hooks=0x50\nveh=0\n' > build/$BASE.cfg

pgrep -x YuanShen.exe >/dev/null || { echo "[switch] 游戏没在运行；先跑 ./mobui-play.sh"; exit 3; }
GPID=$(pgrep -x YuanShen.exe | head -1)
ETIMES=$(ps -o etimes= -p "$GPID" 2>/dev/null | tr -d ' ')
GSTART=$(( $(date +%s) - ${ETIMES:-0} ))

# 先等登录页就位再注入（早注入会被反作弊杀：实测注入后 0.3s 进程消失）
wait_login_page() {
  for i in $(seq 1 300); do
    ok2=0; idle=0
    grep -q 'LoginMainPageContext AfterOpenDoor' "$GLOG" 2>/dev/null && ok2=1
    if [ -f "$GLOG" ]; then now=$(date +%s); mt=$(stat -c %Y "$GLOG"); [ $(( now - mt )) -ge 3 ] && idle=1; fi
    [ "$ok2" = 1 ] && [ "$idle" = 1 ] && return 0
    pgrep -x YuanShen.exe >/dev/null || return 3
    sleep 0.5
  done
  return 4
}
wait_login_page || { echo "[switch] 游戏没到登录页就退出了（或超时）"; exit 3; }

# 探针是否已在本进程里（看日志 mtime 是否晚于进程启动）
LOADED=0
if [ -f "$LOG" ]; then
  LMT=$(stat -c %Y "$LOG")
  [ "$LMT" -ge "$GSTART" ] && grep -q "platform-getter trampoline installed" "$LOG" && LOADED=1
fi
if [ "$LOADED" -eq 0 ]; then
  rm -f "$LOG" "$CMDS"
  "$W" build/inject.exe --name YuanShen.exe "Z:\\home\\h\\dsh-plugins\\.scratch\\genshin-mobile\\build\\$DLL" 2>&1 | tail -1
  for i in $(seq 1 40); do grep -q 'trampoline installed' "$LOG" 2>/dev/null && break; sleep 0.5; done
  grep -q 'trampoline installed' "$LOG" 2>/dev/null || { echo "[switch] 注入失败（游戏侧没装上跳板）"; exit 4; }
fi

# 门：il2cpp 单例就绪 + 登录页已就位（整份日志找，不只看新增）+ 日志静默 3s（游戏空闲在登录页）
wait_ready() {
  for i in $(seq 1 240); do
    ok1=0; ok2=0; idle=0
    grep -q 'slots 9E60=0000' "$LOG" 2>/dev/null && ok1=1
    grep -q 'LoginMainPageContext AfterOpenDoor' "$GLOG" 2>/dev/null && ok2=1
    if [ -f "$GLOG" ]; then
      now=$(date +%s); mt=$(stat -c %Y "$GLOG"); [ $(( now - mt )) -ge 3 ] && idle=1
    fi
    [ "$ok1" = 1 ] && [ "$ok2" = 1 ] && [ "$idle" = 1 ] && return 0
    pgrep -x YuanShen.exe >/dev/null || return 3
    sleep 0.5
  done
  return 4
}
wait_ready || { echo "[switch] 等不到登录页空闲状态，放弃（不硬发）"; exit 4; }

BEFORE=$(grep -c 'RESULT touch_null' "$LOG" 2>/dev/null || echo 0)
for attempt in 1 2 3 4; do
  BEFORE=$(grep -c 'RESULT touch_null' "$LOG" 2>/dev/null | head -1)
  echo 'touch_null' >> "$CMDS"
  for i in $(seq 1 40); do
    NOW=$(grep -c 'RESULT touch_null' "$LOG" 2>/dev/null | head -1)
    [ "${NOW:-0}" -gt "${BEFORE:-0}" ] && break
    pgrep -x YuanShen.exe >/dev/null || { echo "[switch] 切换时游戏退出"; exit 3; }
    sleep 0.5
  done
  MODE=$(grep 'STATE slots' "$LOG" | tail -1 | grep -oE 'mode9EC0=[0-9]+' | cut -d= -f2)
  echo "[switch] 第 $attempt 次: $(grep 'RESULT touch_null' "$LOG" | tail -1 | cut -c1-70) | mode9EC0=${MODE:-?}"
  if [ "${MODE:-2}" = "0" ]; then echo "[switch] ✅ 已切到触摸模式 (mode9EC0=0)"; exit 0; fi
  sleep 3
done
pgrep -x YuanShen.exe >/dev/null || { echo "[switch] 进程已退出 ❌"; exit 5; }
echo "[switch] ⚠️ 仍未切到触摸模式（mode9EC0≠0）：调用发生了但没生效"

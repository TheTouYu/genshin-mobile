#!/bin/bash
# 一键：起原神（独立 systemd unit，dsh 重启不连坐）→ 等登录页 → 注入探针 → 切移动端 UI
set -u
exec > >(tee -a /tmp/mobui-play.log) 2>&1
cd /home/h/dsh-plugins/.scratch/genshin-mobile || exit 1
DLL=${1:-mobui27.dll}
export XDG_RUNTIME_DIR=/run/user/1000 DISPLAY=:1 WAYLAND_DISPLAY=wayland-1 XDG_SESSION_TYPE=wayland
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus

# 触摸守护：手机模式全靠它把平板触摸读成 UDP（v2 协议 + 250ms 心跳）。
# 它是 transient unit，重启/注销后不会自启，所以每次起游戏都幂等拉起来。
if ! pgrep -x touchd >/dev/null; then
  echo "[play] touchd 没在跑，拉起（v2 协议，端口 51721）"
  systemd-run --user --collect --unit=mobui-touchd \
    --working-directory=/home/h/dsh-plugins/.scratch/genshin-mobile/tools \
    /home/h/dsh-plugins/.scratch/genshin-mobile/tools/touchd auto 51721 1 \
    || echo "[play] ⚠️ touchd 拉起失败：触摸会没反应（游戏仍可用鼠标/键盘）"
else
  echo "[play] touchd 已在跑 pid=$(pgrep -x touchd | head -1)"
fi

if pgrep -x YuanShen.exe >/dev/null; then
  echo "[play] 已有实例在跑，直接切 UI"
else
  U=mobui-launcher-$(date +%H%M%S)
  systemd-run --user --collect --property=KillMode=process --unit=$U --setenv=GTK_THEME=Adwaita \
     /usr/sbin/an-anime-game-launcher --just-run-game >/tmp/mobui-unit.log 2>&1 \
     || { echo "[play] 启动失败"; tail -3 /tmp/mobui-unit.log; exit 1; }
  echo "[play] 已启动（unit=$U），等进程..."
  for i in $(seq 1 240); do pgrep -x YuanShen.exe >/dev/null && break; sleep 0.5; done
  pgrep -x YuanShen.exe >/dev/null || { echo "[play] 游戏没起来"; exit 1; }
  G=$(pgrep -x YuanShen.exe | head -1)
  echo "[play] 进程 pid=$G cgroup=$(cat /proc/$G/cgroup 2>/dev/null | cut -d: -f3)"
fi
exec ./mobui-switch.sh "$DLL"

#!/bin/bash
# 运行时调触摸灵敏度增益，不用重启游戏。
# 用法: ./mobui-gain.sh <5..400>      # 写进"当前生效"的探针 cmds
#       ./mobui-gain.sh --show        # 只看当前值和写入目标，不写
#
# 为什么单独有这条：gain 需要 shell 重定向，而 cmds 文件名随探针版本漂移（mobui27/29/30…），
# 在配置里写死 build/mobui29.cmds，一旦注入的是别的版本就静默失效。这里统一认领。
set -u
cd /home/h/dsh-plugins/.scratch/genshin-mobile || exit 1

# 认领正在被注入的探针：显式 MOBUI_DLL 优先，否则取最近写过的 build/mobui<数字>.log
if [ -z "${MOBUI_DLL:-}" ]; then
  auto=$(ls -t build/mobui*.log 2>/dev/null | grep -E '/mobui[0-9]+\.log$' | head -1)
  [ -n "$auto" ] && MOBUI_DLL=$(basename "$auto" .log).dll
fi
DLL=${MOBUI_DLL:-mobui30.dll}
BASE=${DLL%.dll}
CMDS=build/$BASE.cmds
LOG=build/$BASE.log

CFG=build/$BASE.cfg
c=$({ [ -f "$CFG" ] && grep -oE '^lookgain=[0-9]+' "$CFG"; } | tail -1 | cut -d= -f2)

if [ "${1:-}" = "--show" ] || [ $# -eq 0 ]; then
  echo "探针: $DLL"
  echo "命令文件: $CMDS $([ -f "$CMDS" ] && echo "(已存在)" || echo "(尚未创建)")"
  echo "日志: $LOG $([ -f "$LOG" ] && echo "(mtime $(stat -c %y "$LOG" | cut -c12-19))" || echo "(无)")"
  echo "cfg 烘焙值: ${c:-未知}"
  echo "最近写入的 gain 命令: $(grep -o 'gain [0-9]*' "$CMDS" 2>/dev/null | tail -1 || echo 无)"
  pgrep -x YuanShen.exe >/dev/null && echo "游戏: 在跑" || echo "游戏: 没跑（写入也要等下次注入生效）"
  [ $# -eq 0 ] && { echo; echo "用法: $0 <5..400> | --show"; exit 2; }
  exit 0
fi

gv=${1:-}
case "$gv" in ''|*[!0-9]*) echo "增益必须是整数 5..400，收到: '$gv'" >&2; exit 2;; esac
if [ "$gv" -lt 5 ] || [ "$gv" -gt 400 ]; then echo "增益超出 5..400: $gv" >&2; exit 2; fi

printf 'gain %s\n' "$gv" >> "$CMDS"
echo "→ 已写入 $CMDS: gain $gv（探针约 1s 内读一次，下一条触摸生效）"
pgrep -x YuanShen.exe >/dev/null || { echo "⚠️ 游戏没在跑：命令已写入，下次注入才生效"; exit 3; }

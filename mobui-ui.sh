#!/bin/bash
# 实时切换原神输入 UI：touch=移动端 UI，pc=PC UI，status=看当前模式
# 原理：往 <dll>.cmds 追加一行命令，探针每 ~1s 读一次（不重启游戏、不重注入）
set -u
cd /home/h/dsh-plugins/.scratch/genshin-mobile || exit 1
# 认领"正在被注入的那个探针"：显式 MOBUI_DLL 优先，否则取最近写过的 build/mobui<数字>.log
# 教训：DLL 版本会漂移（27→29→30），默认值写死会让 touch/pc 静默写进没人读的 cmds，
#       让 status 读过期日志报出错误模式（2026-11-19 实测：注入 mobui29 却报了 mobui27 旧日志的 mode9EC0=2）
if [ -n "${MOBUI_DLL:-}" ]; then
  DLL=$MOBUI_DLL
else
  auto=$(ls -t build/mobui*.log 2>/dev/null | grep -E '/mobui[0-9]+\.log$' | head -1)
  DLL=${auto:+$(basename "$auto" .log).dll}
  DLL=${DLL:-mobui30.dll}
fi
BASE=${DLL%.dll}
CMDS=build/$BASE.cmds
LOG=build/$BASE.log
mode() { grep "STATE slots" "$LOG" 2>/dev/null | tail -1 | grep -oE "mode9EC0=[0-9]+" | cut -d= -f2; }
case "${1:-status}" in
  touch) m=$(mode); if [ "${m:-x}" = 0 ]; then echo "已经是移动端 UI，不用再切（重复调用会在游戏里抛访问违例）"; exit 0; fi
         printf 'touch_null\n' >> "$CMDS"; echo "→ 已请求切到移动端 UI（触摸）";;
  pc)    m=$(mode); if [ "${m:-x}" = 2 ]; then echo "已经是 PC UI，不用再切"; exit 0; fi
         printf 'uipc\n'       >> "$CMDS"; echo "→ 已请求切到 PC UI（需要 mobui28 及以上）";;
  status) m=$(mode); case "${m:-?}" in 0) echo "当前：移动端 UI（mode9EC0=0）";; 2) echo "当前：PC UI（mode9EC0=2）";; *) echo "当前：未知（mode9EC0=${m:-无日志}）";; esac;;
  *) echo "用法: $0 touch|pc|status（DLL 用 MOBUI_DLL 覆盖，默认 $DLL）"; exit 2;;
esac
pgrep -x YuanShen.exe >/dev/null || { echo "⚠️ 游戏没在跑：命令已写入，下次注入后才生效"; exit 3; }

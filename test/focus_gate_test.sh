#!/usr/bin/env bash
# focus gate 配置/匹配回归（TDD 红先绿后）
# 背景：2026-11-26 实测——门闩默认开启时，用户"切到游戏测试再切回来"，活动窗口是浏览器，
# touchd 判定失焦 → 空包抬起全部接触点 → 触摸完全没进游戏（捏合/边走边转都表现为"坏"）。
# 期望：默认关闭；显式 FOCUS_GATE=1 + FOCUS_CLASS=... 才打开。
set -uo pipefail
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
TD="$ROOT/tools/touchd"
pass=0; fail=0

ck() { # ck <名字> <期望> <实际>
  if [[ "$2" == "$3" ]]; then printf 'PASS %s（%s）\n' "$1" "$3"; pass=$((pass+1));
  else printf 'FAIL %s — 期望 %s，实得 %s\n' "$1" "$2" "$3"; fail=$((fail+1)); fi
}

run_cfg() { timeout 3 env "$@" "$TD" --gate-config 2>/dev/null | head -1; }
run_match() { local cls="$1" json="$2"; timeout 3 env FOCUS_CLASS="$cls" "$TD" --gate-match "$json" 2>/dev/null | head -1; }

ck "default.off"  "gate=off" "$(timeout 3 env -u FOCUS_CLASS -u FOCUS_GATE "$TD" --gate-config 2>/dev/null | head -1)"
ck "empty.off"    "gate=off" "$(run_cfg FOCUS_CLASS= FOCUS_GATE=1)"
ck "optin.on"     "gate=on class=yuanshen.exe" "$(run_cfg FOCUS_CLASS=yuanshen.exe FOCUS_GATE=1)"
ck "match.hit"    "match=1" "$(run_match yuanshen.exe '{"class":"yuanshen.exe"}')"
ck "match.miss"   "match=0" "$(run_match yuanshen.exe '{"class":"firefox"}')"

printf 'SUMMARY PASS=%d FAIL=%d\n' "$pass" "$fail"
[[ $fail -eq 0 ]]

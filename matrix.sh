#!/bin/bash
cd /home/h/dsh-plugins/.scratch/genshin-mobile
for mask in 1 2 4 8; do
  printf "hooks=$mask\nveh=0\n" > build/mobui_probe.cfg
  rm -f build/mobui_probe.log
  ./probe-run.sh 'Z:\home\h\dsh-plugins\.scratch\genshin-mobile\build\mobui_probe.dll' 15 >/dev/null 2>&1
  verdict=$(grep -E 'SURVIVED|DIED' /tmp/probe-run-status.log | tail -1)
  echo "mask=$mask :: $verdict"
  cp build/mobui_probe.log logs/mask-$mask.log 2>/dev/null
  cp /tmp/probe-run-game.log logs/mask-$mask-game.log 2>/dev/null
done
echo MATRIX-DONE

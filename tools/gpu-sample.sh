#!/bin/bash
# 1Hz 采 GPU 时钟/功耗/利用率 + 游戏主线程 CPU，用来判断"移动时掉帧"是 GPU 降频还是别的原因
# 用法: bash tools/gpu-sample.sh <秒数> [csv]
DUR=${1:-60}; OUT=${2:-/tmp/gpu-sample.csv}
P=$(pgrep -x YuanShen.exe | head -1)
[ -z "$P" ] && { echo "游戏没在跑"; exit 1; }
echo "t,gpu_util,enc_util,clk_mhz,clk_max,power_w,pstate,temp,mem_util,game_pct" > "$OUT"
prev=$(awk '{print $14+$15}' /proc/$P/stat); t0=$(date +%s)
while [ $(( $(date +%s) - t0 )) -lt "$DUR" ]; do
  read -r u e c cm p ps tm mu <<<"$(nvidia-smi --query-gpu=utilization.gpu,utilization.encoder,clocks.current.graphics,clocks.max.graphics,power.draw,pstate,temperature.gpu,utilization.memory --format=csv,noheader,nounits | tr -d ',')"
  now=$(awk '{print $14+$15}' /proc/$P/stat)
  gp=$(awk -v a=$prev -v b=$now -v h=$(getconf CLK_TCK) 'BEGIN{printf "%.0f",(b-a)/h*100}')
  prev=$now
  echo "$(($(date +%s)-t0)),$u,$e,$c,$cm,$p,$ps,$tm,$mu,$gp" >> "$OUT"
  sleep 1
done
echo "done → $OUT ($(wc -l < "$OUT") 行)"

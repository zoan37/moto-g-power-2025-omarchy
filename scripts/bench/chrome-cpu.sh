#!/bin/bash
# chrome-cpu.sh SECONDS: CPU (cores) of the --user-data-dir=/tmp/chrome-va Chrome test
# instance per process type, plus the whole system (8 cores), over SECONDS.
secs=${1:-10}
root=$(pgrep -of -- '--user-data-dir=/tmp/chrome-va' )
tree() { echo $1; for c in $(pgrep -P $1); do tree $c; done; }
pids=$(tree $root)
declare -A t0 typ
hz=$(getconf CLK_TCK)
for p in $pids; do
  read -r -a st < /proc/$p/stat 2>/dev/null || continue
  t0[$p]=$((st[13] + st[14]))
  ty=$(tr '\0' ' ' < /proc/$p/cmdline | grep -o -- '--type=[a-z-]*' | head -1 | cut -d= -f2); typ[$p]=${ty:-browser}
done
sys0=$(awk '/^cpu /{print $2+$3+$4+$6+$7+$8}' /proc/stat)
sleep $secs
sys1=$(awk '/^cpu /{print $2+$3+$4+$6+$7+$8}' /proc/stat)
declare -A sum
for p in "${!t0[@]}"; do
  read -r -a st < /proc/$p/stat 2>/dev/null || continue
  d=$((st[13] + st[14] - t0[$p])); sum[${typ[$p]}]=$(( ${sum[${typ[$p]}]:-0} + d ))
done
tot=0; out=""
for k in "${!sum[@]}"; do tot=$((tot + sum[$k])); out+="$k=$(awk -v d=${sum[$k]} -v s=$secs -v h=$hz 'BEGIN{printf "%.2f", d/h/s}') "; done
echo "chrome-va cores: $out total=$(awk -v d=$tot -v s=$secs -v h=$hz 'BEGIN{printf "%.2f", d/h/s}') system=$(awk -v d=$((sys1-sys0)) -v s=$secs -v h=$hz 'BEGIN{printf "%.2f", d/h/s}')/8"

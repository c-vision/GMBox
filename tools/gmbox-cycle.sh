#!/bin/sh
# Start/sign-in/shutdown stability cycles for the native Windows VMM.
#   GMBOX_TEST_PASSWORD=... tools/gmbox-cycle.sh <cycles> <disk.raw> <vars.fd> [workdir]
# Use COPIES of the disk and VARS. Per cycle it logs: time to lock screen, time to desktop,
# time to VM exit after an in-guest shutdown, and the process exit code.
set -u
N=${1:?cycles}; DISK=${2:?disk}; VARS=${3:?vars}; W=${4:-/tmp/gmbox-cycle}
PW=${GMBOX_TEST_PASSWORD:?set GMBOX_TEST_PASSWORD}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$W"; cc -O2 -o "$W/fbstat" "$ROOT/tools/fbstat.c" || exit 1; make -C "$ROOT" hvf-win >/dev/null || exit 1
now() { perl -MTime::HiRes=time -e 'printf "%.1f",time'; }
el() { perl -e "printf '%.1f', $(now)-$1"; }
fifo="$W/in.fifo"; log="$W/cycles.log"; : > "$log"
fails=0
for i in $(seq 1 "$N"); do
  rm -f "$fifo" "$W/screen.bmp"; mkfifo "$fifo"
  t0=$(now)
  ( exec 3<>"$fifo"; cd "$ROOT"; GMBOX_INPUT="$fifo" GMBOX_AUTOENTER=1 GMBOX_FB_DUMP="$W/screen.bmp" GMBOX_FB_INTERVAL=1 \
    GMBOX_DISK="$DISK" GMBOX_VARS="$VARS" GMBOX_WATCHDOG_SEC=900 build/hvf_win > "$W/vm$i.log" 2>&1 < /dev/null ) &
  pid=$!
  tl=""; td=""; res=ok
  while :; do
    [ -f "$W/screen.bmp" ] && st=$("$W/fbstat" "$W/screen.bmp" 2>/dev/null) || st="0 0"
    cw=${st% *}; lum=${st#* }
    if [ -z "$tl" ] && [ "$(echo "$cw > 0.2" | bc)" = 1 ] && [ "$lum" -lt 100 ]; then tl=$(el "$t0"); break; fi
    [ "$(perl -e "print $(el "$t0")>150?1:0")" = 1 ] && { res=timeout-lock; break; }
    sleep 0.5
  done
  if [ "$res" = ok ]; then
    sleep 3; echo "key space" > "$fifo"; sleep 4
    printf 'type %s\nwait 300\nkey enter\n' "$PW" > "$fifo"
    while :; do
      st=$("$W/fbstat" "$W/screen.bmp" 2>/dev/null) || st="0 0"; lum=${st#* }
      [ "$lum" -gt 200 ] && { td=$(el "$t0"); break; }
      [ "$(perl -e "print $(el "$t0")>150?1:0")" = 1 ] && { res=timeout-desktop; break; }
      sleep 0.5
    done
  fi
  ts=""; rc=-1
  if [ "$res" = ok ]; then
    sleep 5
    [ "$i" = 1 ] && { printf 'key win+r\nwait 1500\ntype cmd /c powercfg /h off\nwait 300\nkey enter\nwait 4000\n' > "$fifo"; sleep 8; }
    ts0=$(now)
    printf 'key win+r\nwait 1500\ntype shutdown /s /f /t 0\nwait 300\nkey enter\n' > "$fifo"
    k=0; while kill -0 $pid 2>/dev/null && [ $k -lt 240 ]; do sleep 0.5; k=$((k+1)); done
    if kill -0 $pid 2>/dev/null; then res=timeout-shutdown; kill $pid 2>/dev/null; fi
    wait $pid; rc=$?
    ts=$(el "$ts0")
  else
    pkill hvf_win; wait $pid 2>/dev/null; rc=$?
  fi
  [ "$res" != ok ] && fails=$((fails+1))
  echo "cycle=$i result=$res lock=${tl:-na}s desktop=${td:-na}s shutdown=${ts:-na}s exit=$rc" | tee -a "$log"
  cp "$W/screen.bmp" "$W/last$i.bmp" 2>/dev/null
done
echo "done: $((N-fails))/$N cycles ok" | tee -a "$log"
[ "$fails" = 0 ]

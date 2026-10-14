#!/bin/bash
# xp.sh '<cmd on XP>' [timeout]  -- run a command on the XP box, print its output
URL=http://127.0.0.1:8899
[ -z "$1" ] && { echo "usage: xp.sh '<command>' [timeout]"; exit 1; }
seq=$(curl -s --data-raw "$1" "$URL/push")
[[ "$seq" =~ ^[0-9]+$ ]] || { echo "push failed: $seq"; exit 1; }
out=$(curl -s "$URL/wait?seq=$seq&timeout=${2:-120}")
case "$out" in
  OK*)   printf '%s\n' "${out#OK}" | sed '/^$/d' ;;
  TIMEOUT*) echo "[xp.sh] TIMEOUT waiting for result"; exit 2 ;;
  *)     printf '%s\n' "$out" ;;
esac

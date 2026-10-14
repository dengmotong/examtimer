#!/bin/bash
# xpget.sh <remote-on-xp-relative-to-share> <local-path>
URL=http://127.0.0.1:8899
n="$1"; d="${2:-.}"
out=$(/root/codepath/ExamTool/test/xp.sh "@UPLOAD C:\\xptest\\$n $n" 300)
echo "$out"
[ -f "/srv/xpshare/$n" ] && cp -f "/srv/xpshare/$n" "$d/" && echo "saved -> $d/$n"

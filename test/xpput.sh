#!/bin/bash
# xpput.sh <local-file> [remote-name-on-xp]   -- copy host file to C:\xptest on XP
URL=http://127.0.0.1:8899
src="$1"; name="${2:-$(basename "$1")}"
cp -f "$src" "/srv/xpshare/$name" || exit 1
chmod 644 "/srv/xpshare/$name"
/root/codepath/ExamTool/test/xp.sh "@DOWNLOAD $name C:\\xptest\\$name" 120
/root/codepath/ExamTool/test/xp.sh "dir C:\\xptest\\$name | find \"$name\"" 60

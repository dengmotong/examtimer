#!/bin/bash
# xprun.sh '<windows command line>' [wait]  -- type a command into the XP "Run" dialog
export DISPLAY=:99
CMD="$1"; WAIT="${2:-4}"
xdotool mousemove 500 500; sleep 0.4
xdotool key --clearmodifiers super+r; sleep 2.2
xdotool key --clearmodifiers ctrl+a; xdotool key --clearmodifiers Delete; sleep 0.2
xdotool type --clearmodifiers --delay 25 "$CMD"
sleep 0.4
xdotool key --clearmodifiers Return
sleep "$WAIT"

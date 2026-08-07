#!/bin/sh

LOG=/tmp/dmabuf-longrun-detached.txt
RESULT=/tmp/dmabuf-longrun-detached.exit
BIN=/tmp/drm_v4l2_pipeline/drm_v4l2_pipeline

restore_display()
{
	systemctl start lightdm
}

trap restore_display EXIT HUP INT TERM

rm -f "$LOG" "$RESULT"
systemctl stop lightdm
"$BIN" --backend dmabuf -W 1280 -H 720 -b 4 -n 60000 -l 1805 >"$LOG" 2>&1
rc=$?
echo "$rc" >"$RESULT"
exit "$rc"

#!/bin/sh

set -u
BIN=/tmp/drm_v4l2_pipeline/drm_v4l2_pipeline-w4
OUT=/tmp/week4-restart

restore_display()
{
	systemctl start lightdm
}

trap restore_display EXIT HUP INT TERM
mkdir -p "$OUT/logs"
grep -E 'MemAvailable|CmaTotal|CmaFree' /proc/meminfo >"$OUT/before-meminfo.txt"
systemctl stop lightdm
success=0
failure=0
iteration=1
while [ "$iteration" -le 100 ]; do
	if "$BIN" --backend dmabuf -W 1280 -H 720 -b 4 -n 10 -l 10 \
		>"$OUT/logs/run-$iteration.txt" 2>&1; then
		success=$((success+1))
	else
		failure=$((failure+1))
	fi
	iteration=$((iteration+1))
done
grep -E 'MemAvailable|CmaTotal|CmaFree' /proc/meminfo >"$OUT/after-meminfo.txt"
printf 'runs=100 success=%s failure=%s\n' "$success" "$failure" >"$OUT/summary.txt"
[ "$failure" -eq 0 ]

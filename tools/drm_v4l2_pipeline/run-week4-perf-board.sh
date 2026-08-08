#!/bin/sh

set -u
BIN=/tmp/drm_v4l2_pipeline/drm_v4l2_pipeline-w4
PERF=/tmp/perf
OUT=/tmp/week4-perf

restore_display()
{
	systemctl start lightdm
}

trap restore_display EXIT HUP INT TERM
mkdir -p "$OUT"
systemctl stop lightdm

for backend in copy dmabuf; do
	"$BIN" --backend "$backend" -W 1280 -H 720 -b 4 -n 2400 -l 2400 \
		>"$OUT/$backend-program.txt" 2>&1 &
	pid=$!
	sleep 5
	"$PERF" stat -p "$pid" \
		-e cycles,instructions,cache-references,cache-misses,context-switches,page-faults \
		-- sleep 60 >"$OUT/$backend-perf-stat.txt" 2>&1
	wait "$pid" || exit 1
done

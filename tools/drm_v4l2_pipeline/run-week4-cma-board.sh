#!/bin/sh

set -u
TRACE=/sys/kernel/tracing
BIN=/tmp/drm_v4l2_pipeline/drm_v4l2_pipeline-w4
OUT=/tmp/week4-cma
PRESSURE=/dev/shm/week4-pressure.bin

cleanup()
{
	echo 0 >"$TRACE/tracing_on"
	echo 0 >"$TRACE/events/cma/enable"
	rm -f "$PRESSURE"
	systemctl start lightdm
}

trap cleanup EXIT HUP INT TERM
mkdir -p "$OUT"
rm -f "$PRESSURE"
systemctl stop lightdm
echo 0 >"$TRACE/tracing_on"
echo 0 >"$TRACE/events/cma/enable"
: >"$TRACE/trace"
echo 1 >"$TRACE/events/cma/enable"
echo 1 >"$TRACE/tracing_on"

for buffers in 3 4 6; do
	for backend in copy dmabuf; do
		name="cold-$backend-b$buffers"
		echo "$name-BEGIN" >"$TRACE/trace_marker"
		grep -E 'MemAvailable|CmaTotal|CmaFree' /proc/meminfo >"$OUT/$name-before.txt"
		"$BIN" --backend "$backend" -W 1280 -H 720 -b "$buffers" -n 120 -l 120 \
			>"$OUT/$name-program.txt" 2>&1
		rc=$?
		grep -E 'MemAvailable|CmaTotal|CmaFree' /proc/meminfo >"$OUT/$name-after.txt"
		echo "$rc" >"$OUT/$name-exit.txt"
		echo "$name-END" >"$TRACE/trace_marker"
		[ "$rc" -eq 0 ] || exit "$rc"
	done
done

dd if=/dev/zero of="$PRESSURE" bs=1M count=2048 status=none
grep -E 'MemAvailable|CmaTotal|CmaFree' /proc/meminfo >"$OUT/pressure-meminfo.txt"
for buffers in 3 4 6; do
	name="pressure-dmabuf-b$buffers"
	echo "$name-BEGIN" >"$TRACE/trace_marker"
	"$BIN" --backend dmabuf -W 1280 -H 720 -b "$buffers" -n 120 -l 120 \
		>"$OUT/$name-program.txt" 2>&1
	rc=$?
	echo "$rc" >"$OUT/$name-exit.txt"
	echo "$name-END" >"$TRACE/trace_marker"
	[ "$rc" -eq 0 ] || exit "$rc"
done

echo 0 >"$TRACE/tracing_on"
cat "$TRACE/trace" >"$OUT/cma-trace.txt"

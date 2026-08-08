#!/bin/sh

set -u
TRACE=/sys/kernel/tracing
BIN=/tmp/drm_v4l2_pipeline/drm_v4l2_pipeline-w4
OUT=/tmp/week4-ftrace

cleanup()
{
	echo 0 >"$TRACE/tracing_on"
	echo nop >"$TRACE/current_tracer"
	: >"$TRACE/set_ftrace_filter"
	systemctl start lightdm
}

trap cleanup EXIT HUP INT TERM
mkdir -p "$OUT"
systemctl stop lightdm
echo 0 >"$TRACE/tracing_on"
echo nop >"$TRACE/current_tracer"
: >"$TRACE/trace"
printf '%s\n' vb2_buffer_done drm_atomic_commit \
	rockchip_drm_atomic_helper_commit_tail_rpm dma_fence_signal \
	>"$TRACE/set_ftrace_filter"
echo function >"$TRACE/current_tracer"
echo 1 >"$TRACE/tracing_on"
"$BIN" --backend dmabuf -W 1280 -H 720 -b 4 -n 300 -l 300 \
	>"$OUT/program.txt" 2>&1
rc=$?
echo 0 >"$TRACE/tracing_on"
cat "$TRACE/trace" >"$OUT/function-trace.txt"
cat "$TRACE/set_ftrace_filter" >"$OUT/filter.txt"
echo "$rc" >"$OUT/exit.txt"
exit "$rc"

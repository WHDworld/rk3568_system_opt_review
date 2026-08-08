#!/bin/sh

set -u

BIN=/tmp/drm_v4l2_pipeline/drm_v4l2_pipeline-w4
OUT=/tmp/week4-benchmark
RUN_FRAMES=1800
WARMUP_FRAMES=900
RUNS=5

restore_display()
{
	systemctl start lightdm
}

trap restore_display EXIT HUP INT TERM
mkdir -p "$OUT"
systemctl stop lightdm

for backend in copy dmabuf; do
	"$BIN" --backend "$backend" -W 1280 -H 720 -b 4 \
		-n "$WARMUP_FRAMES" -l 900 >"$OUT/$backend-warmup.txt" 2>&1 || exit 1
	run=1
	while [ "$run" -le "$RUNS" ]; do
		log="$OUT/$backend-run$run.txt"
		samples="$OUT/$backend-run$run-samples.csv"
		printf 'elapsed_s,proc_ticks,system_total_ticks,system_idle_ticks,rss_kb,pss_kb,fd_count,soc_temp_mC,cma_free_kB\n' >"$samples"
		"$BIN" --backend "$backend" -W 1280 -H 720 -b 4 \
			-n "$RUN_FRAMES" -l 1800 >"$log" 2>&1 &
		pid=$!
		start=$(cut -d' ' -f22 "/proc/$pid/stat")
		start_wall=$(cut -d' ' -f1 /proc/uptime | cut -d. -f1)
		while kill -0 "$pid" 2>/dev/null; do
			now=$(cut -d' ' -f1 /proc/uptime | cut -d. -f1)
			rss=$(awk '/VmRSS:/ {print $2}' "/proc/$pid/status" 2>/dev/null)
			pss=$(awk '/^Pss:/ {print $2}' "/proc/$pid/smaps_rollup" 2>/dev/null)
			fds=$(find "/proc/$pid/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)
			temp=$(cat /sys/class/thermal/thermal_zone0/temp)
			cma=$(awk '/CmaFree:/ {print $2}' /proc/meminfo)
			proc_ticks=$(awk '{print $14+$15}' "/proc/$pid/stat" 2>/dev/null)
			set -- $(awk '/^cpu / {total=0; for (i=2;i<=NF;i++) total+=$i; print total, $5+$6}' /proc/stat)
			printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$((now-start_wall))" \
				"${proc_ticks:-0}" "$1" "$2" "${rss:-0}" "${pss:-0}" \
				"$fds" "$temp" "$cma" >>"$samples"
			sleep 1
		done
		wait "$pid" || exit 1
		end_wall=$(cut -d' ' -f1 /proc/uptime | cut -d. -f1)
		# Process already exited, so latency/FPS data comes from its own SUMMARY.
		printf 'BENCH_META backend=%s run=%s wall_s=%s start_ticks=%s\n' \
			"$backend" "$run" "$((end_wall-start_wall))" "$start" >>"$log"
		run=$((run+1))
	done
done

#!/bin/sh

set -u
BIN=/tmp/drm_v4l2_pipeline/drm_v4l2_pipeline-w4
OUT=/tmp/week4-2hour
FRAMES=217000

restore_display()
{
	systemctl start lightdm
}

trap restore_display EXIT HUP INT TERM
mkdir -p "$OUT"
grep -E 'MemAvailable|CmaTotal|CmaFree' /proc/meminfo >"$OUT/before-meminfo.txt"
dmesg >"$OUT/dmesg-before.txt"
systemctl stop lightdm
"$BIN" --backend dmabuf -W 1280 -H 720 -b 4 -n "$FRAMES" -l 18000 \
	>"$OUT/program.txt" 2>&1 &
pid=$!
printf 'elapsed_s,proc_ticks,rss_kb,pss_kb,fd_count,soc_temp_mC,cma_free_kB\n' \
	>"$OUT/runtime-samples.csv"
start_wall=$(cut -d' ' -f1 /proc/uptime | cut -d. -f1)
while kill -0 "$pid" 2>/dev/null; do
	now=$(cut -d' ' -f1 /proc/uptime | cut -d. -f1)
	proc_ticks=$(awk '{print $14+$15}' "/proc/$pid/stat" 2>/dev/null)
	rss=$(awk '/VmRSS:/ {print $2}' "/proc/$pid/status" 2>/dev/null)
	pss=$(awk '/^Pss:/ {print $2}' "/proc/$pid/smaps_rollup" 2>/dev/null)
	fds=$(find "/proc/$pid/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)
	temp=$(cat /sys/class/thermal/thermal_zone0/temp)
	cma=$(awk '/CmaFree:/ {print $2}' /proc/meminfo)
	printf '%s,%s,%s,%s,%s,%s,%s\n' "$((now-start_wall))" "${proc_ticks:-0}" \
		"${rss:-0}" "${pss:-0}" "$fds" "$temp" "$cma" >>"$OUT/runtime-samples.csv"
	sleep 60
done
wait "$pid"
rc=$?
grep -E 'MemAvailable|CmaTotal|CmaFree' /proc/meminfo >"$OUT/after-meminfo.txt"
dmesg >"$OUT/dmesg-after.txt"
echo "$rc" >"$OUT/exit.txt"
exit "$rc"

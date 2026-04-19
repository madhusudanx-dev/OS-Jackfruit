#!/usr/bin/env bash
set -euo pipefail

echo "[scheduler-demo] starting concurrent CPU-bound containers with different nice values"

rm -rf rootfs-cpuhi rootfs-cpulo
cp -a rootfs-base rootfs-cpuhi
cp -a rootfs-base rootfs-cpulo
cp -f ./cpu_hog ./rootfs-cpuhi/cpu_hog
cp -f ./cpu_hog ./rootfs-cpulo/cpu_hog

start_ts="$(date +%s)"

sudo ./engine run cpuhi ./rootfs-cpuhi "/cpu_hog 15" --nice 0 > scheduler-high.out 2>&1 &
high_pid=$!
sudo ./engine run cpulo ./rootfs-cpulo "/cpu_hog 15" --nice 10 > scheduler-low.out 2>&1 &
low_pid=$!

wait "${high_pid}" || true
high_end="$(date +%s)"
wait "${low_pid}" || true
low_end="$(date +%s)"

echo "[scheduler-demo] high-priority client wall time: $((high_end - start_ts))s"
echo "[scheduler-demo] low-priority client wall time:  $((low_end - start_ts))s"
echo
echo "[scheduler-demo] high-priority output"
cat scheduler-high.out
echo
echo "[scheduler-demo] low-priority output"
cat scheduler-low.out

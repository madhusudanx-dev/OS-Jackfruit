#!/usr/bin/env bash
set -euo pipefail

echo "[cleanup-demo] supervisor view"
sudo ./engine ps || true
echo
echo "[cleanup-demo] host process check"
ps -ef | grep -E "engine|cpu_hog|memory_hog|io_pulse" | grep -v grep || true
echo
echo "[cleanup-demo] zombie check"
ps -eo pid,ppid,stat,cmd | grep " Z" | grep -v grep || true

#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "This script must be run inside the required Ubuntu VM."
  exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARTIFACT_DIR="${ROOT_DIR}/artifacts"
SUPERVISOR_LOG="${ARTIFACT_DIR}/supervisor.out"
RUN_LOG="${ARTIFACT_DIR}/demo-summary.log"
SUPERVISOR_PID=""

mkdir -p "${ARTIFACT_DIR}"

log() {
  printf '[demo] %s\n' "$*" | tee -a "${RUN_LOG}"
}

require_root() {
  if ! sudo -n true 2>/dev/null; then
    echo "sudo access is required. Please authenticate once before running this script."
    sudo true
  fi
}

cleanup() {
  set +e

  if [[ -n "${SUPERVISOR_PID}" ]] && kill -0 "${SUPERVISOR_PID}" 2>/dev/null; then
    log "Stopping supervisor process ${SUPERVISOR_PID}"
    sudo kill -TERM "${SUPERVISOR_PID}" 2>/dev/null || true
    wait "${SUPERVISOR_PID}" 2>/dev/null || true
  fi

  sudo "${ROOT_DIR}/engine" stop alpha >/dev/null 2>&1 || true
  sudo "${ROOT_DIR}/engine" stop beta >/dev/null 2>&1 || true
  sudo "${ROOT_DIR}/engine" stop memdemo >/dev/null 2>&1 || true
  sudo rmmod monitor >/dev/null 2>&1 || true
}

trap cleanup EXIT

require_root

cd "${ROOT_DIR}"

log "Cleaning previous artifacts"
rm -rf "${ARTIFACT_DIR}"
mkdir -p "${ARTIFACT_DIR}"
rm -f scheduler-high.out scheduler-low.out

log "Building project"
make 2>&1 | tee "${ARTIFACT_DIR}/01-build.txt"

log "Preparing root filesystems"
./scripts/prepare_rootfs.sh alpha beta memdemo 2>&1 | tee "${ARTIFACT_DIR}/02-rootfs.txt"

log "Loading kernel module"
sudo insmod monitor.ko
ls -l /dev/container_monitor | tee "${ARTIFACT_DIR}/03-device.txt"

log "Starting supervisor"
sudo "${ROOT_DIR}/engine" supervisor ./rootfs-base >"${SUPERVISOR_LOG}" 2>&1 &
SUPERVISOR_PID=$!
sleep 2
ps -fp "${SUPERVISOR_PID}" | tee "${ARTIFACT_DIR}/04-supervisor.txt"

log "Starting background containers"
sudo "${ROOT_DIR}/engine" start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80 \
  | tee "${ARTIFACT_DIR}/05-start-alpha.txt"
sudo "${ROOT_DIR}/engine" start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96 \
  | tee "${ARTIFACT_DIR}/06-start-beta.txt"

log "Capturing metadata table"
sudo "${ROOT_DIR}/engine" ps | tee "${ARTIFACT_DIR}/07-ps.txt"

log "Generating log activity"
sudo "${ROOT_DIR}/engine" run iolog ./rootfs-beta "/io_pulse 8 100" \
  | tee "${ARTIFACT_DIR}/08-io-run.txt" || true
sudo "${ROOT_DIR}/engine" logs iolog | tee "${ARTIFACT_DIR}/09-io-logs.txt" || true

log "Running memory-limit demo"
./scripts/demo_memory_limits.sh memdemo 2>&1 | tee "${ARTIFACT_DIR}/10-memory-demo.txt"
sudo dmesg | tail -n 40 | tee "${ARTIFACT_DIR}/11-dmesg.txt"
sudo "${ROOT_DIR}/engine" ps | tee "${ARTIFACT_DIR}/12-post-memory-ps.txt"

log "Running scheduler demo"
./scripts/demo_scheduler.sh 2>&1 | tee "${ARTIFACT_DIR}/13-scheduler.txt"

log "Stopping background containers"
sudo "${ROOT_DIR}/engine" stop alpha | tee "${ARTIFACT_DIR}/14-stop-alpha.txt" || true
sudo "${ROOT_DIR}/engine" stop beta | tee "${ARTIFACT_DIR}/15-stop-beta.txt" || true
sleep 2

log "Capturing cleanup evidence"
./scripts/demo_cleanup.sh 2>&1 | tee "${ARTIFACT_DIR}/16-cleanup.txt"

log "Stopping supervisor"
sudo kill -TERM "${SUPERVISOR_PID}"
wait "${SUPERVISOR_PID}" || true
SUPERVISOR_PID=""

log "Unloading kernel module"
sudo rmmod monitor
lsmod | grep monitor | tee "${ARTIFACT_DIR}/17-lsmod-after-rmmod.txt" || true

log "Demo run complete. Collect screenshots using docs/DEMO_RUNBOOK.md and files in artifacts/."

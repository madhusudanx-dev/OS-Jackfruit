#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <container-id>"
  echo "Example: $0 memdemo"
  exit 1
fi

id="$1"
rootfs="rootfs-${id}"

if [[ ! -d "${rootfs}" ]]; then
  echo "Missing ${rootfs}. Run ./scripts/prepare_rootfs.sh ${id} first."
  exit 1
fi

echo "[memory-demo] triggering soft and hard limits for ${id}"
sudo ./engine run "${id}" "./${rootfs}" "/memory_hog 8 400" --soft-mib 16 --hard-mib 32 || true
echo
echo "[memory-demo] recent kernel events"
sudo dmesg | tail -n 20

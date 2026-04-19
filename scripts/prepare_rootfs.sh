#!/usr/bin/env bash
set -euo pipefail

ROOTFS_URL="${ROOTFS_URL:-https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz}"
ROOTFS_TARBALL="${ROOTFS_TARBALL:-alpine-minirootfs-3.20.3-x86_64.tar.gz}"
CONTAINERS=("${@:-alpha beta}")

if [[ ! -f "${ROOTFS_TARBALL}" ]]; then
  wget -O "${ROOTFS_TARBALL}" "${ROOTFS_URL}"
fi

rm -rf rootfs-base
mkdir -p rootfs-base
tar -xzf "${ROOTFS_TARBALL}" -C rootfs-base

for name in "${CONTAINERS[@]}"; do
  rm -rf "rootfs-${name}"
  cp -a rootfs-base "rootfs-${name}"
  cp -f ./memory_hog "./rootfs-${name}/memory_hog"
  cp -f ./cpu_hog "./rootfs-${name}/cpu_hog"
  cp -f ./io_pulse "./rootfs-${name}/io_pulse"
done

echo "Prepared rootfs-base and copies for: ${CONTAINERS[*]}"

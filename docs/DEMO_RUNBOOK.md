# Demo Runbook

This runbook is the fastest honest path to finishing the submission in the required Ubuntu VM.

## Before You Start

1. Boot the Ubuntu 22.04 or 24.04 VM.
2. Disable Secure Boot for kernel module loading.
3. Clone your fork and open two terminals.
4. In terminal 1, keep the supervisor visible whenever possible.
5. In terminal 2, run the CLI commands and capture screenshots.

## One-Time Setup

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) wget
git clone https://github.com/madhusudanx-dev/OS-Jackfruit.git
cd OS-Jackfruit
chmod +x scripts/*.sh
```

## Fastest Reproducible Path

### Option A: Guided manual demo

Use the commands in `README.md` and take screenshots exactly when prompted below.

### Option B: Automated artifact capture

Run:

```bash
./scripts/run_full_demo.sh
```

This produces text artifacts in `artifacts/` that you can keep open while taking screenshots.

## Required Screenshots

### `01-ci-build.png`

Command:

```bash
make -C boilerplate ci
```

Must show:

- successful CI-safe user-space build

### `02-full-build.png`

Command:

```bash
make
```

Must show:

- root-level build success
- `monitor.ko` present

### `03-module-device.png`

Commands:

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

Must show:

- module inserted successfully
- device node exists

### `04-supervisor-start.png`

Command:

```bash
sudo ./engine supervisor ./rootfs-base
```

Must show:

- long-running supervisor terminal
- socket listener startup line

### `05-multi-container.png`

Commands from terminal 2:

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96
sudo ./engine ps
```

Must show:

- two live containers under one supervisor

### `06-ps-metadata.png`

Command:

```bash
sudo ./engine ps
```

Must show:

- ID
- PID
- state
- timestamps
- soft/hard limits
- reason
- log path

### `07-logging.png`

Commands:

```bash
sudo ./engine run iolog ./rootfs-beta "/io_pulse 8 100"
sudo ./engine logs iolog
```

Must show:

- persisted log output
- supervisor terminal with `[producer]` and `[logger]`

### `08-cli-ipc.png`

Must show:

- one terminal issuing a CLI command
- the supervisor terminal processing it

Good command pair:

```bash
sudo ./engine start cliipc ./rootfs-alpha /bin/sh
sudo ./engine stop cliipc
```

### `09-soft-limit.png`

Commands:

```bash
./scripts/demo_memory_limits.sh memdemo
sudo dmesg | tail -n 40
```

Must show:

- `SOFT LIMIT` warning line in `dmesg`

### `10-hard-limit.png`

Commands:

```bash
sudo dmesg | tail -n 40
sudo ./engine ps
```

Must show:

- `HARD LIMIT` kill line
- supervisor metadata showing `hard_limit_killed`

### `11-scheduler.png`

Command:

```bash
./scripts/demo_scheduler.sh
```

Must show:

- different outcomes for different `nice` values
- clear timing/progress difference between the two runs

### `12-cleanup.png`

Commands:

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
./scripts/demo_cleanup.sh
sudo rmmod monitor
```

Must show:

- no zombie processes
- no lingering workload processes
- module removed

## Final Steps

1. Save the screenshots in `docs/screenshots/`.
2. Replace the placeholder team information in `README.md`.
3. Fill the scheduler results table in `README.md` with the measured numbers from your VM run.
4. Commit and push the screenshots plus the updated `README.md`.

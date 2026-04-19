# Multi-Container Runtime

## Team Information

Replace this block before final submission:

- Team Member 1: `YOUR NAME` - `YOUR SRN`
- Team Member 2: `TEAMMATE NAME` - `TEAMMATE SRN`

## Project Overview

This project implements a lightweight Linux container runtime in C with a long-running supervisor and a kernel-space memory monitor.

The runtime supports:

- Multiple concurrent containers under a single supervisor
- PID, UTS, and mount namespace isolation
- Per-container writable root filesystems derived from a common base image
- A UNIX-domain-socket control plane for `start`, `run`, `ps`, `logs`, and `stop`
- A bounded-buffer logging pipeline with producer/consumer synchronization
- Kernel-enforced soft and hard memory limits through `/dev/container_monitor`
- Scheduler experiments using CPU-bound and I/O-oriented workloads

## Repository Layout

- [`engine.c`](./engine.c): root-level wrapper so `make` works from the repository root
- [`monitor.c`](./monitor.c): root-level wrapper for kernel module build
- [`monitor_ioctl.h`](./monitor_ioctl.h): root-level shared ioctl header
- [`boilerplate/`](./boilerplate): canonical implementation used by inherited GitHub Actions smoke checks
- [`scripts/`](./scripts): helper scripts for rootfs setup and demo runs
- [`docs/TEST_CASES.md`](./docs/TEST_CASES.md): test matrix and screenshot checklist
- [`REPORT.md`](./REPORT.md): expanded project report

## Build, Load, and Run Instructions

These steps are intended for the required Ubuntu 22.04 or 24.04 VM with Secure Boot disabled.

### 1. Install dependencies

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) wget
```

### 2. Build the project

```bash
make
```

The inherited CI-safe smoke path remains available:

```bash
make -C boilerplate ci
```

### 3. Prepare the root filesystem

```bash
chmod +x scripts/prepare_rootfs.sh
./scripts/prepare_rootfs.sh alpha beta
```

This downloads Alpine minirootfs, prepares `rootfs-base/`, and creates writable copies `rootfs-alpha/` and `rootfs-beta/` with the workload binaries copied in.

### 4. Load the kernel module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

### 5. Start the supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

Keep this terminal open. It will show producer and consumer activity for the logging pipeline.

### 6. Launch containers from another terminal

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96
```

### 7. Inspect state and logs

```bash
sudo ./engine ps
sudo ./engine logs alpha
```

### 8. Run workload commands

Examples:

```bash
sudo ./engine run memdemo ./rootfs-alpha "/memory_hog 8 400" --soft-mib 16 --hard-mib 32
sudo ./engine run cpudemo ./rootfs-beta "/cpu_hog 10" --nice 5
```

### 9. Stop containers and clean up

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
./scripts/demo_cleanup.sh
sudo dmesg | tail -n 20
sudo rmmod monitor
```

## CLI Reference

```bash
./engine supervisor <base-rootfs>
./engine start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
./engine run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
./engine ps
./engine logs <id>
./engine stop <id>
```

## Demo With Screenshots

The assignment requires annotated screenshots for the following items. Store them in [`docs/screenshots/`](./docs/screenshots) using the filenames listed in [`docs/TEST_CASES.md`](./docs/TEST_CASES.md).

### 1. Multi-container supervision

Capture two or more live containers managed by one supervisor.

Suggested command:

```bash
sudo ./engine ps
```

Caption to use: `Two isolated containers are running concurrently under one long-lived supervisor.`

### 2. Metadata tracking

Capture the `ps` table showing container IDs, host PIDs, states, start times, memory limits, reasons, and log paths.

### 3. Bounded-buffer logging

Capture:

- The supervisor terminal showing `[producer]` and `[logger]` activity
- The result of `sudo ./engine logs <id>` showing persisted output

### 4. CLI and IPC

Capture a client command from a second terminal and the supervisor response path through the UNIX socket interface.

### 5. Soft-limit warning

Run:

```bash
./scripts/demo_memory_limits.sh memdemo
```

Capture the `dmesg` line showing the soft-limit warning before the kill event.

### 6. Hard-limit enforcement

Capture the follow-up `dmesg` line showing the hard-limit kill plus `sudo ./engine ps` showing the final `hard_limit_killed` reason.

### 7. Scheduling experiment

Run:

```bash
./scripts/demo_scheduler.sh
```

Capture the two concurrent `run` clients and the timing difference between different `nice` values.

### 8. Clean teardown

Capture:

- `sudo ./engine stop <id>`
- `./scripts/demo_cleanup.sh`
- `sudo rmmod monitor`

The screenshot should show there are no lingering container processes or zombies.

## Design Decisions and Tradeoffs

### Namespace Isolation

- Choice: `clone()` with new PID, UTS, and mount namespaces, followed by `chroot()` and an in-container `/proc` mount.
- Tradeoff: `chroot()` is easier to reason about than `pivot_root()`, but it is less airtight against escape paths if the surrounding setup is weakened.
- Justification: It keeps the implementation compact while still exercising the kernel mechanisms the assignment targets.

### Supervisor Architecture

- Choice: one persistent supervisor process owns metadata, logging, signal handling, and kernel monitor registration.
- Tradeoff: centralization simplifies lifecycle tracking, but it means the supervisor becomes the main coordination point for all containers.
- Justification: this is the cleanest way to guarantee correct reaping, shared logging, and consistent state transitions.

### IPC and Logging

- Choice: UNIX domain socket for CLI control-plane IPC, and anonymous pipes for container stdout/stderr.
- Tradeoff: two IPC paths mean more moving pieces, but each mechanism is specialized for its job.
- Justification: separating command/control from log transport makes the design easier to debug and easier to explain.

### Kernel Monitor

- Choice: a timer-driven kernel module with a mutex-protected linked list of monitored PIDs.
- Tradeoff: periodic polling is simpler than event-driven accounting, but it only observes memory at the sampling interval.
- Justification: it is straightforward, safe for the linked-list manipulation paths, and matches the assignment’s requirement to explain soft and hard enforcement behavior clearly.

### Scheduler Experiments

- Choice: compare concurrent CPU-bound containers with different `nice` values and optionally compare CPU-bound versus I/O-oriented workloads.
- Tradeoff: experiments stay simple and reproducible, but they do not cover every scheduler knob.
- Justification: the chosen workloads make fairness, responsiveness, and throughput differences visible with minimal setup overhead.

## Engineering Analysis

### 1. Isolation Mechanisms

Container isolation here comes from kernel namespaces plus filesystem redirection. `CLONE_NEWPID` gives the child a separate PID namespace so processes see a container-local process tree. `CLONE_NEWUTS` lets the runtime assign a container-specific hostname. `CLONE_NEWNS` gives the child an independent mount table, which is why mounting `/proc` inside the container does not mutate the host mount namespace. `chroot()` changes the visible filesystem root so the container’s `/` resolves inside its writable rootfs copy. Even with these boundaries, the kernel itself is still shared: all containers rely on the same host kernel scheduler, memory manager, and device model.

### 2. Supervisor and Process Lifecycle

A long-running supervisor is useful because containers are not independent background shells in this design; they are managed processes with metadata, logs, and policy attached to them. The supervisor creates children with `clone()`, stores the host PID, tracks start time and memory limits, and handles cleanup when `SIGCHLD` arrives. This also keeps signal ownership consistent. A manual `stop` request marks `stop_requested` before sending termination so the runtime can later classify the exit as a user-initiated stop instead of a hard-limit kill.

### 3. IPC, Threads, and Synchronization

The project deliberately uses two IPC mechanisms. Control traffic moves over a UNIX domain socket because requests are structured, bidirectional, and short-lived. Log traffic uses per-container pipes because stdout and stderr are byte streams naturally represented as file descriptors. The shared bounded buffer needs a mutex and two condition variables: without the mutex, producers and consumers could corrupt the ring-buffer head/tail indices; without `not_full`, producers could busy-spin or overwrite entries; without `not_empty`, consumers would either spin wastefully or read invalid data. Metadata is synchronized separately so container state transitions do not race with `ps`, `stop`, or `run` waiters.

### 4. Memory Management and Enforcement

RSS measures resident physical memory currently mapped into a process, not the total virtual address space and not memory that has been swapped out. That is why RSS is a useful signal for real pressure but not a complete description of a process’s memory footprint. Soft and hard limits represent different operating-system policies: a soft limit is a warning threshold that signals growing pressure, while a hard limit is an enforcement boundary that protects the system by terminating the offender. The enforcement belongs in kernel space because only the kernel has authoritative, race-resistant visibility into process memory accounting and the ability to act on a process even if user-space runtime components are delayed or compromised.

### 5. Scheduling Behavior

Linux’s scheduler tries to balance fairness with responsiveness. For CPU-bound tasks, lowering the nice value gives the task a stronger claim on CPU time, so under contention it should complete or make progress more quickly than a less favored CPU-bound peer. An I/O-heavy task, by contrast, often sleeps voluntarily and becomes runnable in short bursts, which tends to preserve responsiveness because the scheduler can run it promptly when it wakes. The runtime is useful as an experiment harness because it keeps the workloads isolated, logs their output, and makes it easy to vary one scheduling input at a time.

## Scheduler Experiment Results

Fill this table after running the demo in the Ubuntu VM.

| Experiment | Workloads | Configuration | Measurement | Observation |
| --- | --- | --- | --- | --- |
| EXP-01 | `cpu_hog` vs `cpu_hog` | `nice 0` vs `nice 10` | Wall time to completion / log progress rate | Higher-priority CPU container should make faster progress under contention |
| EXP-02 | `cpu_hog` vs `io_pulse` | same priority | Responsiveness and interleaving of output | I/O-oriented work should remain responsive because it sleeps between bursts |

## Test Cases

See [`docs/TEST_CASES.md`](./docs/TEST_CASES.md) for the complete test matrix and screenshot mapping.

## Expanded Report

See [`REPORT.md`](./REPORT.md) for the longer submission report and final personalization checklist.

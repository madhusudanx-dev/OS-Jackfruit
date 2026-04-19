# Multi-Container Runtime Report

## Submission Summary

This repository implements a lightweight Linux container runtime in C with:

- A long-running supervisor that manages multiple containers concurrently
- A UNIX-domain-socket control plane for `start`, `run`, `ps`, `logs`, and `stop`
- A pipe-based bounded-buffer logging pipeline with producer and consumer threads
- A Linux kernel module that tracks container PIDs and enforces soft and hard RSS limits
- Workload programs for memory pressure and scheduling experiments

## Deliverables Included

- Source code in both the repo root and `boilerplate/` for compatibility with the submission guide and inherited CI
- `Makefile` at the repo root plus the required CI-safe `make -C boilerplate ci` path
- Demo helper scripts in [`scripts/`](./scripts)
- Test matrix in [`docs/TEST_CASES.md`](./docs/TEST_CASES.md)
- Screenshot runbook in [`docs/DEMO_RUNBOOK.md`](./docs/DEMO_RUNBOOK.md)
- Screenshot staging folder in [`docs/screenshots/`](./docs/screenshots)

## Implementation Highlights

### User-Space Runtime

- `engine supervisor <base-rootfs>` starts a persistent supervisor that owns metadata and the logging pipeline.
- CLI requests use a UNIX domain socket at `/tmp/mini_runtime.sock`.
- `start` returns after launch and metadata registration.
- `run` blocks until container exit and returns the container exit status.
- `logs <id>` returns the persisted per-container log file.
- `stop <id>` marks `stop_requested` before sending termination, which allows the runtime to distinguish manual stop from hard-limit kill.

### Container Isolation

- Each child is created with `clone()` using new PID, UTS, and mount namespaces.
- The container hostname is set to the container ID.
- Each container enters its own rootfs with `chroot()`.
- `/proc` is mounted inside the container after entering the rootfs so container-local process views work.

### Logging Pipeline

- Each container’s stdout and stderr are redirected into a dedicated pipe.
- A producer thread reads from each pipe and inserts chunks into a bounded shared buffer.
- A consumer thread drains the buffer and appends to `logs/<container>.log`.
- The buffer uses one mutex plus `not_empty` and `not_full` condition variables to prevent corruption and deadlock.

### Kernel Monitor

- The module exposes `/dev/container_monitor`.
- User space registers the host PID and soft/hard thresholds with `ioctl`.
- Kernel space tracks monitored processes in a mutex-protected linked list.
- Soft-limit crossings emit a warning once.
- Hard-limit crossings send `SIGKILL` and remove the tracked entry.
- Stale tasks are cleaned up automatically during timer scans.

## Verification Status

### Completed from this workspace

- Repository forked into `madhusudanx-dev/OS-Jackfruit`
- Runtime and kernel module implementations added
- Top-level build path and inherited CI-safe build path aligned
- Demo scripts and report/test-plan documentation added

### Still requires the target Ubuntu VM

- Full kernel module build against the running Ubuntu kernel headers
- `insmod`/`rmmod` validation
- Real container execution with namespaces and `chroot`
- `dmesg` capture for soft-limit and hard-limit behavior
- Final screenshot evidence required by the assignment

## Recommended Final Demo Order

1. Build everything with `make`
2. Load the kernel module with `sudo insmod monitor.ko`
3. Start the supervisor with `sudo ./engine supervisor ./rootfs-base`
4. Launch two background containers and capture `engine ps`
5. Trigger log generation and capture `engine logs`
6. Trigger memory-limit events with `./scripts/demo_memory_limits.sh memdemo`
7. Run `./scripts/demo_scheduler.sh`
8. Stop containers, show cleanup, unload the module

For the shortest path, `./scripts/run_full_demo.sh` automates most of this flow and saves text artifacts under `artifacts/`.

## Personalization Items Before Submission

- Replace placeholder team member names and SRNs in `README.md`
- Run the full demo in the required Ubuntu VM
- Add the required screenshots into `docs/screenshots/`
- Fill the scheduler results table in `README.md` with measured numbers from the VM run

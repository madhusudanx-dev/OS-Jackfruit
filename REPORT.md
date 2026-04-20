# Multi-Container Runtime Report

## Team Information

- Madhusudan P - PES1UG24AM375
- Pranaav P - PES1UG24AM387

## Submission Summary

This repository contains a lightweight Linux container runtime in C with:

- a long-running supervisor for managing multiple containers
- a UNIX-domain-socket control plane for `start`, `run`, `ps`, `logs`, and `stop`
- a bounded-buffer logging pipeline using producer and consumer threads
- a Linux kernel module that registers container PIDs and monitors memory usage
- workload programs for runtime testing

## Deliverables Included

- Source code in both the repository root and `boilerplate/`
- `Makefile` with root-level build support
- CI-safe build path through `make -C boilerplate ci`
- Screenshot evidence in [`docs/screenshots/`](./docs/screenshots)
- Final project README and this report

## Implementation Highlights

### User-space runtime

- `engine supervisor <base-rootfs>` launches the persistent runtime supervisor.
- Short-lived CLI commands communicate with the supervisor over a UNIX socket.
- The runtime tracks container ID, host PID, state, limits, and log path.
- `logs <id>` reads back persisted per-container output.
- `stop <id>` follows a controlled termination path through the supervisor.

### Isolation model

- Containers are created with `clone()` using PID, UTS, and mount namespaces.
- Each container enters its assigned root filesystem using `chroot()`.
- `/proc` is mounted inside the container so process inspection works in the isolated namespace view.

### Logging model

- Container stdout and stderr are redirected to the supervisor through pipes.
- Producer threads read container output and push it into a bounded shared buffer.
- The logger thread consumes buffered data and writes it into `logs/<container>.log`.
- Mutex and condition variables protect the ring buffer against races and deadlock.

### Kernel monitor

- The kernel module exposes `/dev/container_monitor`.
- The supervisor registers the host PID and configured limits through `ioctl`.
- The module stores active entries in a mutex-protected linked list.
- Entries are removed when containers exit or when the module is unloaded.

## Screenshot Evidence Included

The repository includes screenshots demonstrating:

1. kernel module load and device creation
2. supervisor startup
3. multi-container launch
4. supervisor metadata output through `engine ps`
5. log retrieval through `engine logs`
6. controlled container stop
7. no lingering workload processes after cleanup
8. successful module unload and rootfs cleanup

## Design Decisions and Tradeoffs

### Namespace isolation

- Choice: PID, UTS, and mount namespaces with `chroot()`
- Tradeoff: simpler than `pivot_root()`, but not as strict if the surrounding environment is weakened
- Reason: compact implementation with the required isolation properties

### Supervisor structure

- Choice: a single long-running supervisor process
- Tradeoff: centralized coordination point, but easier lifecycle correctness
- Reason: simplifies metadata ownership, child reaping, logging, and signal handling

### IPC separation

- Choice: UNIX socket for control commands, pipes for log transport
- Tradeoff: two IPC paths increase implementation complexity
- Reason: each path cleanly matches its workload and makes the design easier to reason about

### Monitoring strategy

- Choice: timer-based kernel checks over a protected linked list
- Tradeoff: periodic checks rather than continuous event-driven accounting
- Reason: straightforward, safe, and sufficient for the required monitoring behavior

## Engineering Analysis

### Isolation mechanisms

The runtime isolates containers using kernel namespaces and a container-specific root filesystem. PID namespaces give each container its own process-numbering view, UTS namespaces isolate hostnames, and mount namespaces isolate mount changes such as the container-local `/proc` mount. `chroot()` redirects filesystem resolution so the container sees only its assigned root filesystem tree. These mechanisms isolate user-space views, while the host kernel remains shared.

### Supervisor and process lifecycle

The supervisor is valuable because it provides one authoritative parent for all containers. That allows consistent metadata tracking, centralized signal handling, orderly shutdown, and explicit child reaping. Without a persistent supervisor, lifecycle coordination would be fragmented and zombie handling would be much harder to guarantee.

### IPC, threads, and synchronization

The runtime uses separate control-plane and log-plane IPC to keep responsibilities clear. The control plane needs request/response semantics, which fit a UNIX domain socket well. The log path needs efficient byte-stream forwarding, which fits pipes naturally. A mutex plus condition variables protect the bounded log buffer from races and coordinate producer/consumer blocking when the buffer is full or empty.

### Memory management and enforcement

RSS measures resident physical memory, not total virtual address space. That makes it useful for spotting real pressure but incomplete as a full memory-footprint metric. Soft and hard limits embody different policies: soft limits warn, while hard limits enforce. Putting enforcement in the kernel is appropriate because the kernel has the most reliable visibility into process memory usage and can act immediately.

### Scheduling behavior

CPU-bound and I/O-oriented workloads interact differently with Linux scheduling. CPU-bound programs compete directly for processor time, so priority differences can visibly change progress rates. I/O-oriented programs tend to sleep between bursts and become runnable intermittently, which often preserves responsiveness. This runtime provides a practical platform for observing those scheduling effects in isolated processes.

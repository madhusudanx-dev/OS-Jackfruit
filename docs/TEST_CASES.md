# Test Cases and Evidence Plan

This file maps directly to the project rubric and can be used during the Ubuntu VM demo to capture evidence systematically.

## Environment

- OS: Ubuntu 22.04 or 24.04 VM
- Secure Boot: OFF
- Kernel headers installed for current kernel
- Module device expected at `/dev/container_monitor`

## Test Matrix

| ID | Scenario | Command(s) | Expected evidence | Screenshot file |
| --- | --- | --- | --- | --- |
| TC-01 | CI-safe user-space build | `make -C boilerplate ci` | All user-space binaries compile; `engine` usage path works | `docs/screenshots/01-ci-build.png` |
| TC-02 | Full root build | `make` | Root-level `engine`, workloads, and `monitor.ko` build successfully | `docs/screenshots/02-full-build.png` |
| TC-03 | Module load and device creation | `sudo insmod monitor.ko && ls -l /dev/container_monitor` | Device node exists and module reports load success | `docs/screenshots/03-module-device.png` |
| TC-04 | Supervisor startup | `sudo ./engine supervisor ./rootfs-base` | Long-running supervisor process listening on the UNIX socket | `docs/screenshots/04-supervisor-start.png` |
| TC-05 | Multi-container supervision | `sudo ./engine start alpha ./rootfs-alpha /bin/sh` and `sudo ./engine start beta ./rootfs-beta /bin/sh` | Two live containers under one supervisor | `docs/screenshots/05-multi-container.png` |
| TC-06 | Metadata tracking | `sudo ./engine ps` | Container ID, PID, state, start time, limits, reason, log path | `docs/screenshots/06-ps-metadata.png` |
| TC-07 | Bounded-buffer logging | Run `/io_pulse 10 100` or `/cpu_hog 10`, then `sudo ./engine logs <id>` | Container output persisted, plus producer/consumer activity visible in supervisor terminal | `docs/screenshots/07-logging.png` |
| TC-08 | CLI + control IPC | Issue `start`, `logs`, or `stop` from a second terminal | Supervisor responds over UNIX domain socket | `docs/screenshots/08-cli-ipc.png` |
| TC-09 | Soft-limit warning | `./scripts/demo_memory_limits.sh memdemo` | `dmesg` shows soft-limit warning before kill | `docs/screenshots/09-soft-limit.png` |
| TC-10 | Hard-limit enforcement | Same as TC-09 | `dmesg` shows hard-limit kill and `engine ps` marks `hard_limit_killed` | `docs/screenshots/10-hard-limit.png` |
| TC-11 | Scheduler experiment | `./scripts/demo_scheduler.sh` | Different completion timing / responsiveness for different `nice` values | `docs/screenshots/11-scheduler.png` |
| TC-12 | Clean teardown | `sudo ./engine stop <id>` plus `./scripts/demo_cleanup.sh` | No zombies, no stuck containers, no stale monitor entries after `sudo rmmod monitor` | `docs/screenshots/12-cleanup.png` |

## Notes for Capture

- Keep one terminal dedicated to the supervisor so producer/consumer flush lines are visible.
- Use a second terminal for `engine start`, `engine run`, `engine ps`, `engine logs`, and `engine stop`.
- After each run, store the screenshot in `docs/screenshots/` using the file names above.
- Add a one-line caption under each image in `README.md`.

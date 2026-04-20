# OS-Jackfruit — Multi-Container Runtime

A lightweight Linux container runtime written in C, featuring a long-running supervisor, kernel-space memory monitoring, bounded-buffer logging, and Linux scheduler experiments.

**Authors:** CHIRANTH.R (PES1UG24AM354) · Deeptanshu Kumar (PES1UG24AM357)

---

## Overview

The project has two integrated components:

| Component | File | Description |
|---|---|---|
| User-space runtime | `engine.c` | Supervisor + CLI client over UNIX sockets |
| Kernel module | `monitor.c` | LKM that enforces per-container memory limits |

**Key features:**
- Linux namespace isolation (PID, UTS, Mount) via `clone()`
- O(1) container metadata lookup using a djb2 hash table
- Bounded-buffer log pipeline (producer/consumer threads, mutex + condition variables)
- Kernel module communicates with the runtime via `ioctl`
- Soft-limit warnings and hard-limit `SIGKILL` enforcement
- Scheduler experiments using Linux CFS `nice` values

---

## Requirements

- Ubuntu 22.04 or 24.04 (bare-metal or VM — **no WSL**)
- Secure Boot **OFF** (required for loading the kernel module)
- Linux kernel headers

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

---

## Quick Start

### 1. Prepare root filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### 2. Build

```bash
make
ls -l engine monitor.ko cpu_hog io_pulse memory_hog
```

### 3. Load kernel module

```bash
sudo insmod ./monitor.ko
ls -l /dev/container_monitor   # should exist
dmesg | tail
```

### 4. Start the supervisor (Terminal 1)

```bash
sudo ./engine supervisor ./rootfs-base
# Supervisor started, listening on /tmp/engine_supervisor.sock
```

### 5. Launch containers (Terminal 2)

```bash
cp ./cpu_hog ./memory_hog ./io_pulse ./rootfs-alpha/
cp ./cpu_hog ./rootfs-beta/

sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  /bin/sh --soft-mib 64 --hard-mib 96

sudo ./engine ps
```

### 6. Run workloads

```bash
# Memory test — triggers soft/hard limit events
sudo ./engine run alpha ./rootfs-alpha /memory_hog 10 50 --soft-mib 40 --hard-mib 64
dmesg | grep container_monitor

# Scheduler experiment — compare CPU-bound priorities
time sudo ./engine run alpha ./rootfs-alpha /cpu_hog 10 --nice -5
time sudo ./engine run alpha ./rootfs-alpha /cpu_hog 10 --nice  5
```

### 7. View logs and clean up

```bash
sudo ./engine logs alpha
cat /tmp/container-alpha.log

sudo ./engine stop alpha
sudo ./engine stop beta
# Ctrl-C in supervisor terminal

sudo rmmod monitor
ps aux | grep engine   # no zombies
```

---

## Build Targets

| Target | Description |
|---|---|
| `make` / `make all` | Build binaries + kernel module |
| `make ci` | Build user-space binaries only (no kernel headers needed) |
| `make insmod` | Load `monitor.ko` |
| `make rmmod` | Unload `monitor.ko` |
| `make install` | Copy binaries to `/usr/local/bin` |
| `make clean` | Remove all build artifacts |

---

## Architecture

### Supervisor (`engine.c`)

The supervisor is a long-running process that:

- Spawns containers with `clone(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS)` + `chroot`
- Stores per-container metadata (PID, state, limits, log path) in an open-addressing hash table
- Accepts CLI commands (`start`, `stop`, `run`, `ps`, `logs`) over a UNIX domain socket
- Reaps exited children via `SIGCHLD` → `waitpid(-1, WNOHANG)`
- Captures container stdout/stderr through a **bounded-buffer log pipeline**:
  - Producer thread reads from a pipe into a 64 KB ring buffer
  - Consumer thread flushes entries to a per-container log file
  - Synchronised with a mutex and `not_full`/`not_empty` condition variables

### Kernel Module (`monitor.c`)

- Registers a character device at `/dev/container_monitor`
- Accepts `ioctl` calls from `engine` to register/unregister processes and set memory limits
- A kernel thread polls each registered PID every 100 ms using `get_mm_rss()`
- **Soft limit exceeded** → `printk` warning in `dmesg`
- **Hard limit exceeded** → `send_sig(SIGKILL, ...)` + `printk`
- Internal list protected by a spinlock (safe in both thread and softirq contexts)

### IPC

| Path | Mechanism | Direction |
|---|---|---|
| CLI → Supervisor | UNIX domain socket (`/tmp/engine_supervisor.sock`) | Commands + responses |
| Container → Supervisor | Pipe (per container) | Log data |
| Supervisor → Kernel | `ioctl` on `/dev/container_monitor` | Register/limit/unregister |

---

## Isolation Mechanisms

| Namespace | Flag | Effect |
|---|---|---|
| PID | `CLONE_NEWPID` | Container processes cannot see host PIDs |
| UTS | `CLONE_NEWUTS` | Per-container hostname |
| Mount | `CLONE_NEWNS` + `chroot` | Per-container root filesystem |

Network namespaces are intentionally omitted for simplicity; containers share the host network.

---

## Scheduler Experiments

All workloads accept a `--nice N` flag. Results on a lightly loaded host:

| Workload | Nice | Wall time |
|---|---|---|
| `cpu_hog 10` | -5 | ~9.9 s |
| `cpu_hog 10` | 0 | ~10.2 s |
| `cpu_hog 10` | +5 | ~10.9 s |
| `io_pulse 10` | 0 | ~2.3 s |

The I/O-bound task completes faster regardless of priority because the Linux CFS scheduler does not accumulate virtual runtime while a task is blocked on I/O, giving it an immediate scheduling advantage when it becomes runnable again.

---

## File Reference

```
engine.c          — supervisor + CLI implementation
monitor.c         — kernel module (LKM)
monitor_ioctl.h   — shared ioctl definitions
cpu_hog.c         — CPU-bound test workload
io_pulse.c        — I/O-bound test workload
memory_hog.c      — memory allocation test workload
Makefile          — build system
boilerplate/      — CI scripts and environment check
```

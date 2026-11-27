# Headless macOS CI, 16 KB Kernel & Multi-Guest Packing (`cloud-arm` v3)

This document summarizes the headless CI runner stack, native 16 KB kernel port, multi-guest packing architecture, and zero-Mac headless factory added on the `cloud-arm` branch.

## Components

- **Host fleet supervisor (`scripts/xnu-runnerd`, `scripts/xnu-runnerd.service`):** Dependency-free Python 3 supervisor supporting `mock-queue`, `github-jit` (ephemeral JIT registrations), and `github-reg` modes. Boots each job on an isolated `dm-snapshot` copy-on-write overlay over a read-only (`chmod 444`) golden `mac.img`, tracks per-job `xnu0` TX drops and golden immutability, and tears down cleanly on exit. Verified with `scripts/xnu-runnerd-mock-test.sh`.
- **Zero-Mac headless disk factory (`tools/cloud/vm/build-disk-linux.sh --headless on`, `tools/cloud/vm/disk_linux.py`):** Builds a bootable headless macOS 27.2 disk strictly on Linux with no GPU bundle (`269 s` end-to-end on an 8-vCPU ARM64 host), disables 50 system + 57 user UI/background services offline, and optionally stages guest tools from `tools/cloud/guest/`.
- **Guest headless & warmup tools (`tools/cloud/guest/`):** Reversible `xnu-mode {headless|desktop|status}` toggle, `xnu-xcrun-seed` LaunchDaemon + `/etc/zshenv` for seeding pre-warmed `/var/db/xcrun_db` across ephemeral CoW boots.
- **Native 16 KB host + XNU kernel (`xnu-patches/0006-linux64-16k-granule.patch`, `xnu-patches/0007-linux64-phase4-perf.patch`):** Ports `fki.ko`, `xnu.ko`, and XNU (`__ARM_16K_PG__`, 3-level 47-bit VA, BBM `L1[2047]` handoff) to `CONFIG_ARM64_16K_PAGES=y` Linux hosts, plus direct EL0 context save, in-vector `mach_absolute_time`/`mach_continuous_time` 65-bit reciprocal traps, ARMv8.4 range TLB invalidations (`tlbi rva(l)e1is`), 128-byte per-CPU cacheline alignment, and `validation_disables=0x20924`.
- **Multi-guest packing (`scripts/ci-multiguest-run.sh`, `scripts/vm-net-up.sh`, `scripts/vm-net-down.sh`):** Isolates multiple concurrent XNU guests on one Linux host using private per-guest `fki_ttbr1` shadow roots (keeping Linux strictly on `swapper_pg_dir`), disjoint physical carve-out slices, exclusive CPU sets, and per-guest `xnu0..N` NICs with `FORWARD DROP` isolation. Sets `linux64_idle_poll_us=0` in multi-guest mode to prevent cross-guest wakeup resonance.
- **Correctness & audit hardening:** Full `v0..v31` FP/SIMD inline-asm clobbers on `linux64_call2` and `mdev_host_call`, bounded `XnuEthernet` RX poll bursts (`maxBursts=8`), universal `XnuDisk` bounce-buffer fallback with `kIOMemoryMapperNone`, EL0 SError routed to sync exception handler, `FAST_STOP=1` 120 s unload wait, and `ROOT_UUID`-bound persistent keystores.

## Verified Benchmarks (ARM64 Neoverse-V2 Linux Host)

### 1. Live Side-by-Side GitHub Actions Multi-Language Showdown (`scripts/ci-multilang-bench.sh`)

Run side-by-side (`[self-hosted, macOS, ARM64]` via `xnu-runnerd --mode github-jit`, 7 vCPU ephemeral CoW vs GitHub-hosted `macos-15`, 3c M1) across SwiftPM release build & XCTest, headless `xcodebuild`, Rust/Cargo, Go, C/C++ CMake+Ninja, Node.js, Python 3, and live GitHub HTTPS:

| Metric | Self-Hosted ARM64 CoW (7c, macOS 27.2) | GitHub `macos-15` (3c M1) | Speedup |
|---|---:|---:|---:|
| **GitHub Actions Job Wall** | **`69 s`** | `141 s` | **`2.04×`** |
| **Multi-Language Benchmark Total** | **`59.91 s`** (`52.00 s` 5x CoW median) | `104.49 s` | **`1.74×`** |
| SwiftPM XCTest | **`9.94 s`** | `25.76 s` | **`2.59×`** |
| Headless `xcodebuild` | **`16.24 s`** | `27.83 s` | **`1.71×`** |
| Rust Cargo Build | **`1.71 s`** | `8.05 s` | **`4.71×`** |
| C/C++ CMake + Ninja | **`2.88 s`** | `8.54 s` | **`2.97×`** |
| Go Build + Test | **`6.27 s`** | `12.82 s` | **`2.05×`** |

Cold CoW boot: `8.7–12.8 s`; teardown: `0.99 s`; `xnu0` `tx_dropped_delta=0`; `golden_mutated=false`.

### 2. Native 16 KB Kernel & Multi-Guest Packing (`ci-bench.sh` timed workload)

| Configuration | Boot (s) | Job Wall (s) | Teardown (s) | Timed Sum (ms) | Jobs/hr | Est. Cost / 1k Jobs |
|---|---:|---:|---:|---:|---:|---:|
| Hosted M1 `3c` (timed floor) | — | ≥124.93 | — | `124,930` | ≤28.82 | ≥`$129.10` |
| CI v2 4K `7c` | ~5.7–6.6 | — | ~0.64 | `83,228` | — | — |
| **CI v3 16K `3c`** | `5.35` | `92.37` | `1.88` | **`88,803`** (`-11.2%`) | `36.14` | **`$6.53`** |
| **CI v3 16K `7c`** | `5.25` | `78.14` | `1.87` | **`74,212`** (`-10.8%`) | `42.22` | **`$11.18`** |
| **CI v3 16K `7c+RAM`** | `5.33` | `79.22` | `0.73` | **`73,211`** (`-12.0%`) | `42.21` | **`$11.18`** |
| **CI v3 16K `2×3c`** (8-vCPU host) | `7.54` | `116.82` | `0.69` | `111,331` (mean) | **`57.58`** | **`$8.20`** |
| **CI v3 16K `4×3c`** (16-vCPU host) | `15.23` | `122.17` | `0.82` | `116,176` (mean) | **`104.18`** | **`$9.06`** |
| **CI v3 16K `2×7c`** (16-vCPU host) | `7.54` | `89.24` | `0.53` | `84,985` (mean) | **`73.99`** | **`$12.76`** |

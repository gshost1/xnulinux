# Notes for AI coding agents

## What this repository is

xnulinux runs the macOS kernel (XNU, built from Apple's open source) as a loadable module inside an arm64 Linux
kernel, so that macOS user space runs next to Linux. Upstream is
[AFK-surf/xnulinux](https://github.com/AFK-surf/xnulinux). This fork's `cloud-arm` branch adds notes, three fixes and
helper tools for one more kind of host: a rented cloud ARM VM with no GPU.

## Where things are

| Path | What |
|---|---|
| `docs/cloud-arm-agent.md` | **The runbook for you.** Phases, commands, checks, known failures, what to ask first. |
| `docs/cloud-arm.md` | The same procedure for a person, with the reasons, measured numbers and troubleshooting. |
| `tools/cloud/` | Non-interactive helpers (ssh to the VM and the guest, screenshots, input, image transfer, disk build). Start with its `README.md`; every tool has `--help`. |
| `tests/hosted/`, `tests/lavapipe/` | Test programs with expected output. |
| `README.md`, `docs/production.md` | Upstream's description of the project and its stopgaps. |
| `LICENSE`, `LICENSES/` | Per-directory licences. Every source file carries an `SPDX-License-Identifier` line in its first three lines; CI checks it. |

If the user asks you to set this up on a cloud VM, follow `docs/cloud-arm-agent.md` from the top. Do not improvise
the order.

## Rules here

1. **Ask before spending.** Renting a VM or a Mac, storage and network transfer cost money. State the expected cost
   and wait for a yes.
2. **Ask before anything that needs a login or a password.** Get credentials through the user's own secret mechanism.
   Never print, log or commit one. Passwords are read from files the user names, over standard input.
3. **Tell the user about Apple's licence** before any step that installs or runs macOS: Apple's licence restricts
   macOS to Apple-branded hardware, and the decision is theirs. State it once, plainly, without comment.
4. **An Apple silicon Mac is required** to build the system disk. Say so early; there is no workaround in this repository.
5. **No Apple files from third parties, and none passed on.** Never download macOS, disk images, kernel collections or
   trust caches from anyone but Apple, and never upload them where others can read them. Nothing of Apple's is ever
   committed here: no binaries, images, kernel caches, trust caches, crash reports or disassembly.
6. **Never delete** the user's VM, disk image, bucket or its contents without an explicit yes for that exact thing.
7. **Login windows:** take a fresh screenshot and see the password field before sending input. Never type a password
   while a screen recording is running.
8. **Same fix failed twice the same way: stop and report** what you ran and what you saw.
9. **Report honestly.** Say which checks passed, which failed and which you did not run.
10. **Keep the licence line.** New files get the SPDX line of the directory they are in (`scripts/`, `tools/`,
    `tests/`, `docs/`: GPL-2.0-only; XNU-side code: APSL-2.0; patches to Mesa and tests of them: MIT). Linux-side code
    must not include XNU code, and the reverse.
11. **Do not commit** `tools/cloud/cloud.env`, keys, tokens, addresses or host names.

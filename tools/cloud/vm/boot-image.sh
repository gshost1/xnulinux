#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# boot-image.sh DIR [WAIT_SECONDS=90]: (on the VM) boot the hosted macOS from the image in DIR, which holds mac.img,
# mac.conf and trustcache.bin as scripts/mk-macvm-disk.sh made them. Installs the trust cache under /lib/firmware/xnu and
# mac.conf as /var/tmp/mac.conf, sets kernel.panic_on_oops=0 (a fault in the hosted kernel then ends the run instead of
# rebooting the VM), runs scripts/xnulinux-boot.sh with DISK=DIR/mac.img, waits, and prints the end of the hosted console
# and any lines that look like trouble. Run it through ../vm-run or copy it to the VM. XNU_REPO: this repository on the
# VM (default ~/w/poc). Not safe to repeat while a hosted system runs (xnulinux-boot.sh refuses; stop it first).
# Exit: 0 xnu.ko is loaded after the wait, 1 it is not, 2 wrong usage.
# Status: only syntax-checked in this cleaned form; the private original booted every image used.
set -uo pipefail
case "${1:-}" in -h|--help) sed -n '3,/^set /{/^#/!d;s/^# \{0,1\}//;p;}' "$0"; exit 0;; esac
D=${1:?usage: boot-image.sh DIR [wait-seconds]}; W=${2:-90}
for f in mac.img mac.conf trustcache.bin; do [ -e "$D/$f" ] || { echo "boot-image: no $D/$f" >&2; exit 2; }; done
cd "${XNU_REPO:-$HOME/w/poc}" || { echo "boot-image: no repository at ${XNU_REPO:-$HOME/w/poc} (set XNU_REPO)" >&2; exit 2; }
sudo cp "$D/trustcache.bin" /lib/firmware/xnu/trustcache-mac.bin; cp "$D/mac.conf" /var/tmp/mac.conf
sudo sysctl -qw kernel.panic_on_oops=0; sync
DISK=$D/mac.img bash scripts/xnulinux-boot.sh 2>&1 | tail -5
sleep "$W"
sudo dmesg > /var/tmp/xnu-boot-dmesg.txt
echo "dmesg lines: $(wc -l < /var/tmp/xnu-boot-dmesg.txt); xnu.ko loaded: $(lsmod | grep -c '^xnu ')"
grep -a "darwin console: " /var/tmp/xnu-boot-dmesg.txt | sed 's/.*darwin console: //' | grep -av "^$" | tail -40 | cut -c1-200
echo "--- lines worth reading"
grep -aiE "XNU panic|panic\(|took a fault|cannot resolve|Oops|BUG:|Waiting on|root device|BSD root|launchd|xnulinux#" /var/tmp/xnu-boot-dmesg.txt | cut -c1-220 | tail -25
lsmod | grep -q '^xnu ' || { echo "boot-image: xnu.ko is not loaded: the run ended (see /var/tmp/xnu-boot-dmesg.txt)" >&2; exit 1; }

#!/bin/bash
# Ephemeral copy-on-write (dm-snapshot) CI runner lifecycle for headless xnulinux.
# Creates an isolated RAM-backed CoW overlay over a read-only golden mac.img,
# boots CI_HEADLESS=1 FAST_BOOT=1 with early-boot retry supervision, runs a job
# script inside the guest over SSH, and tears down cleanly without mutating the base image.
set -euo pipefail

POC_DIR=$(cd "$(dirname "$0")/.." && pwd)
GOLDEN_DIR=${GOLDEN_DIR:-/var/tmp/golden}
COW_SIZE=${COW_SIZE:-8G}
CPUS=${CPUS:-$(($(nproc --all) > 1 ? $(nproc --all) - 1 : 1))}
RAM_MB=${RAM_MB:-7900}
GUEST_SSH_KEY=${GUEST_SSH_KEY:-$HOME/.ssh/hosted}
GUEST_USER=${GUEST_USER:-xnu}
BOOT_TIMEOUT=${BOOT_TIMEOUT:-90}
# the host's translation granule picks XNU's build (its page tables are the hardware's) and the snapshot's chunk size (a
# dm-snapshot chunk is at least a page: 16 sectors on a 4 KB host, 32 on a 16 KB one)
PAGESZ=$(getconf PAGESIZE)
if [ "$PAGESZ" = 16384 ]; then
  IMAGE=${IMAGE:-xnu/kc16k.bin}; COW_CHUNK=${COW_CHUNK:-$((PAGESZ / 512))}
else
  IMAGE=${IMAGE:-xnu/kc.bin}; COW_CHUNK=${COW_CHUNK:-16}
fi
JOB_SCRIPT=""

usage() {
  cat >&2 << 'EOF'
usage: ci-ephemeral-run.sh [--golden DIR] [--cow-size 8G] [--cpus N] [--ram-mb N] [--ssh-key PATH] --job SCRIPT_PATH
EOF
  exit 2
}

while [ $# -gt 0 ]; do
  case "$1" in
    --golden) GOLDEN_DIR=${2:?}; shift 2;;
    --cow-size) COW_SIZE=${2:?}; shift 2;;
    --cpus) CPUS=${2:?}; shift 2;;
    --ram-mb) RAM_MB=${2:?}; shift 2;;
    --ssh-key) GUEST_SSH_KEY=${2:?}; shift 2;;
    --job) JOB_SCRIPT=${2:?}; shift 2;;
    -h|--help) usage;;
    *) echo "unknown option: $1" >&2; usage;;
  esac
done

[ -n "$JOB_SCRIPT" ] && [ -r "$JOB_SCRIPT" ] || { echo "ci-ephemeral-run: --job SCRIPT_PATH required" >&2; exit 2; }
[ -s "$GOLDEN_DIR/mac.img" ] || { echo "ci-ephemeral-run: missing $GOLDEN_DIR/mac.img" >&2; exit 1; }
[ -r "$GUEST_SSH_KEY" ] || { echo "ci-ephemeral-run: missing guest key $GUEST_SSH_KEY" >&2; exit 1; }

RUN_ID="$$"
DM_NAME="xnu-ci-cow-${RUN_ID}"
JOB_DIR="/dev/shm/xnu-ci-job-${RUN_ID}"
COW_FILE="/dev/shm/xnu-ci-cow-${RUN_ID}.img"
BASE_LOOP=""
COW_LOOP=""
DM_ACTIVE=0

cleanup() {
  local rc=$?
  set +e
  (cd "$POC_DIR" && FAST_STOP=1 bash scripts/xnulinux-boot.sh stop >/dev/null 2>&1) || true
  if [ "$DM_ACTIVE" = 1 ]; then
    sudo dmsetup remove --retry "$DM_NAME" >/dev/null 2>&1 || { sleep 0.5; sudo dmsetup remove --retry "$DM_NAME" >/dev/null 2>&1 || true; }
  fi
  [ -n "$COW_LOOP" ] && sudo losetup -d "$COW_LOOP" >/dev/null 2>&1 || true
  [ -n "$BASE_LOOP" ] && sudo losetup -d "$BASE_LOOP" >/dev/null 2>&1 || true
  rm -rf "$COW_FILE" "$JOB_DIR"
  exit "$rc"
}
trap cleanup EXIT INT TERM

mkdir -p "$JOB_DIR"
if [ -s "$GOLDEN_DIR/keystore.bin" ]; then
  cp "$GOLDEN_DIR/keystore.bin" "$JOB_DIR/keystore.bin"
else
  truncate -s 1M "$JOB_DIR/keystore.bin"
fi
: > "$JOB_DIR/console-in"
: > "$JOB_DIR/input"
: > "$JOB_DIR/fb.raw"

truncate -s "$COW_SIZE" "$COW_FILE"
BASE_LOOP=$(sudo losetup --find --show --read-only "$GOLDEN_DIR/mac.img")
SECTORS=$(sudo blockdev --getsz "$BASE_LOOP")
COW_LOOP=$(sudo losetup --find --show "$COW_FILE")
sudo dmsetup create "$DM_NAME" --table "0 $SECTORS snapshot $BASE_LOOP $COW_LOOP N $COW_CHUNK"
DM_ACTIVE=1

boot_t0=$(python3 -c 'import time; print(time.monotonic())')
GUEST_IP=""
for attempt in 1 2; do
  (
    cd "$POC_DIR"
    CI_HEADLESS=1 FAST_BOOT=1 FAST_STOP=1 \
      CPUS="$CPUS" RAM_MB="$RAM_MB" \
      DISK="/dev/mapper/$DM_NAME" IMAGE="$IMAGE" \
      KEYSTORE="$JOB_DIR/keystore.bin" \
      CONSOLE_IN="$JOB_DIR/console-in" \
      INPUT_IN="$JOB_DIR/input" \
      FB_DUMP="$JOB_DIR/fb.raw" \
      bash scripts/xnulinux-boot.sh
  )

  deadline=$((SECONDS + BOOT_TIMEOUT))
  failed=0
  while [ "$SECONDS" -lt "$deadline" ]; do
    if sudo dmesg | grep -aqE "XNU panic|userspace panic|posix_spawn.*14|Bad address|run ended by"; then
      echo "ci-ephemeral-run: early boot failure on attempt $attempt, retrying..." >&2
      failed=1
      break
    fi
    # vm-net-up.sh keeps xnu0's leases in /run/dnsmasq-xnu0.leases (the guest's MAC: 02:58:4e:55:00:01)
    gip=$(awk '$2 == "02:58:4e:55:00:01" {ip = $3} END {if (ip) print ip}' /run/dnsmasq-xnu0.leases 2>/dev/null || true)
    [ -n "$gip" ] || gip=$(awk '{print $3; exit}' /var/lib/misc/dnsmasq.leases 2>/dev/null || true)
    if [ -n "$gip" ] && ssh -i "$GUEST_SSH_KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR -o ConnectTimeout=3 "${GUEST_USER}@${gip}" true 2>/dev/null; then
      GUEST_IP="$gip"
      break
    fi
    sleep 0.5
  done
  [ -n "$GUEST_IP" ] && break
  (cd "$POC_DIR" && FAST_STOP=1 bash scripts/xnulinux-boot.sh stop >/dev/null 2>&1) || true
  [ "$attempt" -eq 2 ] && { echo "ci-ephemeral-run: guest failed to boot SSH in 2 attempts" >&2; exit 1; }
  [ "$failed" -eq 1 ] && cp "${GOLDEN_DIR}/keystore.bin" "$JOB_DIR/keystore.bin" 2>/dev/null || true
done

boot_dt=$(python3 - "$boot_t0" << 'PY'
import sys, time
print(f"{time.monotonic() - float(sys.argv[1]):.2f}")
PY
)
echo "CI_EPHEMERAL_BOOT_S=$boot_dt guest=$GUEST_IP dm=$DM_NAME image=$IMAGE chunk=$COW_CHUNK"

job_t0=$(python3 -c 'import time; print(time.monotonic())')
set +e
ssh -i "$GUEST_SSH_KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
  -o LogLevel=ERROR -o ConnectTimeout=10 "${GUEST_USER}@${GUEST_IP}" bash -s < "$JOB_SCRIPT"
job_rc=$?
set -e
job_dt=$(python3 - "$job_t0" << 'PY'
import sys, time
print(f"{time.monotonic() - float(sys.argv[1]):.2f}")
PY
)

tear_t0=$(python3 -c 'import time; print(time.monotonic())')
(cd "$POC_DIR" && FAST_STOP=1 bash scripts/xnulinux-boot.sh stop >/dev/null 2>&1) || true
sudo dmsetup remove --retry "$DM_NAME" || { sleep 0.5; sudo dmsetup remove --retry "$DM_NAME"; }
DM_ACTIVE=0
sudo losetup -d "$COW_LOOP"; COW_LOOP=""
sudo losetup -d "$BASE_LOOP"; BASE_LOOP=""
rm -rf "$COW_FILE" "$JOB_DIR"
tear_dt=$(python3 - "$tear_t0" << 'PY'
import sys, time
print(f"{time.monotonic() - float(sys.argv[1]):.2f}")
PY
)
echo "CI_EPHEMERAL_DONE boot_s=$boot_dt job_s=$job_dt teardown_s=$tear_dt job_rc=$job_rc"
exit "$job_rc"

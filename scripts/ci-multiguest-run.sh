#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Several ephemeral headless macOS guests at once on one Linux host (xnu.ko guests, /sys/kernel/xnu/ctl). Guest N gets:
# its own CPUs (one of --cpusets), its own copy-on-write disk (dm-snapshot over one shared read-only loop device of the
# golden mac.img, the CoW file on disk under --cow-dir), its own keystore copy, console input and console log, its own
# link xnuN (192.168.(77+N).0/24, MAC 02:58:4e:55:00:(01+N), its own dnsmasq and lease file). Guests cannot reach each
# other (FORWARD DROP between xnu links). The host's interrupts, unbound workqueues, receive steering and its own processes
# (systemd slices) stay on --host-cpus while it runs; what was set before is put back at the end.
#
# Every guest boots at once, each with up to two attempts (supervised through its console.log and its line in
# /sys/kernel/xnu/status, never through dmesg -C), until SSH answers at the address dnsmasq leased to its MAC; then every
# guest's job starts at the same moment; when the last one ends, every guest is stopped and reaped and its disk, loop
# devices and CoW file removed.
#
# Usage: ci-multiguest-run.sh --cpusets "1-3 4-6" --host-cpus 0,7 --jobs "JOB [JOB ...]" [--golden DIR] [--ram-mb N]
#        [--cow-size 8G] [--cow-dir /var/tmp/xnu-ci] [--ssh-key PATH] [--boot-timeout S]
#   --jobs: one script for every guest, or one per guest ("-": none, boot and check only)
set -euo pipefail

POC_DIR=$(cd "$(dirname "$0")/.." && pwd)
_save_env=()
for _v in GOLDEN_DIR RAM_MB COW_SIZE COW_DIR GUEST_SSH_KEY GUEST_USER BOOT_TIMEOUT BOOTARGS IMAGE TRUSTCACHE; do
  [ "${!_v+x}" = x ] && _save_env+=("$_v=${!_v}")
done
[ -f /etc/xnulinux.conf ] && . /etc/xnulinux.conf
if [ "${#_save_env[@]}" -gt 0 ]; then
  for _kv in "${_save_env[@]}"; do printf -v "${_kv%%=*}" "%s" "${_kv#*=}"; done
fi
unset _save_env _v _kv

GOLDEN_DIR=${GOLDEN_DIR:-/var/tmp/golden}
CPUSETS=""; HOST_CPUS=""; JOBS=""
RAM_MB=${RAM_MB:-7900}
COW_SIZE=${COW_SIZE:-8G}
COW_DIR=${COW_DIR:-/var/tmp/xnu-ci}
GUEST_SSH_KEY=${GUEST_SSH_KEY:-$HOME/.ssh/hosted}
GUEST_USER=${GUEST_USER:-xnu}
BOOT_TIMEOUT=${BOOT_TIMEOUT:-120}

usage() {
  sed -n '/^# Usage:/,/^set -euo/p' "$0" | sed '$d' >&2
  exit 2
}
while [ $# -gt 0 ]; do
  case "$1" in
    --golden) GOLDEN_DIR=${2:?}; shift 2;;
    --cpusets) CPUSETS=${2:?}; shift 2;;
    --host-cpus) HOST_CPUS=${2:?}; shift 2;;
    --ram-mb) RAM_MB=${2:?}; shift 2;;
    --cow-size) COW_SIZE=${2:?}; shift 2;;
    --cow-dir) COW_DIR=${2:?}; shift 2;;
    --jobs) JOBS=${2:?}; shift 2;;
    --ssh-key) GUEST_SSH_KEY=${2:?}; shift 2;;
    --boot-timeout) BOOT_TIMEOUT=${2:?}; shift 2;;
    -h|--help) usage;;
    *) echo "ci-multiguest-run: unknown option: $1" >&2; usage;;
  esac
done

read -r -a SETS <<< "$CPUSETS"
read -r -a JOBV <<< "$JOBS"
N=${#SETS[@]}
[ "$N" -ge 1 ] || { echo "ci-multiguest-run: --cpusets required" >&2; exit 2; }
[ "$N" -le 4 ] || { echo "ci-multiguest-run: at most 4 guests" >&2; exit 2; }
[ -n "$HOST_CPUS" ] || { echo "ci-multiguest-run: --host-cpus required" >&2; exit 2; }
[ "${#JOBV[@]}" -ge 1 ] || { echo "ci-multiguest-run: --jobs required" >&2; exit 2; }
for j in "${JOBV[@]}"; do [ "$j" = - ] || [ -r "$j" ] || { echo "ci-multiguest-run: no job script $j" >&2; exit 2; }; done
[ "${#JOBV[@]}" = 1 ] || [ "${#JOBV[@]}" = "$N" ] || { echo "ci-multiguest-run: one job, or one per guest" >&2; exit 2; }
[ -s "$GOLDEN_DIR/mac.img" ] || { echo "ci-multiguest-run: missing $GOLDEN_DIR/mac.img" >&2; exit 1; }
[ -r "$GUEST_SSH_KEY" ] || { echo "ci-multiguest-run: missing guest key $GUEST_SSH_KEY" >&2; exit 1; }

# golden disk's volume UUIDs only (preserve explicit CLI/env RAM_MB/BOOTARGS)
_s_ram=$RAM_MB; _s_ba=${BOOTARGS-}
if [ -f "$GOLDEN_DIR/mac.conf" ]; then . "$GOLDEN_DIR/mac.conf"; elif [ -f /var/tmp/mac.conf ]; then . /var/tmp/mac.conf; fi
RAM_MB=$_s_ram; [ -n "$_s_ba" ] && BOOTARGS=$_s_ba
unset _s_ram _s_ba
: "${ROOT_UUID:?no ROOT_UUID (mac.conf)}" "${BOOT_UUID:?no BOOT_UUID}"
BOOTARGS=${BOOTARGS:-amfi_get_out_of_my_way=1 lp-sip0=2050}
case " $BOOTARGS " in *" validation_disables="*) ;; *) BOOTARGS="$BOOTARGS validation_disables=0x20924" ;; esac
case " $BOOTARGS " in *" xnu-headless=1 "*) ;; *) BOOTARGS="$BOOTARGS xnu-headless=1" ;; esac
case " $BOOTARGS " in *" linux64_idle_poll_us="*) ;; *) BOOTARGS="$BOOTARGS linux64_idle_poll_us=0" ;; esac
TRUSTCACHE=${TRUSTCACHE:-xnu/trustcache-mac.bin}
PAGESZ=$(getconf PAGESIZE)
if [ "$PAGESZ" = 16384 ]; then
  IMAGE=${IMAGE:-xnu/kc16k.bin}; COW_CHUNK=$((PAGESZ / 512)); BLOCK_MB=32; DEF_CARVE_BASE=0x600000000
else
  IMAGE=${IMAGE:-xnu/kc.bin}; COW_CHUNK=16; BLOCK_MB=2; DEF_CARVE_BASE=0x200000000
fi

cpulist_mask() {	# "0,7" or "0-1,7" -> hex mask
  local m=0 part lo hi c
  IFS=, read -r -a parts <<< "$1"
  for part in "${parts[@]}"; do
    lo=${part%-*}; hi=${part#*-}
    for ((c = lo; c <= hi; c++)); do m=$((m | (1 << c))); done
  done
  printf '%x' "$m"
}
gstate() {	# guest N's state in /sys/kernel/xnu/status
  awk -v id="guest $1:" 'index($0, id) == 1 { for (i = 1; i <= NF; i++) if ($i ~ /^state=/) { sub("state=", "", $i); print $i } }' \
    /sys/kernel/xnu/status 2>/dev/null || true
}
ctl() { sudo bash -c 'printf "%s\n" "$1" > /sys/kernel/xnu/ctl' _ "$1"; }	# one write (some tee builds retry a failed one)
now() { python3 -c 'import time; print(f"{time.monotonic():.2f}")'; }
since() { python3 -c "import sys,time; print(f'{time.monotonic() - float(sys.argv[1]):.2f}')" "$1"; }
SSHO=(-i "$GUEST_SSH_KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)

# ---- modules: fki with a link per guest and room for every guest's DRAM, xnu.ko in control mode (mode=0) ----
K=/lib/modules/$(uname -r)/build
build() {
  local log; log=$(make -s -C "$K" M="$POC_DIR/$1" ${2:-} modules 2>&1) || { echo "$log" | grep -E "error|Error" | head -20 >&2; exit 1; }
}
build fki-ko
build xnu-ko "KBUILD_EXTRA_SYMBOLS=$POC_DIR/fki-ko/Module.symvers"
NEED_MB=$((N * ((RAM_MB + 160 + BLOCK_MB - 1) / BLOCK_MB * BLOCK_MB)))
XNU_LOADED_HERE=0
if ! lsmod | grep -q '^xnu '; then
  cur_nets=$(cat /sys/module/fki/parameters/nets 2>/dev/null || echo 0)
  cur_mb=$(cat /sys/module/fki/parameters/carve_mb 2>/dev/null || echo 0)
  cur_base=$(cat /sys/module/fki/parameters/carve_base 2>/dev/null || echo 0)
  if [ "$(cat /sys/module/fki/srcversion 2>/dev/null)" != "$(modinfo -F srcversion "$POC_DIR/fki-ko/fki.ko")" ] ||
     [ "$cur_nets" -lt "$N" ] || [ "$cur_mb" -lt "$NEED_MB" ]; then
    base=$cur_base; [ "$base" = 0 ] && base=$DEF_CARVE_BASE
    mb=$cur_mb; [ "$mb" -lt "$NEED_MB" ] && mb=$NEED_MB
    nets=$cur_nets; [ "$nets" -lt "$N" ] && nets=$N
    sudo systemctl stop xnudrmd 2>/dev/null || true
    sudo rmmod fki 2>/dev/null || true
    sudo insmod "$POC_DIR/fki-ko/fki.ko" carve_base="$base" carve_mb="$mb" nets="$nets"
    echo "ci-multiguest-run: fki carve_base=$base carve_mb=$mb nets=$nets"
  fi
  sudo insmod "$POC_DIR/xnu-ko/xnu.ko" mode=0 host_cpus="$HOST_CPUS"
  XNU_LOADED_HERE=1
else
  [ "$(cat /sys/module/xnu/parameters/mode)" = 0 ] || { echo "ci-multiguest-run: xnu.ko runs a legacy guest (mode!=0)" >&2; exit 1; }
fi
[ -w /sys/kernel/xnu/ctl ] || [ -e /sys/kernel/xnu/ctl ] || { echo "ci-multiguest-run: no /sys/kernel/xnu/ctl" >&2; exit 1; }
sudo dmesg -n "${CONLEVEL:-1}"

# ---- state and cleanup trap (installed before host tuning so failures restore cleanly) ----
RUN=$$
mkdir -p "$COW_DIR"
BASE_LOOP=""
pids=()
declare -a DM COWF COWL JD
WQ=/sys/devices/virtual/workqueue/cpumask
WQ_OLD=""; IRQ_DEF_OLD=""
declare -A IRQ_OLD RPS_OLD
SLICES=(system.slice user.slice init.scope)
host_restore() {
  for s in "${SLICES[@]}"; do sudo systemctl set-property --runtime "$s" AllowedCPUs= 2>/dev/null || true; done
  [ -n "$WQ_OLD" ] && echo "$WQ_OLD" | sudo tee "$WQ" >/dev/null 2>&1 || true
  for q in "${!RPS_OLD[@]}"; do echo "${RPS_OLD[$q]}" | sudo tee "$q" >/dev/null 2>&1 || true; done
  [ -n "$IRQ_DEF_OLD" ] && echo "$IRQ_DEF_OLD" | sudo tee /proc/irq/default_smp_affinity >/dev/null 2>&1 || true
  for f in "${!IRQ_OLD[@]}"; do [ -n "${IRQ_OLD[$f]}" ] && echo "${IRQ_OLD[$f]}" | sudo tee "$f" >/dev/null 2>&1 || true; done
  return 0
}
zombie_present() {
  local idx st
  for ((idx = 0; idx < N; idx++)); do
    st=$(gstate "$idx")
    if [ "$st" = zombie ]; then
      echo "ci-multiguest-run: FATAL guest $idx is ZOMBIE; leaving other guests untouched and skipping rmmod xnu (VM reset required)" >&2
      return 0
    fi
  done
  return 1
}
cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  set +e
  if [ "${#pids[@]}" -gt 0 ]; then
    kill "${pids[@]}" 2>/dev/null
    wait "${pids[@]}" 2>/dev/null || true
    pids=()
  fi
  if zombie_present; then
    host_restore
    exit 1
  fi
  for ((i = 0; i < N; i++)); do
    st=$(gstate "$i")
    if [ -n "$st" ] && [ "$st" != stopped ] && [ "$st" != free ]; then ctl "stop id=$i" 2>/dev/null; fi
  done
  for ((i = 0; i < N; i++)); do
    for _ in $(seq 1 600); do st=$(gstate "$i"); [ -z "$st" ] || [ "$st" = stopped ] || [ "$st" = zombie ] && break; sleep 0.1; done
    if [ "$(gstate "$i")" = zombie ]; then
      echo "ci-multiguest-run: FATAL guest $i entered ZOMBIE; aborting teardown and skipping rmmod xnu (VM reset required)" >&2
      host_restore
      exit 1
    fi
    [ "$(gstate "$i")" = stopped ] && ctl "reap id=$i" 2>/dev/null
    [ -n "${JD[$i]:-}" ] && [ -f "${JD[$i]}/console.log" ] && cp "${JD[$i]}/console.log" "$COW_DIR/last-g$i-console.log" 2>/dev/null
    if [ -n "${DM[$i]:-}" ]; then
      sudo dmsetup remove --retry "${DM[$i]}" >/dev/null 2>&1 || { sleep 0.5; sudo dmsetup remove --retry "${DM[$i]}" >/dev/null 2>&1; }
      DM[$i]=""
    fi
    local lp=""
    [ -n "${JD[$i]:-}" ] && lp=$(cat "${JD[$i]}/loop" 2>/dev/null || true)
    lp=${lp:-${COWL[$i]:-}}
    [ -n "$lp" ] && sudo losetup -d "$lp" >/dev/null 2>&1
    COWL[$i]=""
    [ -n "${COWF[$i]:-}" ] && rm -f "${COWF[$i]}"
    COWF[$i]=""
    [ -n "${JD[$i]:-}" ] && rm -rf "${JD[$i]}"
    JD[$i]=""
  done
  [ -n "$BASE_LOOP" ] && sudo losetup -d "$BASE_LOOP" >/dev/null 2>&1
  BASE_LOOP=""
  host_restore
  if [ "$XNU_LOADED_HERE" = 1 ]; then sudo rmmod xnu 2>/dev/null; fi
  exit "$rc"
}
trap cleanup EXIT INT TERM

# ---- the host's own work on the host CPUs (once) ----
MASK=$(cpulist_mask "$HOST_CPUS")
WQ_OLD=$(cat "$WQ" 2>/dev/null || true)
IRQ_DEF_OLD=$(cat /proc/irq/default_smp_affinity 2>/dev/null || true)
for f in /proc/irq/*/smp_affinity; do IRQ_OLD[$f]=$(cat "$f" 2>/dev/null || true); done
echo "$MASK" | sudo tee /proc/irq/default_smp_affinity >/dev/null 2>&1 || true
for f in /proc/irq/*/smp_affinity; do echo "$MASK" | sudo tee "$f" >/dev/null 2>&1 || true; done
[ -e "$WQ" ] && { echo "$MASK" | sudo tee "$WQ" >/dev/null 2>&1 || true; }
UP=$(ip -o route show default | awk '{for(i=1;i<=NF;i++) if($i=="dev"){print $(i+1); exit}}')
if [ -n "${UP:-}" ]; then
  for q in /sys/class/net/"$UP"/queues/rx-*/rps_cpus; do
    [ -e "$q" ] || continue
    RPS_OLD[$q]=$(cat "$q" 2>/dev/null || true)
    echo "$MASK" | sudo tee "$q" >/dev/null 2>&1 || true
  done
  sudo ethtool -K "$UP" gro off 2>/dev/null || true
fi
for s in "${SLICES[@]}"; do sudo systemctl set-property --runtime "$s" AllowedCPUs="$HOST_CPUS" 2>/dev/null || true; done

# ---- per-guest disks and files ----
BASE_LOOP=$(sudo losetup --find --show --read-only "$GOLDEN_DIR/mac.img")
SECTORS=$(sudo blockdev --getsz "$BASE_LOOP")
for ((i = 0; i < N; i++)); do
  st=$(gstate "$i")
  [ -z "$st" ] || { echo "ci-multiguest-run: guest $i is $st already" >&2; exit 1; }
  JD[$i]="$COW_DIR/run-$RUN-g$i"; mkdir -p "${JD[$i]}"
  if [ -s "$GOLDEN_DIR/keystore.bin" ]; then cp "$GOLDEN_DIR/keystore.bin" "${JD[$i]}/keystore.bin"; else truncate -s 1M "${JD[$i]}/keystore.bin"; fi
  : > "${JD[$i]}/console-in"
  : > "${JD[$i]}/console.log"
  COWF[$i]="$COW_DIR/run-$RUN-g$i.cow"
  truncate -s "$COW_SIZE" "${COWF[$i]}"
  COWL[$i]=$(sudo losetup --find --show "${COWF[$i]}")
  echo "${COWL[$i]}" > "${JD[$i]}/loop"
  DM[$i]="xnu-ci-$RUN-g$i"
  sudo dmsetup create "${DM[$i]}" --table "0 $SECTORS snapshot $BASE_LOOP ${COWL[$i]} N $COW_CHUNK"
  bash "$POC_DIR/scripts/vm-net-up.sh" "xnu$i" $((77 + i)) >/dev/null
done

# ---- boot: every guest at once, each supervised on its own ----
guest_boot() {
  trap - EXIT INT TERM
  local i=$1 jd=${JD[$1]} mac set ip="" t0 off st lp
  mac=02:58:4e:55:00:$(printf %02x $((1 + i)))
  set=${SETS[$i]}
  t0=$(now)
  for attempt in 1 2; do
    : > "$jd/console-in"
    off=$(stat -c %s "$jd/console.log")
    if ! ctl "start id=$i cpus=$set ram_mb=$RAM_MB image=$IMAGE trustcache=$TRUSTCACHE disk=/dev/mapper/${DM[$i]} root_uuid=$ROOT_UUID boot_uuid=$BOOT_UUID preboot_uuid=$BOOT_UUID keystore=$jd/keystore.bin console_in=$jd/console-in console_log=$jd/console.log mac_id=$i net=xnu$i fb_width=0 fb_height=0 bootargs="$BOOTARGS"" 2> "$jd/start.err"; then
      echo "ci-multiguest-run: guest $i did not start (attempt $attempt): $(cat "$jd/start.err")" >&2
    else
      local deadline=$((SECONDS + BOOT_TIMEOUT)) failed=0
      while [ "$SECONDS" -lt "$deadline" ]; do
        if tail -c +$((off + 1)) "$jd/console.log" | grep -aqE "XNU panic|userspace panic|posix_spawn.*14|Bad address|run ended by"; then
          failed=1; break
        fi
        st=$(gstate "$i"); [ "$st" = running ] || { failed=1; break; }
        ip=$(awk -v m="$mac" '$2 == m {ip = $3} END {if (ip) print ip}' "/run/dnsmasq-xnu$i.leases" 2>/dev/null || true)
        if [ -n "$ip" ] && ssh "${SSHO[@]}" -o ConnectTimeout=3 "$GUEST_USER@$ip" true 2>/dev/null; then
          echo "$ip" > "$jd/ip"
          echo "CI_MULTIGUEST_BOOT g=$i cpus=$set ip=$ip mac=$mac attempt=$attempt boot_s=$(since "$t0")"
          return 0
        fi
        sleep 0.5
      done
      echo "ci-multiguest-run: guest $i failed to boot to SSH (attempt $attempt$([ $failed = 1 ] && echo ", early failure"))" >&2
      ctl "stop id=$i" 2>/dev/null || true
      for _ in $(seq 1 600); do st=$(gstate "$i"); [ "$st" = stopped ] || [ "$st" = zombie ] && break; sleep 0.1; done
      st=$(gstate "$i")
      if [ "$st" = zombie ]; then
        echo "ci-multiguest-run: FATAL guest $i is ZOMBIE during retry (VM reset required)" >&2
        return 1
      fi
      if [ "$st" = stopped ]; then
        ctl "reap id=$i"
        if [ "$attempt" = 1 ]; then
          sudo dmsetup remove --retry "${DM[$i]}" >/dev/null 2>&1 || { sleep 0.5; sudo dmsetup remove --retry "${DM[$i]}"; }
          lp=$(cat "$jd/loop" 2>/dev/null || true)
          [ -n "$lp" ] && sudo losetup -d "$lp" >/dev/null 2>&1 || true
          rm -f "${COWF[$i]}"
          truncate -s "$COW_SIZE" "${COWF[$i]}"
          lp=$(sudo losetup --find --show "${COWF[$i]}")
          echo "$lp" > "$jd/loop"
          sudo dmsetup create "${DM[$i]}" --table "0 $SECTORS snapshot $BASE_LOOP $lp N $COW_CHUNK"
        fi
      fi
      [ -s "$GOLDEN_DIR/keystore.bin" ] && cp "$GOLDEN_DIR/keystore.bin" "$jd/keystore.bin"
    fi
  done
  touch "$jd/failed"
  return 1
}
BOOT_T0=$(now)
pids=()
for ((i = 0; i < N; i++)); do guest_boot "$i" & pids+=($!); done
bootfail=0
for p in "${pids[@]}"; do wait "$p" || bootfail=$((bootfail + 1)); done
pids=()
echo "CI_MULTIGUEST_BOOTED guests=$N failed=$bootfail all_s=$(since "$BOOT_T0")"
[ "$bootfail" = 0 ] || exit 1
cat /sys/kernel/xnu/status

# ---- jobs: started together, each guest's to its own log ----
run_job() {
  trap - EXIT INT TERM
  local i=$1 job=$2 jd=${JD[$1]} t0 rc
  t0=$(now)
  set +e
  ssh "${SSHO[@]}" -o ConnectTimeout=10 "$GUEST_USER@$(cat "$jd/ip")" bash -s < "$job" > "$jd/job.log" 2>&1
  rc=$?
  set -e
  cp "$jd/job.log" "$COW_DIR/last-g$i-job.log" 2>/dev/null || true
  echo "CI_MULTIGUEST_JOB g=$i rc=$rc job_s=$(since "$t0")"
  return "$rc"
}
JOB_T0=$(now)
pids=()
for ((i = 0; i < N; i++)); do
  job=${JOBV[0]}; [ "${#JOBV[@]}" = "$N" ] && job=${JOBV[$i]}
  [ "$job" = - ] && continue
  run_job "$i" "$job" & pids+=($!)
done
jobfail=0
for p in "${pids[@]}"; do wait "$p" || jobfail=$((jobfail + 1)); done
pids=()
JOB_S=$(since "$JOB_T0")
cat /sys/kernel/xnu/status

# ---- teardown ----
TEAR_T0=$(now)
for ((i = 0; i < N; i++)); do ctl "stop id=$i" 2>/dev/null || true; done
for ((i = 0; i < N; i++)); do
  for _ in $(seq 1 600); do st=$(gstate "$i"); [ "$st" = stopped ] || [ "$st" = zombie ] && break; sleep 0.05; done
  st=$(gstate "$i")
  [ "$st" = stopped ] || { echo "ci-multiguest-run: guest $i did not stop ($st)" >&2; exit 1; }
  ctl "reap id=$i"
  cp "${JD[$i]}/console.log" "$COW_DIR/last-g$i-console.log" 2>/dev/null || true
  sudo dmsetup remove --retry "${DM[$i]}" || { sleep 0.5; sudo dmsetup remove --retry "${DM[$i]}"; }
  DM[$i]=""
  lp=$(cat "${JD[$i]}/loop" 2>/dev/null || echo "${COWL[$i]:-}")
  [ -n "$lp" ] && sudo losetup -d "$lp"
  COWL[$i]=""
  rm -f "${COWF[$i]}"; COWF[$i]=""
  rm -rf "${JD[$i]}"; JD[$i]=""
done
sudo losetup -d "$BASE_LOOP"; BASE_LOOP=""
echo "CI_MULTIGUEST_DONE guests=$N job_fail=$jobfail job_s=$JOB_S teardown_s=$(since "$TEAR_T0")"
[ "$jobfail" = 0 ]

#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# In the VM: undo vm-net-up.sh for one link (its dnsmasq, its address, its NAT and FORWARD rules). The guest-to-guest DROP
# rule stays while any other xnu link still has an address.
#
# Usage: vm-net-down.sh [iface] [octet]    (defaults: xnu0 77)
set -u
IF=${1:-xnu0}; OCT=${2:-77}
case "$IF" in xnu[0-9]|xnu[0-9][0-9]) ;; *) echo "vm-net-down: the interface is xnuN, not '$IF'" >&2; exit 2;; esac
case "$OCT" in ''|*[!0-9]*) echo "vm-net-down: bad octet '$OCT'" >&2; exit 2;; esac
NET=192.168.$OCT
PIDF=/run/dnsmasq-$IF.pid
if [ -s "$PIDF" ]; then
  pid=$(cat "$PIDF" 2>/dev/null || true)
  if [ -n "$pid" ] && tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q -- "--interface=$IF "; then
    sudo kill "$pid" 2>/dev/null || true
    for _ in $(seq 1 30); do [ -e "/proc/$pid" ] || break; sleep 0.1; done
  fi
fi
sudo rm -f "$PIDF"
up=$(ip -o route show default | awk '{for(i=1;i<=NF;i++) if($i=="dev"){print $(i+1); exit}}')
if [ -n "${up:-}" ]; then
  while sudo iptables -t nat -C POSTROUTING -s "$NET.0/24" -o "$up" -j MASQUERADE 2>/dev/null; do
    sudo iptables -t nat -D POSTROUTING -s "$NET.0/24" -o "$up" -j MASQUERADE
  done
fi
for rule in "-o $IF -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT" "-i $IF -j ACCEPT" \
            "-i $IF -m conntrack --ctstate INVALID -j DROP"; do
  # shellcheck disable=SC2086
  while sudo iptables -C FORWARD $rule 2>/dev/null; do sudo iptables -D FORWARD $rule; done
done
if ip link show "$IF" >/dev/null 2>&1; then
  sudo ip addr del "$NET.1/24" dev "$IF" 2>/dev/null || true
  sudo ip link set "$IF" down 2>/dev/null || true
fi
others=$(ip -o -4 addr show 2>/dev/null | awk '{print $2}' | grep -E '^xnu[0-9]+$' | grep -vx "$IF" || true)
if [ -z "$others" ]; then
  while sudo iptables -C FORWARD -i xnu+ -o xnu+ -j DROP 2>/dev/null; do sudo iptables -D FORWARD -i xnu+ -o xnu+ -j DROP; done
fi
echo "vm-net-down: $IF ($NET.0/24) down"

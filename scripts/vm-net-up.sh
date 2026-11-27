#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# In the VM, after fki.ko is loaded: the host side of a hosted macOS's en0. fki.ko's netdev xnuN carries one guest's en0
# frames; Linux routes and NATs 192.168.<octet>.0/24 out of its own uplink, and dnsmasq (when installed) answers DHCP and
# DNS on that link (its own pid file and lease file per link).
#
# Usage: vm-net-up.sh [iface] [octet]      (defaults: xnu0 77; guest N of several: xnuN 77+N)
#
# Guests never reach each other: frames forwarded from one xnu link to another are dropped (the first FORWARD rule).
set -u
IF=${1:-xnu0}; OCT=${2:-77}
case "$IF" in xnu[0-9]|xnu[0-9][0-9]) ;; *) echo "vm-net-up: the interface is xnuN, not '$IF'" >&2; exit 2;; esac
case "$OCT" in ''|*[!0-9]*) echo "vm-net-up: bad octet '$OCT'" >&2; exit 2;; esac
[ "$OCT" -ge 1 ] && [ "$OCT" -le 254 ] || { echo "vm-net-up: octet $OCT out of range" >&2; exit 2; }
NET=192.168.$OCT
PIDF=/run/dnsmasq-$IF.pid
LEASES=/run/dnsmasq-$IF.leases
ip link show "$IF" >/dev/null 2>&1 || { echo "vm-net-up: no $IF (fki.ko without the link: load it with nets=N)"; exit 0; }
up=$(ip -o route show default | awk '{for(i=1;i<=NF;i++) if($i=="dev"){print $(i+1); exit}}')
if [ -n "${up:-}" ]; then sudo ethtool -K "$up" gro off 2>/dev/null || true; sudo ethtool -K "$up" rx-gro-hw off 2>/dev/null || true; fi
mtu=1500
if [ -n "${up:-}" ] && [ -r "/sys/class/net/$up/mtu" ]; then
  up_mtu=$(cat "/sys/class/net/$up/mtu")
  if [ -n "$up_mtu" ] && [ "$up_mtu" -ge 576 ] && [ "$up_mtu" -lt "$mtu" ]; then
    mtu=$up_mtu
  fi
fi
sudo ip addr replace "$NET.1/24" dev "$IF"
sudo ip link set dev "$IF" mtu "$mtu"
sudo ip link set "$IF" up
sudo sysctl -q -w net.ipv4.ip_forward=1
if [ -n "${up:-}" ]; then
  sudo iptables -t nat -C POSTROUTING -s "$NET.0/24" -o "$up" -j MASQUERADE 2>/dev/null ||
    sudo iptables -t nat -A POSTROUTING -s "$NET.0/24" -o "$up" -j MASQUERADE
fi
sudo iptables -t mangle -C FORWARD -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu 2>/dev/null ||
  sudo iptables -t mangle -I FORWARD 1 -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu
sudo iptables -C FORWARD -o "$IF" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT 2>/dev/null ||
  sudo iptables -I FORWARD 1 -o "$IF" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
sudo iptables -C FORWARD -i "$IF" -j ACCEPT 2>/dev/null ||
  sudo iptables -I FORWARD 1 -i "$IF" -j ACCEPT
sudo iptables -C FORWARD -i "$IF" -m conntrack --ctstate INVALID -j DROP 2>/dev/null ||
  sudo iptables -I FORWARD 1 -i "$IF" -m conntrack --ctstate INVALID -j DROP
# guest to guest: dropped, and above every link's ACCEPT (moved back to the top after this link's rules went in)
while sudo iptables -C FORWARD -i xnu+ -o xnu+ -j DROP 2>/dev/null; do sudo iptables -D FORWARD -i xnu+ -o xnu+ -j DROP; done
sudo iptables -I FORWARD 1 -i xnu+ -o xnu+ -j DROP
command -v dnsmasq >/dev/null || sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q dnsmasq-base >/dev/null 2>&1
if command -v dnsmasq >/dev/null; then
  # this link's dnsmasq only (its pid file), never another link's
  if [ -s "$PIDF" ]; then
    pid=$(cat "$PIDF" 2>/dev/null || true)
    if [ -n "$pid" ] && tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q -- "--interface=$IF "; then
      sudo kill "$pid" 2>/dev/null || true
      for _ in $(seq 1 30); do [ -e "/proc/$pid" ] || break; sleep 0.1; done
    fi
  fi
  sudo rm -f "$PIDF"
  sudo touch "$LEASES"
  # authoritative: the link is this guest's alone, so a guest asking for the address it had last time (a fresh lease file)
  # is answered at once instead of ignored until its client gives up and starts over (about 14 s)
  sudo dnsmasq --interface="$IF" --bind-interfaces --except-interface=lo --dhcp-range="$NET.2,$NET.50,12h" --dhcp-authoritative \
    --dhcp-option=3,"$NET.1" --dhcp-option=6,"$NET.1" --dhcp-option=26,"$mtu" --pid-file="$PIDF" --dhcp-leasefile="$LEASES"
fi
echo "vm-net-up: $IF $NET.1/24 mtu $mtu up, NAT via ${up:-none}, dnsmasq: $(command -v dnsmasq >/dev/null && echo "yes ($LEASES)" || echo no)"

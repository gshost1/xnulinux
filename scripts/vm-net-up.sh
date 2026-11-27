#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# In the VM, after fki.ko is loaded: the host side of the hosted macOS's en0. fki.ko's netdev xnu0 carries en0's
# frames; Linux routes and NATs 192.168.77.0/24 out of its own uplink, and dnsmasq (when installed) answers DHCP and
# DNS on xnu0.
set -u
ip link show xnu0 >/dev/null 2>&1 || { echo "vm-net-up: no xnu0 (fki.ko without the link)"; exit 0; }
up=$(ip -o route show default | awk '{for(i=1;i<=NF;i++) if($i=="dev"){print $(i+1); exit}}')
if [ -n "${up:-}" ]; then sudo ethtool -K "$up" gro off 2>/dev/null || true; sudo ethtool -K "$up" rx-gro-hw off 2>/dev/null || true; fi
mtu=1500
if [ -n "${up:-}" ] && [ -r "/sys/class/net/$up/mtu" ]; then
  up_mtu=$(cat "/sys/class/net/$up/mtu")
  if [ -n "$up_mtu" ] && [ "$up_mtu" -ge 576 ] && [ "$up_mtu" -lt "$mtu" ]; then
    mtu=$up_mtu
  fi
fi
sudo ip addr replace 192.168.77.1/24 dev xnu0
sudo ip link set dev xnu0 mtu "$mtu"
sudo ip link set xnu0 up
sudo sysctl -q -w net.ipv4.ip_forward=1
if [ -n "${up:-}" ]; then
  sudo iptables -t nat -C POSTROUTING -s 192.168.77.0/24 -o "$up" -j MASQUERADE 2>/dev/null ||
    sudo iptables -t nat -A POSTROUTING -s 192.168.77.0/24 -o "$up" -j MASQUERADE
fi
sudo iptables -t mangle -C FORWARD -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu 2>/dev/null ||
  sudo iptables -t mangle -I FORWARD 1 -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu
sudo iptables -C FORWARD -o xnu0 -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT 2>/dev/null ||
  sudo iptables -I FORWARD 1 -o xnu0 -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
sudo iptables -C FORWARD -i xnu0 -j ACCEPT 2>/dev/null ||
  sudo iptables -I FORWARD 1 -i xnu0 -j ACCEPT
sudo iptables -C FORWARD -i xnu0 -m conntrack --ctstate INVALID -j DROP 2>/dev/null ||
  sudo iptables -I FORWARD 1 -i xnu0 -m conntrack --ctstate INVALID -j DROP
command -v dnsmasq >/dev/null || sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q dnsmasq-base >/dev/null 2>&1
if command -v dnsmasq >/dev/null; then
  sudo pkill -f 'dnsmasq.*xnu0' 2>/dev/null || true
  for _ in $(seq 1 20); do pgrep -f 'dnsmasq.*xnu0' >/dev/null || break; sleep 0.1; done
  sudo rm -f /run/dnsmasq-xnu0.pid
  sudo dnsmasq --interface=xnu0 --bind-interfaces --except-interface=lo --dhcp-range=192.168.77.2,192.168.77.50,12h \
    --dhcp-option=3,192.168.77.1 --dhcp-option=6,192.168.77.1 --dhcp-option=26,"$mtu" --pid-file=/run/dnsmasq-xnu0.pid
fi
echo "vm-net-up: xnu0 192.168.77.1/24 mtu $mtu up, NAT via ${up:-none}, dnsmasq: $(command -v dnsmasq >/dev/null && echo yes || echo no)"

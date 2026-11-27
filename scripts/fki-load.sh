#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# In the VM, from xnulinux-fki.service early in every boot (scripts/vm-install-fki-service.sh): load fki.ko with the carve-out
# in /etc/xnulinux-fki.conf. A fki.ko built for another kernel (the distribution's unattended upgrades install a new one, the
# next reboot runs it) is rebuilt first against the running kernel's headers, as the owner of the tree: before, insmod failed
# with "Invalid module format", and xnulinux-boot.sh then loaded fki with its built-in default carve-out, which overlaps
# Linux's RAM on a VM set up with another one ("carve-out ... overlaps IORESOURCE_SYSTEM_RAM; refusing to map").
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
CONF=${FKI_CONF:-/etc/xnulinux-fki.conf}
[ -r "$CONF" ] || { echo "no $CONF (scripts/vm-install-fki-service.sh)" >&2; exit 1; }
. "$CONF"
: "${CARVE:?no CARVE= in $CONF}"
KO=$P/fki-ko/fki.ko; K=/lib/modules/$(uname -r)/build
vermagic() { modinfo -F vermagic "$1" 2>/dev/null | cut -d' ' -f1; }
if [ "$(vermagic "$KO")" != "$(uname -r)" ]; then
  [ -d "$K" ] || { echo "fki.ko is for kernel '$(vermagic "$KO")', and there are no headers for $(uname -r) to rebuild it" >&2; exit 1; }
  echo "fki.ko is for kernel '$(vermagic "$KO")': rebuilding it for $(uname -r)"
  runuser -u "$(stat -c %U "$P/fki-ko")" -- make -s -C "$K" M="$P/fki-ko" modules
fi
exec /usr/sbin/insmod "$KO" $CARVE

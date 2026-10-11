#!/bin/bash
# One-time setup of a Raspberry Pi (Raspberry Pi OS Lite, 64-bit) to run the DustGate brain. Run ON the Pi, as root:
#   curl -O ... / or copy this directory over, then:  sudo bash setup.sh
# Safe to run again. It does not start the brain: `deploy.sh` (from your Mac) ships the code and does that.
set -euo pipefail
[ "$(id -u)" = 0 ] || exec sudo bash "$0" "$@"
HERE="$(cd "$(dirname "$0")" && pwd)"
ARDUINOJSON_VERSION=6.21.5

echo "== packages"
apt-get update -qq
apt-get install -y build-essential git rsync curl libboost-dev avahi-daemon avahi-utils

echo "== the brain's user and directories"
id dustgate >/dev/null 2>&1 || useradd --system --home-dir /var/lib/dustgate --create-home --shell /usr/sbin/nologin dustgate
install -d -o dustgate -g dustgate /var/lib/dustgate
install -d -o root -g root /opt/dustgate /opt/dustgate/bin /opt/dustgate/vendor
# Source lands here (rsync from the Mac, or a git checkout). Owned by whoever you SSH in as, so deploy.sh needs no sudo to copy.
install -d -o "${SUDO_USER:-root}" /opt/dustgate/src /opt/dustgate/www

echo "== ArduinoJson ${ARDUINOJSON_VERSION} (the brain's only third-party header besides Boost)"
if [ ! -f /opt/dustgate/vendor/ArduinoJson.h ]; then
  curl -fsSL "https://github.com/bblanchon/ArduinoJson/releases/download/v${ARDUINOJSON_VERSION}/ArduinoJson-v${ARDUINOJSON_VERSION}.h" -o /opt/dustgate/vendor/ArduinoJson.h
fi

# Only for `deploy.sh --on-pi`: the brain is normally cross-built on the Mac, and Trixie has no dphys-swapfile, so on a
# current Pi this block does nothing. (A Zero 2 W thrashed its SD-card swap until the watchdog rebooted it — 2026-10-06.)
echo "== swap, for an on-Pi build (skipped where dphys-swapfile does not exist)"
MEM_KB=$(awk '/MemTotal/ {print $2}' /proc/meminfo)
if [ "$MEM_KB" -lt 1500000 ] && [ -f /etc/dphys-swapfile ]; then
  sed -i 's/^#\?CONF_SWAPSIZE=.*/CONF_SWAPSIZE=2048/' /etc/dphys-swapfile
  sed -i 's/^#\?CONF_MAXSWAP=.*/CONF_MAXSWAP=2048/' /etc/dphys-swapfile
  dphys-swapfile setup >/dev/null && dphys-swapfile swapon || true
fi

echo "== WiFi power saving OFF (it adds latency and drops the nodes' links)"
if [ -d /etc/NetworkManager/conf.d ]; then
  printf '[connection]\nwifi.powersave = 2\n' > /etc/NetworkManager/conf.d/99-dustgate-wifi-powersave.conf
  systemctl reload NetworkManager 2>/dev/null || true
fi

echo "== a hung Pi reboots itself (hardware watchdog), and the journal survives a reboot"
install -d /etc/systemd/system.conf.d /etc/systemd/journald.conf.d
printf '[Manager]\nRuntimeWatchdogSec=15\nRebootWatchdogSec=2min\n' > /etc/systemd/system.conf.d/99-dustgate-watchdog.conf
printf '[Journal]\nStorage=persistent\nSystemMaxUse=200M\n' > /etc/systemd/journald.conf.d/99-dustgate.conf

echo "== mDNS keeps the name it was given (no silent rename to dustgate-2)"
# Seen 2026-10-11: after a plain reboot the Pi came back as dustgate-2.local and dustgate.local answered nothing, so the
# app looked frozen while the brain ran fine (the boards were unaffected: they redial the brain's last IP). Avahi renames
# itself when it believes another host holds its name, and on a Pi that "other host" is usually its OWN announcement
# heard back over IPv6 or a second interface. Announcing on the one interface that carries the default route, over IPv4
# only, removes the echo. Nothing in DustGate uses IPv6 mDNS.
IFACE="$(ip route show default 2>/dev/null | awk '{print $5; exit}')"
AV=/etc/avahi/avahi-daemon.conf
if [ -f "$AV" ]; then
  sed -i -E 's/^#?[[:space:]]*use-ipv6=.*/use-ipv6=no/' "$AV"
  if [ -n "$IFACE" ]; then
    if grep -qE '^#?[[:space:]]*allow-interfaces=' "$AV"; then
      sed -i -E "s/^#?[[:space:]]*allow-interfaces=.*/allow-interfaces=$IFACE/" "$AV"
    else
      sed -i -E "/^\[server\]/a allow-interfaces=$IFACE" "$AV"
    fi
  fi
fi

echo "== the service"
install -m 644 "$HERE/dustgate-brain.service" /etc/systemd/system/dustgate-brain.service
install -m 755 "$HERE/update.sh" /opt/dustgate/update.sh
systemctl daemon-reload
systemctl enable dustgate-brain.service
systemctl enable avahi-daemon.service
systemctl restart avahi-daemon.service   # picks up the settings above

echo
echo "Done. Hostname: $(hostname).local — the brain does not NEED mDNS, but it is how you reach this Pi by name."
echo "Next, from your Mac:  native/pi/deploy.sh ${SUDO_USER:-$(whoami)}@$(hostname).local"   # under sudo, whoami is root

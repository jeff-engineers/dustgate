#!/bin/bash
# Ship the brain and the app from this Mac to the Pi, build it there and restart it.
#   native/pi/deploy.sh pi@dustgate.local            # source + app, build, swap, restart (rolls back if it does not come up)
#   native/pi/deploy.sh pi@dustgate.local --no-ui    # skip the app build (the brain only)
#   native/pi/deploy.sh pi@dustgate.local --state /tmp/dgbench   # also copy a state directory (layout, pairings, API key) over
#   --on-pi  build on the Pi instead of cross-building here (slow; a Zero 2 W cannot do it without a swapfile)
# The brain is cross-built here with zig (brew install zig) when it is installed, and only the binary is installed on the Pi.
# Needs: ssh access with a key, `rsync` on both ends, and native/pi/setup.sh already run on the Pi.
set -euo pipefail
HOST="${1:?usage: deploy.sh user@host [--no-ui] [--state DIR]}"; shift || true
UI=1; STATE=""; ONPI=0
while [ $# -gt 0 ]; do case "$1" in --no-ui) UI=0;; --on-pi) ONPI=1;; --state) STATE="$2"; shift;; *) echo "unknown: $1" >&2; exit 2;; esac; shift; done
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

if [ "$UI" = 1 ]; then
  echo "== building the app"
  (cd dustgate-ui && npm run build >/dev/null)
fi
COMMIT="$(git rev-parse --short HEAD)$(git diff --quiet HEAD -- native firmware dustgate-ui 2>/dev/null || echo +)"
echo "== shipping $COMMIT"
echo "$COMMIT" > /tmp/dustgate-COMMIT
# The brain is native/ plus the firmware headers it shares. Nothing else is needed to build it.
rsync -az --delete --exclude 'native/build' --exclude 'native/vendor' --exclude '.pio*' --exclude 'firmware/test' \
  native firmware "$HOST:/opt/dustgate/src/"
rsync -az /tmp/dustgate-COMMIT "$HOST:/opt/dustgate/src/COMMIT"
[ "$UI" = 1 ] && rsync -az --delete dustgate-ui/dist/dustgate-ui/browser/ "$HOST:/opt/dustgate/www/"
if [ -n "$STATE" ]; then
  echo "== copying state from $STATE (stop the other brain first: two brains with one id fight over the nodes)"
  rsync -az --exclude 'brain.log' "$STATE/" "$HOST:/tmp/dustgate-state/"
  ssh "$HOST" 'sudo systemctl stop dustgate-brain; sudo cp -a /tmp/dustgate-state/. /var/lib/dustgate/ && sudo chown -R dustgate:dustgate /var/lib/dustgate && rm -rf /tmp/dustgate-state'
fi
# update.sh is installed once by setup.sh, so ship the current one first: a fix to it must not wait for a re-run of setup.
scp -q native/pi/update.sh "$HOST:/tmp/dustgate-update.sh"
if [ "$ONPI" = 0 ] && command -v zig >/dev/null; then
  echo "== cross-building for the Pi (zig)"
  make -C native arm64 COMMIT="$COMMIT" 2>&1 | grep -E "error|undefined" || true
  [ -f native/build/arm64/dustgate-brain ] || { echo "!! cross-build failed (rerun: make -C native arm64)" >&2; exit 1; }
  scp -q native/build/arm64/dustgate-brain "$HOST:/tmp/dustgate-brain"
  ssh -t "$HOST" 'sudo install -m 755 /tmp/dustgate-update.sh /opt/dustgate/update.sh && sudo /opt/dustgate/update.sh --binary /tmp/dustgate-brain'
else
  echo "== building on the Pi"
  ssh -t "$HOST" 'sudo install -m 755 /tmp/dustgate-update.sh /opt/dustgate/update.sh && sudo /opt/dustgate/update.sh'
fi

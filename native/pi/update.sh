#!/bin/bash
# Build the brain from /opt/dustgate/src and swap it in, keeping the previous binary and rolling back if the new one does not
# come up. Runs on the Pi, as root (deploy.sh calls it). Usage: sudo /opt/dustgate/update.sh [--git [ref]]
#   --binary PATH install a binary built elsewhere (deploy.sh cross-builds on the Mac) instead of compiling here
#   --git [ref]   first `git fetch` + checkout (a git checkout in /opt/dustgate/src), instead of using what was rsynced
set -euo pipefail
[ "$(id -u)" = 0 ] || exec sudo bash "$0" "$@"
SRC=/opt/dustgate/src; BIN=/opt/dustgate/bin
PREBUILT=""
if [ "${1:-}" = "--binary" ]; then PREBUILT="$2"; shift 2; fi
if [ "${1:-}" = "--git" ]; then
  cd "$SRC"; git fetch --quiet origin; git checkout --quiet "${2:-origin/main}"; git rev-parse --short HEAD > COMMIT
fi
cd "$SRC/native"
if [ -n "$PREBUILT" ]; then
  mkdir -p "$SRC/native/build"; cp "$PREBUILT" "$SRC/native/build/dustgate-brain"
  echo "== installing the prebuilt $(cat "$SRC/COMMIT" 2>/dev/null || echo unknown)"
else
  cd "$SRC/native"
  # The compiler is the memory hog: one job on a small board, all cores on a big one.
  MEM_KB=$(awk '/MemTotal/ {print $2}' /proc/meminfo)
  JOBS=1; [ "$MEM_KB" -gt 1500000 ] && JOBS=$(nproc)
  # Raspberry Pi OS Trixie has only a small compressed-RAM swap (no dphys-swapfile), and one Beast translation unit outgrows it on a
  # 512 MB board ("cc1plus: Killed"). So a small board gets a real 2 GB swapfile, once, used for builds only.
  if [ "$MEM_KB" -lt 1500000 ] && ! grep -q '^/swapfile' /proc/swaps; then
    echo "== adding a 2 GB swapfile so the compiler fits"
    [ -f /swapfile ] || { fallocate -l 2G /swapfile && chmod 600 /swapfile && mkswap /swapfile >/dev/null; }
    swapon /swapfile || true
  fi
  echo "== building $(cat ../COMMIT 2>/dev/null || echo unknown) with $JOBS job(s) — a Pi Zero takes several minutes"
  make -j"$JOBS" brain COMMIT="$(cat ../COMMIT 2>/dev/null || echo unknown)"

fi
NEW="$BIN/dustgate-brain.new"
install -m 755 build/dustgate-brain "$NEW"
[ -f "$BIN/dustgate-brain" ] && cp -p "$BIN/dustgate-brain" "$BIN/dustgate-brain.prev"
mv "$NEW" "$BIN/dustgate-brain"
echo "== restarting"
systemctl restart dustgate-brain
for i in $(seq 1 30); do
  if curl -fsS -m 2 localhost/api/info >/dev/null 2>&1; then echo "== up: $(curl -fsS localhost/api/info | sed 's/"apiKey":"[^"]*",//')"; exit 0; fi
  sleep 1
done
echo "!! the new brain did not answer on port 80 within 30 s" >&2
if [ -f "$BIN/dustgate-brain.prev" ]; then
  echo "!! rolling back to the previous binary" >&2
  mv "$BIN/dustgate-brain.prev" "$BIN/dustgate-brain"; systemctl restart dustgate-brain
fi
journalctl -u dustgate-brain -n 30 --no-pager >&2
exit 1

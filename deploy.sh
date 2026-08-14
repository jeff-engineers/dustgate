#!/usr/bin/env bash
# deploy.sh — build Angular UI + flash firmware + flash filesystem
# Run from the project root:
#   bash deploy.sh          # firmware + filesystem + auto-provision
#   bash deploy.sh --ui     # UI build + filesystem only (skip firmware flash)
#   bash deploy.sh --fw     # firmware only (skip UI build + filesystem)
#   bash deploy.sh --no-provision  # skip auto-provision step
#   bash deploy.sh --provision-only  # skip build/flash, just (re)send credentials
#   bash deploy.sh --no-topology-backup  # don't save/restore the shop layout
#   bash deploy.sh --node                # SECONDARY servo-only node (QT Py S3)
#   bash deploy.sh --node=xiao_c5        # ...a different node board (any pio env)
#
# Anything that flashes the filesystem WIPES the saved shop, so the deploy pulls
# topology.json off the device first (§0) and puts it back at the end (§5).
# DUSTGATE_HOST overrides where it looks (default: the configured hostname).
#
# Credentials are read from tools/.env (copy tools/.env.example to get started).
# Never commit tools/.env — it's gitignored.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Board facts derived from platformio.ini + config.h + boards/*.h, rather than
# duplicated here as a list this file would forget to update.
# shellcheck source=tools/boardinfo.sh
source "$SCRIPT_DIR/tools/boardinfo.sh"

# esptool's post-write "hard reset" occasionally fails on this board's native
# USB-CDC port ("Could not configure port: Device not configured") — the
# reset still happens at the hardware level, esptool just can't confirm it.
# Treat that specific failure as success rather than aborting the deploy.
#
# Separately, the automatic 1200bps-touch reset INTO the bootloader (before
# any writing happens) is also occasionally unreliable on this board and
# fails with "No serial data received" — that one's a real failure (nothing
# was written), so prompt for a manual BOOT+RESET and retry rather than
# aborting the whole deploy over a flaky USB handshake.
run_pio() {
  local attempt log
  for attempt in 1 2 3 4 5; do
    log="$(mktemp)"
    if pio "$@" 2>&1 | tee "$log"; then
      rm -f "$log"
      return 0
    fi
    if grep -q "Could not configure port" "$log" && grep -qE "Hash of data verified|Chip erase completed successfully" "$log"; then
      echo "  (Ignoring benign post-operation reset-handshake error — the write itself succeeded.)"
      rm -f "$log"
      return 0
    fi
    if grep -q "No serial data received" "$log"; then
      rm -f "$log"
      echo ""
      echo "  ⚠  Couldn't reset the board into its bootloader automatically."
      echo "  ▶ Hold BOOT, tap RESET once, release BOOT after ~1s, then press Enter to retry."
      read -rp "    Press Enter once done (or Ctrl+C to give up)… "
      continue
    fi
    rm -f "$log"
    return 1
  done
  echo "  Still failing to connect after $attempt attempts — giving up."
  return 1
}
UI_DIR="$SCRIPT_DIR/dustgate-ui"
DATA_DIR="$SCRIPT_DIR/firmware/data"
ENV_FILE="$SCRIPT_DIR/tools/.env"
BACKUP_DIR="$SCRIPT_DIR/.dustgate-backups"

# ── Shop layout backup/restore ─────────────────────────────────────────────
#
# WHY THIS EXISTS: topology.json lives in the same LittleFS partition as the
# Angular bundle, and `pio run --target uploadfs` writes a fresh image built
# from firmware/data/. So every filesystem flash silently erases the user's
# shop — layout, gate calibration, node links, plug pairings. It bit us during
# bring-up and read as a node-pairing failure, which is exactly the kind of
# wrong trail this costs a day on.
#
# The device is the only copy: the configurator holds a document in the
# browser, but a fresh `dev.sh flash` on a machine that has never opened the UI
# has nothing to re-PUT. So the deploy takes its own copy first.
#
# Ordering matters and is not obvious:
#   BACKUP before any flashing, while the OLD firmware is still up and on WiFi.
#   RESTORE after provisioning, because a freshly-flashed board may not have
#   rejoined the network until credentials land.
TOPO_BACKUP=""          # path to this run's backup, empty if we didn't take one
TOPO_BACKUP_HAD_DOC=false

# Where the device lives. mDNS, same name the provision step configures.
topo_host() { echo "${DUSTGATE_HOST:-${HOSTNAME_CFG}.local}"; }

# The API key is handed out unauthenticated by /api/info, the same bootstrap the
# Angular app uses (HttpApiServer.cpp: "only reachable on the local network").
# Fetched fresh on each side of the flash rather than cached: the key lives in
# NVS and normally survives, but an NVS erase regenerates it, and a stale key
# would fail the restore at the very end when it is most expensive to notice.
topo_api_key() {
  curl -fsS --max-time 5 "http://$(topo_host)/api/info" 2>/dev/null \
    | python3 -c 'import json,sys; print(json.load(sys.stdin).get("apiKey",""))' 2>/dev/null || true
}

backup_topology() {
  local host key http out
  host="$(topo_host)"
  echo "▶ Saving the shop layout off the device first…"
  echo "  (a filesystem flash erases it — $host)"

  key="$(topo_api_key)"
  if [[ -z "$key" ]]; then
    echo "  ⚠  Couldn't reach $host to read its API key."
    echo "     If this board has a shop saved on it, THIS DEPLOY WILL ERASE IT."
    echo "     Options: fix the connection and re-run, point DUSTGATE_HOST at its"
    echo "     IP, or pass --no-topology-backup to say you don't need it."
    # Interactive: let the operator decide. Non-interactive (CI, a script): stop,
    # because destroying the only copy of someone's layout is not a default.
    if [[ -t 0 ]]; then
      read -rp "    Continue anyway and lose any saved layout? [y/N] " reply
      [[ "$reply" =~ ^[Yy]$ ]] || { echo "  Aborted."; exit 1; }
    else
      echo "  Aborted (not a terminal, so nothing to ask). Pass --no-topology-backup to override."
      exit 1
    fi
    return 0
  fi

  mkdir -p "$BACKUP_DIR"
  out="$BACKUP_DIR/topology-$(date +%Y%m%d-%H%M%S).json"
  # Ask for the status code separately from the body: 404 is the ORDINARY case
  # (a board that has never been set up) and must not read as a failure.
  http="$(curl -sS --max-time 10 -o "$out" -w '%{http_code}' \
            -H "X-Api-Key: $key" "http://$host/api/topology" 2>/dev/null || echo 000)"
  case "$http" in
    200)
      # Guard against a truncated or error body being restored later as if it
      # were a shop: it has to parse, and it has to be an object.
      if python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if isinstance(d,dict) else 1)' "$out" 2>/dev/null; then
        TOPO_BACKUP="$out"; TOPO_BACKUP_HAD_DOC=true
        echo "  ✓ Saved $(wc -c < "$out" | tr -d ' ') bytes → ${out#$SCRIPT_DIR/}"
      else
        rm -f "$out"
        echo "  ⚠  The device returned something that isn't a topology document — not restoring it."
      fi
      ;;
    404) rm -f "$out"; echo "  ℹ  No shop saved on the device yet — nothing to preserve." ;;
    401) rm -f "$out"; echo "  ⚠  API key rejected. Not backing up; a saved layout would be lost." ;;
    *)   rm -f "$out"; echo "  ⚠  Couldn't read the topology (HTTP $http). A saved layout would be lost." ;;
  esac
}

restore_topology() {
  $TOPO_BACKUP_HAD_DOC || return 0
  local host key http resp
  host="$(topo_host)"
  echo "▶ Putting the shop layout back…"

  # The board has just rebooted and may still be joining WiFi; mDNS can take
  # longer than the boot itself. Poll /api/info rather than sleeping a guess.
  key=""
  for _ in $(seq 1 30); do
    key="$(topo_api_key)"
    [[ -n "$key" ]] && break
    sleep 2
  done
  if [[ -z "$key" ]]; then
    echo "  ⚠  $host never came back within ~60s, so the layout is still only in:"
    echo "       ${TOPO_BACKUP#$SCRIPT_DIR/}"
    echo "     Restore it once the device is up:"
    echo "       bash tools/restore-topology.sh"
    return 0
  fi

  resp="$(curl -sS --max-time 15 -X PUT -H "X-Api-Key: $key" \
            -H 'Content-Type: application/json' \
            --data-binary "@$TOPO_BACKUP" "http://$host/api/topology" 2>&1 || true)"
  http="$(curl -sS --max-time 10 -o /dev/null -w '%{http_code}' \
            -H "X-Api-Key: $key" "http://$host/api/topology" 2>/dev/null || echo 000)"
  if [[ "$resp" == *'"ok":true'* && "$http" == "200" ]]; then
    echo "  ✓ Shop layout restored (and the device can read it back)."
  else
    echo "  ⚠  Restore didn't confirm: $resp"
    echo "     Your layout is safe here: ${TOPO_BACKUP#$SCRIPT_DIR/}"
    echo "     Retry with: bash tools/restore-topology.sh"
  fi
}

# Python with pyserial, used by the provision step to drive the serial port
# while holding DTR/RTS deasserted (so the DevKitC's CP2102 auto-reset never
# reboots the board mid-command). Prefer PlatformIO's bundled interpreter —
# pyserial is always present there — and fall back to system python3.
PROV_PY="$HOME/.platformio/penv/bin/python"
if [[ ! -x "$PROV_PY" ]] || ! "$PROV_PY" -c "import serial" 2>/dev/null; then
  PROV_PY="python3"
fi

DO_UI=true
DO_FW=true
DO_FS=true
DO_PROVISION=true
FORCE_PROVISION=false
DO_TOPO_BACKUP=true
# Which PlatformIO env to build. Empty = platformio.ini's default_envs (the
# primary DevKitC). --node switches to the servo-only secondary.
PIO_ENV=""

for arg in "$@"; do
  case $arg in
    --ui) DO_FW=false ;;
    --fw) DO_UI=false; DO_FS=false ;;
    --no-provision) DO_PROVISION=false ;;
    --provision-only) DO_UI=false; DO_FW=false; DO_FS=false; FORCE_PROVISION=true ;;
    --no-topology-backup) DO_TOPO_BACKUP=false ;;
    # A SECONDARY node: servo-only firmware, and no Angular bundle or LittleFS
    # image at all — a node's entire interface is the /nodelink WebSocket, which
    # is exactly why it fits on a 4MB board. Credentials still go over serial.
    #
    # --node=<env> names a different node board (xiao_c5, dustgate_node_c3, …).
    # Bare --node keeps the default, which is the QT Py S3 that gets bench-tested.
    --node)   PIO_ENV="dustgate_node"; DO_UI=false; DO_FS=false ;;
    --node=*) PIO_ENV="${arg#--node=}"; DO_UI=false; DO_FS=false ;;
  esac
done

# Assembled once so every pio invocation below targets the same env. Empty for
# the primary (pio then uses default_envs from platformio.ini).
#
# Expand with the ${arr[@]+"${arr[@]}"} guard everywhere: macOS ships bash 3.2,
# where "${arr[@]}" on an EMPTY array trips `set -u` as an unbound variable.
PIO_ENV_ARGS=()
[[ -n "$PIO_ENV" ]] && PIO_ENV_ARGS=(-e "$PIO_ENV")

# The xiao_c5 env rides a different platform, and the two platforms fight over
# one shared Arduino-core directory. Settle it before anything builds — left to
# PlatformIO it does NOT resolve itself; it dies with an opaque SCons TypeError
# naming no package at all. See tools/boardinfo.sh.
ensure_core_for_env "$PIO_ENV"

# ── Load credentials from tools/.env if present ────────────────────────────
# Callers (e.g. dev.sh, after interactively prompting) may already have these
# exported — only fall back to the file for whichever ones aren't set.
WIFI_SSID="${WIFI_SSID:-}"
WIFI_PASS="${WIFI_PASS:-}"
HOSTNAME_CFG="${HOSTNAME_CFG:-}"

if [[ -f "$ENV_FILE" ]]; then
  while IFS='=' read -r key val; do
    [[ "$key" =~ ^#.*$ || -z "$key" ]] && continue
    val="${val%%#*}"   # strip inline comments
    val="${val%"${val##*[![:space:]]}"}"  # rtrim
    case "$key" in
      WIFI_SSID)     [[ -z "$WIFI_SSID" ]]     && WIFI_SSID="$val" ;;
      WIFI_PASS)     [[ -z "$WIFI_PASS" ]]     && WIFI_PASS="$val" ;;
      HOSTNAME)      [[ -z "$HOSTNAME_CFG" ]]  && HOSTNAME_CFG="$val" ;;
    esac
  done < "$ENV_FILE"
fi
HOSTNAME_CFG="${HOSTNAME_CFG:-dustgate}"

echo ""
echo "╔══════════════════════════════════╗"
echo "║        DustGate Deploy           ║"
echo "╚══════════════════════════════════╝"
echo ""

# ── 0. Save the shop layout ────────────────────────────────────────────────
# Before anything is built or flashed, so a board we can't reach stops the
# deploy while it is still cheap to stop. Only when the filesystem is going to
# be rewritten — that is the step that erases it.
if $DO_FS && $DO_TOPO_BACKUP; then
  backup_topology
  echo ""
fi

# ── 1. Build Angular UI ────────────────────────────────────────────────────
if $DO_UI; then
  echo "▶ Building Angular UI…"
  cd "$UI_DIR"
  ng build --configuration production
  echo "▶ Copying bundle → firmware/data/"
  BROWSER_DIR="dist/dustgate-ui/browser"
  if [ ! -d "$BROWSER_DIR" ]; then
    BROWSER_DIR="dist/dustgate-ui"
  fi
  rm -rf "$DATA_DIR"/*
  cp -r "$BROWSER_DIR"/* "$DATA_DIR/"
  echo "  Files in data/:"
  ls -lh "$DATA_DIR"
  cd "$SCRIPT_DIR"
  echo ""
fi

# ── 2. Flash firmware ──────────────────────────────────────────────────────
if $DO_FW; then
  echo "▶ Flashing firmware…"
  cd "$SCRIPT_DIR"
  # -j 1: see extra_script.py — this project's build has a Mkdir/compile race
  # under parallel jobs that env.SetOption couldn't reliably suppress; the
  # CLI flag does.
  run_pio run ${PIO_ENV_ARGS[@]+"${PIO_ENV_ARGS[@]}"} --target upload -j 1
  echo ""
fi

# ── 3. Flash filesystem (LittleFS) ─────────────────────────────────────────
if $DO_FS; then
  echo "▶ Flashing filesystem (LittleFS)…"
  cd "$SCRIPT_DIR"
  run_pio run ${PIO_ENV_ARGS[@]+"${PIO_ENV_ARGS[@]}"} --target uploadfs -j 1
  echo ""
fi

# ── 4. Auto-provision credentials ─────────────────────────────────────────
if $DO_PROVISION && ($DO_FW || $DO_FS || $FORCE_PROVISION); then
  # Does this target speak USB straight from the MCU, or through a bridge chip?
  # It decides how we drive the modem control lines below, and the two cases want
  # OPPOSITE settings — see the long note at the pyserial block.
  #
  # DERIVED, not listed. This used to be a hardcoded env pattern, which is a copy
  # of what the board header already declares — and the copy went stale the moment
  # the XIAO C5 env was added, which would have driven a native-USB board the
  # bridge way and produced a board that prints nothing at all.
  if board_has_native_usb "$PIO_ENV"; then NATIVE_USB=1; else NATIVE_USB=0; fi
  echo "  Target: $(describe_env "$PIO_ENV")"

  if [[ -z "$WIFI_SSID" ]]; then
    echo "ℹ  No credentials in tools/.env — skipping auto-provision."
    echo "   (Copy tools/.env.example → tools/.env to enable this step.)"
  else
    echo "▶ Auto-provisioning credentials…"

    # Build provision JSON
    PAYLOAD=$(python3 -c "
import json, sys
d = {}
ssid = sys.argv[1]; pw = sys.argv[2]; host = sys.argv[3]
if ssid: d['ssid'] = ssid; d['pass'] = pw
if host: d['host'] = host
print(json.dumps(d))
" "$WIFI_SSID" "$WIFI_PASS" "$HOSTNAME_CFG")

    # Wait for the board to finish resetting and boot the app, then re-detect
    # the port fresh — the flash/reset cycle can change which /dev/cu.* node the
    # board enumerates as, so a port captured before the flash may no longer be
    # valid by the time we get here. Match the DevKitC's USB-serial bridge
    # (usbserial / SLAB_USBtoUART / wchusbserial) and the Feather's native USB
    # (usbmodem) — keep this in sync with detect_port() in dev.sh.
    echo "  Waiting for device to boot…"
    PORT=""
    for _ in $(seq 1 15); do
      sleep 1
      # `|| true`: when the globs match nothing, `ls` exits nonzero and this
      # bare assignment would otherwise silently kill the script under `set -e`.
      PORT="$(ls /dev/cu.usbserial* /dev/cu.SLAB_USBtoUART* /dev/cu.wchusbserial* \
                 /dev/cu.usbmodem* 2>/dev/null | head -1 || true)"
      if [[ -n "$PORT" ]]; then
        break
      fi
    done
    if [[ -z "$PORT" ]]; then
      echo "  ⚠  No USB serial port found — skipping provision."
      echo "     Troubleshooting:"
      echo "       - Check the board is still plugged in: ls /dev/cu.*"
      echo "       - Send it manually once connected: bash dev.sh provision"
      echo "       - Or open the setup portal WiFi hotspot on the device and configure via the web form."
    else
      echo "  Serial port: $PORT"
      echo "  Waiting for device to confirm…"
      # Send the "provision" command over serial WITHOUT resetting the board.
      # Raw shell redirection (printf > "$PORT") drops DTR when the fd closes
      # (macOS HUPCL), and the DevKitC's CP2102 auto-reset turns that into a
      # reboot — which reset the board between the write and the read and lost
      # the command entirely (the reason .env auto-provision "did nothing" on
      # the DevKitC). pyserial drives the lines explicitly and resends until the
      # firmware acks, covering a board still booting after a fresh flash.
      RESPONSE="$(PROVISION_PAYLOAD="$PAYLOAD" "$PROV_PY" - "$PORT" "$NATIVE_USB" <<'PY'
import glob, os, sys, time, serial

port_hint  = sys.argv[1]
native_usb = sys.argv[2] == "1"
payload    = os.environ["PROVISION_PAYLOAD"]
cmd        = ("provision %s\r\n" % payload).encode()

# DTR/RTS mean OPPOSITE things on the two kinds of target, so this is not a
# setting that can be shared:
#
#   Bridge chip (DevKitC CP2102/CH340): DTR->GPIO0, RTS->EN is an auto-RESET
#   circuit. Asserting either reboots the board mid-command. Hold both LOW.
#
#   Native USB (QT Py S3/C3, Feather S2): there is no reset circuit — the lines
#   are just CDC line state. But TinyUSB reports the port "connected" only while
#   DTR is asserted, and USBCDC::write() DROPS EVERY BYTE when not connected
#   (cores/esp32/USBCDC.cpp: `!tud_cdc_n_connected(itf)` guards write/print).
#   Hold DTR low here and the board never replies at all — the ack is
#   unreachable, not merely slow. Assert BOTH (USBCDC marks itself connected on
#   dtr && rts).
lines_on = native_usb


def find_port():
    """Re-glob every attempt: a native-USB board re-enumerates after its post-flash
    reset, so the /dev node from before the reset can be stale (opening it yields
    OSError 6, 'Device not configured') or renamed outright."""
    if os.path.exists(port_hint):
        return port_hint
    for pat in ("/dev/cu.usbmodem*", "/dev/cu.usbserial*",
                "/dev/cu.SLAB_USBtoUART*", "/dev/cu.wchusbserial*"):
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    return None


buf = b""
deadline = time.time() + 20   # generous: covers a full re-enumeration cycle
last_err = None

while time.time() < deadline and b"OK provision" not in buf:
    port = find_port()
    if port is None:
        time.sleep(0.5)
        continue

    s = serial.Serial()
    s.port     = port
    s.baudrate = 115200
    s.dtr      = lines_on   # set BEFORE open so the CP2102 reset never fires
    s.rts      = lines_on
    s.timeout  = 0.2
    try:
        s.open()
    except Exception as e:
        last_err = e
        time.sleep(0.5)
        continue

    # Never let a mid-command unplug/re-enumerate escape as a traceback: the
    # outer loop retries, and a genuine timeout falls through to the "no
    # confirmation seen" guidance the caller already prints.
    try:
        time.sleep(0.3)
        s.reset_input_buffer()
        last_send = 0.0
        while time.time() < deadline:
            now = time.time()
            if now - last_send > 1.5:      # (re)send; board may still be booting
                s.write(cmd); s.flush()
                last_send = now
            chunk = s.read(256)
            if chunk:
                buf += chunk
                if b"OK provision" in buf:
                    break
    except Exception as e:
        last_err = e
        time.sleep(0.5)
    finally:
        try:
            s.close()
        except Exception:
            pass

if last_err is not None and b"OK provision" not in buf:
    sys.stderr.write("  (serial: %s)\n" % last_err)
sys.stdout.write(buf.decode("utf-8", "replace"))
PY
)"
      if echo "$RESPONSE" | grep -q "OK provision"; then
        echo "  ✓ Device confirmed: credentials saved."
        echo "  Web UI should be reachable shortly at: http://${HOSTNAME_CFG}.local"
      else
        echo "  ⚠  No confirmation seen from the device — it may not have been ready yet,"
        echo "     or the port changed again mid-command. Troubleshooting:"
        echo "       - Open a serial monitor and watch the boot log: bash dev.sh monitor"
        echo "       - Retry provisioning on its own once you see it fully booted: bash dev.sh provision"
        # NB: do NOT echo $PAYLOAD here — it contains the WiFi password in
        # cleartext. Point at the re-run instead, which rebuilds the payload
        # from tools/.env without printing any secret.
      fi
    fi
  fi
  echo ""
fi

# ── 5. Put the shop layout back ────────────────────────────────────────────
# After provisioning, not before: a freshly-flashed board may not rejoin WiFi
# until its credentials land, and this restore travels over the network.
if $DO_FS && $DO_TOPO_BACKUP; then
  restore_topology
  echo ""
fi

echo "✓ Deploy complete."

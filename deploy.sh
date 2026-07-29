#!/usr/bin/env bash
# deploy.sh — build Angular UI + flash firmware + flash filesystem
# Run from the project root:
#   bash deploy.sh          # firmware + filesystem + auto-provision
#   bash deploy.sh --ui     # UI build + filesystem only (skip firmware flash)
#   bash deploy.sh --fw     # firmware only (skip UI build + filesystem)
#   bash deploy.sh --no-provision  # skip auto-provision step
#   bash deploy.sh --provision-only  # skip build/flash, just (re)send credentials
#
# Credentials are read from tools/.env (copy tools/.env.example to get started).
# Never commit tools/.env — it's gitignored.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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
DATA_DIR="$SCRIPT_DIR/linear_actuator/data"
ENV_FILE="$SCRIPT_DIR/tools/.env"

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

for arg in "$@"; do
  case $arg in
    --ui) DO_FW=false ;;
    --fw) DO_UI=false; DO_FS=false ;;
    --no-provision) DO_PROVISION=false ;;
    --provision-only) DO_UI=false; DO_FW=false; DO_FS=false; FORCE_PROVISION=true ;;
  esac
done

# ── Load credentials from tools/.env if present ────────────────────────────
# Callers (e.g. dev.sh, after interactively prompting) may already have these
# exported — only fall back to the file for whichever ones aren't set.
WIFI_SSID="${WIFI_SSID:-}"
WIFI_PASS="${WIFI_PASS:-}"
ANTHROPIC_KEY="${ANTHROPIC_KEY:-}"
HOSTNAME_CFG="${HOSTNAME_CFG:-}"

if [[ -f "$ENV_FILE" ]]; then
  while IFS='=' read -r key val; do
    [[ "$key" =~ ^#.*$ || -z "$key" ]] && continue
    val="${val%%#*}"   # strip inline comments
    val="${val%"${val##*[![:space:]]}"}"  # rtrim
    case "$key" in
      WIFI_SSID)     [[ -z "$WIFI_SSID" ]]     && WIFI_SSID="$val" ;;
      WIFI_PASS)     [[ -z "$WIFI_PASS" ]]     && WIFI_PASS="$val" ;;
      ANTHROPIC_KEY) [[ -z "$ANTHROPIC_KEY" ]] && ANTHROPIC_KEY="$val" ;;
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

# ── 1. Build Angular UI ────────────────────────────────────────────────────
if $DO_UI; then
  echo "▶ Building Angular UI…"
  cd "$UI_DIR"
  ng build --configuration production
  echo "▶ Copying bundle → linear_actuator/data/"
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
  run_pio run --target upload -j 1
  echo ""
fi

# ── 3. Flash filesystem (LittleFS) ─────────────────────────────────────────
if $DO_FS; then
  echo "▶ Flashing filesystem (LittleFS)…"
  cd "$SCRIPT_DIR"
  run_pio run --target uploadfs -j 1
  echo ""
fi

# ── 4. Auto-provision credentials ─────────────────────────────────────────
if $DO_PROVISION && ($DO_FW || $DO_FS || $FORCE_PROVISION); then
  if [[ -z "$WIFI_SSID" && -z "$ANTHROPIC_KEY" ]]; then
    echo "ℹ  No credentials in tools/.env — skipping auto-provision."
    echo "   (Copy tools/.env.example → tools/.env to enable this step.)"
  else
    echo "▶ Auto-provisioning credentials…"

    # Build provision JSON
    PAYLOAD=$(python3 -c "
import json, sys
d = {}
ssid = sys.argv[1]; pw = sys.argv[2]; key = sys.argv[3]; host = sys.argv[4]
if ssid: d['ssid'] = ssid; d['pass'] = pw
if key:  d['key'] = key
if host: d['host'] = host
print(json.dumps(d))
" "$WIFI_SSID" "$WIFI_PASS" "$ANTHROPIC_KEY" "$HOSTNAME_CFG")

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
      # the DevKitC). pyserial holds DTR/RTS deasserted so opening the port
      # never resets the board, and resends until the firmware acks — covering
      # the case where the board is still booting after a fresh flash.
      RESPONSE="$(PROVISION_PAYLOAD="$PAYLOAD" "$PROV_PY" - "$PORT" <<'PY'
import os, sys, time, serial
port    = sys.argv[1]
payload = os.environ["PROVISION_PAYLOAD"]
cmd     = ("provision %s\r\n" % payload).encode()
s = serial.Serial()
s.port = port
s.baudrate = 115200
s.dtr = False   # deassert BEFORE open so the CP2102 auto-reset never fires
s.rts = False
s.timeout = 0.2
try:
    s.open()
except Exception as e:
    sys.stderr.write("  (serial open failed: %s)\n" % e)
    sys.exit(0)
time.sleep(0.3)
s.reset_input_buffer()
buf = b""
last_send = 0.0
deadline = time.time() + 8
while time.time() < deadline:
    now = time.time()
    if now - last_send > 1.5:          # (re)send; board may still be booting
        s.write(cmd); s.flush()
        last_send = now
    chunk = s.read(256)
    if chunk:
        buf += chunk
        if b"OK provision" in buf:
            break
s.close()
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
        # NB: do NOT echo $PAYLOAD here — it contains the WiFi password and the
        # Anthropic API key in cleartext. Point at the re-run instead, which
        # rebuilds the payload from tools/.env without printing any secret.
      fi
    fi
  fi
  echo ""
fi

echo "✓ Deploy complete."

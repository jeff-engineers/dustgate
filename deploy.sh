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
    # the port fresh — the flash/reset cycle can change which /dev/cu.usbmodem*
    # node the board enumerates as, so a port captured before the flash may no
    # longer be valid by the time we get here.
    echo "  Waiting for device to boot…"
    PORT=""
    for _ in $(seq 1 15); do
      sleep 1
      # `|| true`: when the glob matches nothing, `ls` exits nonzero and this
      # bare assignment would otherwise silently kill the script under `set -e`.
      PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)"
      if [[ -n "$PORT" ]]; then
        break
      fi
    done
    if [[ -z "$PORT" ]]; then
      echo "  ⚠  No USB serial port found — skipping provision."
      echo "     Troubleshooting:"
      echo "       - Check the board is still plugged in: ls /dev/cu.usbmodem*"
      echo "       - Send it manually once connected: bash dev.sh provision"
      echo "       - Or open the setup portal WiFi hotspot on the device and configure via the web form."
    else
      echo "  Serial port: $PORT"
      stty -f "$PORT" 115200 2>/dev/null || true
      printf "provision %s\r\n" "$PAYLOAD" > "$PORT"

      # Confirm it actually landed rather than assuming — read back whatever
      # the firmware echoes for a few seconds and look for its own ack line.
      # (macOS ships no `timeout` binary, so use bash's own `read -t` instead.)
      echo "  Waiting for device to confirm…"
      RESPONSE=""
      while IFS= read -r -t 4 _line; do
        RESPONSE+="$_line"$'\n'
      done < "$PORT"
      if echo "$RESPONSE" | grep -q "OK provision"; then
        echo "  ✓ Device confirmed: credentials saved."
        echo "  Web UI should be reachable shortly at: http://${HOSTNAME_CFG}.local"
      else
        echo "  ⚠  No confirmation seen from the device — it may not have been ready yet,"
        echo "     or the port changed again mid-command. Troubleshooting:"
        echo "       - Open a serial monitor and watch the boot log: bash dev.sh monitor"
        echo "       - Retry provisioning on its own once you see it fully booted: bash dev.sh provision"
        echo "       - Or send the command by hand: provision $PAYLOAD"
      fi
    fi
  fi
  echo ""
fi

echo "✓ Deploy complete."

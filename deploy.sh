#!/usr/bin/env bash
# deploy.sh — build Angular UI + flash firmware + flash filesystem
# Run from the project root:
#   bash deploy.sh          # firmware + filesystem + auto-provision
#   bash deploy.sh --ui     # UI build + filesystem only (skip firmware flash)
#   bash deploy.sh --fw     # firmware only (skip UI build + filesystem)
#   bash deploy.sh --no-provision  # skip auto-provision step
#
# Credentials are read from tools/.env (copy tools/.env.example to get started).
# Never commit tools/.env — it's gitignored.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UI_DIR="$SCRIPT_DIR/dustgate-ui"
DATA_DIR="$SCRIPT_DIR/linear_actuator/data"
ENV_FILE="$SCRIPT_DIR/tools/.env"

DO_UI=true
DO_FW=true
DO_FS=true
DO_PROVISION=true

for arg in "$@"; do
  case $arg in
    --ui) DO_FW=false ;;
    --fw) DO_UI=false; DO_FS=false ;;
    --no-provision) DO_PROVISION=false ;;
  esac
done

# ── Load credentials from tools/.env if present ────────────────────────────
WIFI_SSID=""
WIFI_PASS=""
ANTHROPIC_KEY=""

if [[ -f "$ENV_FILE" ]]; then
  while IFS='=' read -r key val; do
    [[ "$key" =~ ^#.*$ || -z "$key" ]] && continue
    val="${val%%#*}"   # strip inline comments
    val="${val%"${val##*[![:space:]]}"}"  # rtrim
    case "$key" in
      WIFI_SSID)     WIFI_SSID="$val" ;;
      WIFI_PASS)     WIFI_PASS="$val" ;;
      ANTHROPIC_KEY) ANTHROPIC_KEY="$val" ;;
    esac
  done < "$ENV_FILE"
fi

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
  rm -rf "$DATA_DIR"/*
  cp -r dist/dustgate-ui/* "$DATA_DIR/"
  echo "  Files in data/:"
  ls -lh "$DATA_DIR"
  cd "$SCRIPT_DIR"
  echo ""
fi

# ── 2. Flash firmware ──────────────────────────────────────────────────────
if $DO_FW; then
  echo "▶ Flashing firmware…"
  cd "$SCRIPT_DIR"
  pio run --target upload
  echo ""
fi

# ── 3. Flash filesystem (LittleFS) ─────────────────────────────────────────
if $DO_FS; then
  echo "▶ Flashing filesystem (LittleFS)…"
  cd "$SCRIPT_DIR"
  pio run --target uploadfs
  echo ""
fi

# ── 4. Auto-provision credentials ─────────────────────────────────────────
if $DO_PROVISION && ($DO_FW || $DO_FS); then
  if [[ -z "$WIFI_SSID" && -z "$ANTHROPIC_KEY" ]]; then
    echo "ℹ  No credentials in tools/.env — skipping auto-provision."
    echo "   (Copy tools/.env.example → tools/.env to enable this step.)"
  else
    echo "▶ Auto-provisioning credentials…"

    # Find the ESP32 serial port
    PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
    if [[ -z "$PORT" ]]; then
      echo "  ⚠  No USB serial port found — skipping provision. Connect device and re-run with --no-fw --no-ui, or use tools/provision.html."
    else
      echo "  Serial port: $PORT"

      # Build provision JSON
      PAYLOAD=$(python3 -c "
import json, sys
d = {}
ssid = sys.argv[1]; pw = sys.argv[2]; key = sys.argv[3]
if ssid: d['ssid'] = ssid; d['pass'] = pw
if key:  d['key'] = key
print(json.dumps(d))
" "$WIFI_SSID" "$WIFI_PASS" "$ANTHROPIC_KEY")

      # Wait for device to boot, then send provision command
      echo "  Waiting 5 s for device to boot…"
      sleep 5

      # Send command: stty sets baud, printf writes the line, sleep lets it flush
      stty -f "$PORT" 115200 2>/dev/null || true
      printf "provision %s\r\n" "$PAYLOAD" > "$PORT"
      sleep 0.5

      echo "  ✓ Provision command sent."
    fi
  fi
  echo ""
fi

echo "✓ Deploy complete."

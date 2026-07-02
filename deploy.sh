#!/usr/bin/env bash
# deploy.sh — build Angular UI + flash firmware + flash filesystem
# Run from the project root:
#   bash deploy.sh          # firmware + filesystem
#   bash deploy.sh --ui     # UI build + filesystem only (skip firmware flash)
#   bash deploy.sh --fw     # firmware only (skip UI build + filesystem)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UI_DIR="$SCRIPT_DIR/dustgate-ui"
DATA_DIR="$SCRIPT_DIR/linear_actuator/data"

DO_UI=true
DO_FW=true
DO_FS=true

for arg in "$@"; do
  case $arg in
    --ui) DO_FW=false ;;
    --fw) DO_UI=false; DO_FS=false ;;
  esac
done

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

echo "✓ Deploy complete."

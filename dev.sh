#!/usr/bin/env bash
# dev.sh — one entry point for every way to run DustGate.
#
# Interactive:
#   bash dev.sh
#
# Direct:
#   bash dev.sh demo               # browser-only, fully simulated (DemoApiService), no backend
#   bash dev.sh mock                # ng serve + tools/mock-api.js backend (real HTTP/WS contract)
#   bash dev.sh flash               # full real-hardware deploy (UI build + firmware + filesystem + provision)
#   bash dev.sh flash --fw          # firmware only
#   bash dev.sh flash --ui          # UI + filesystem only (skip firmware)
#   bash dev.sh flash --no-provision
#   bash dev.sh monitor             # serial monitor
#   bash dev.sh erase                # full chip erase (fixes corrupted-partition weirdness)
#   bash dev.sh provision            # (re)send WiFi/key/hostname without reflashing
#   bash dev.sh live [host]          # ng serve with hot reload, proxied to REAL hardware
#                                    #   (default host: dustgate.local)
#
# NOTE for future work: this is deliberately a thin bash wrapper around
# PlatformIO/esptool/serial commands, not a real tool. If this grows much more
# (device discovery, live status, multi-device support), it'd be worth a small
# GUI/TUI app instead of more bash — keep that in mind rather than piling on
# more flags here.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UI_DIR="$SCRIPT_DIR/dustgate-ui"
TOOLS_DIR="$SCRIPT_DIR/tools"
ENV_FILE="$SCRIPT_DIR/tools/.env"

PIO="pio"
if ! command -v pio >/dev/null 2>&1; then
  if [[ -x "$HOME/.platformio/penv/bin/pio" ]]; then
    PIO="$HOME/.platformio/penv/bin/pio"
  fi
fi

# Finds the ESP32's serial port, ignoring unrelated devices (e.g. macOS's
# built-in /dev/cu.Bluetooth-Incoming-Port, which PlatformIO's own auto-detect
# has been observed to grab instead of the board).
detect_port() {
  # The trailing `|| true` matters: when the glob matches nothing, `ls` exits
  # nonzero, and under `set -e -o pipefail` a bare assignment like
  # `port="$(detect_port)"` would otherwise silently kill the whole script
  # right here — with no error message, since stderr is discarded above.
  ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true
}

# Waits (with retries) for the ESP32 to show up on USB, prompting for a manual
# BOOT+RESET if it doesn't appear right away — native USB-CDC boards don't
# always respond to the automatic 1200bps-touch reset.
require_port() {
  local port
  port="$(detect_port)"
  if [[ -n "$port" ]]; then
    echo "$port"
    return 0
  fi

  echo "  No ESP32 detected on /dev/cu.usbmodem*." >&2
  echo "  Try: hold BOOT, tap RESET, release BOOT after ~1s — then this will retry." >&2
  for _ in $(seq 1 60); do
    sleep 1
    port="$(detect_port)"
    if [[ -n "$port" ]]; then
      echo "$port"
      return 0
    fi
  done

  echo "  Still no device found. Check the cable/port and try again." >&2
  return 1
}

# Reads tools/.env (if present) into ENV_* vars, without mutating the file.
# Used purely to prefill prompt defaults.
load_env_defaults() {
  ENV_SSID=""; ENV_PASS=""; ENV_KEY=""; ENV_HOST="dustgate"
  if [[ -f "$ENV_FILE" ]]; then
    while IFS='=' read -r k v; do
      [[ "$k" =~ ^#.*$ || -z "$k" ]] && continue
      v="${v%%#*}"; v="${v%"${v##*[![:space:]]}"}"
      case "$k" in
        WIFI_SSID)     ENV_SSID="$v" ;;
        WIFI_PASS)     ENV_PASS="$v" ;;
        ANTHROPIC_KEY) ENV_KEY="$v" ;;
        HOSTNAME)      ENV_HOST="$v" ;;
      esac
    done < "$ENV_FILE"
  fi
  ENV_HOST="${ENV_HOST:-dustgate}"
}

# Interactively prompts for WiFi SSID/password, optional Anthropic key, and
# mDNS hostname — prefilled from tools/.env where available, Enter keeps the
# default. Exports WIFI_SSID/WIFI_PASS/ANTHROPIC_KEY/HOSTNAME_CFG for
# deploy.sh to pick up directly (it prefers already-exported vars over
# re-reading the file).
prompt_credentials() {
  load_env_defaults
  echo ""
  echo "  Provisioning details — press Enter to keep the default shown."
  read -rp "  WiFi SSID${ENV_SSID:+ [$ENV_SSID]}: " WIFI_SSID
  WIFI_SSID="${WIFI_SSID:-$ENV_SSID}"
  read -rsp "  WiFi Password${ENV_PASS:+ [unchanged, hidden]}: " WIFI_PASS; echo
  WIFI_PASS="${WIFI_PASS:-$ENV_PASS}"
  read -rp "  Anthropic API key (optional, enables AI setup assistant)${ENV_KEY:+ [unchanged]}: " ANTHROPIC_KEY
  ANTHROPIC_KEY="${ANTHROPIC_KEY:-$ENV_KEY}"
  read -rp "  Hostname — device will be at http://<host>.local [$ENV_HOST]: " HOSTNAME_CFG
  HOSTNAME_CFG="${HOSTNAME_CFG:-$ENV_HOST}"
  export WIFI_SSID WIFI_PASS ANTHROPIC_KEY HOSTNAME_CFG
}

pids=()
cleanup() {
  if [[ ${#pids[@]} -gt 0 ]]; then
    echo ""
    echo "Stopping..."
    kill "${pids[@]}" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

run_demo() {
  echo "▶ Demo mode — fully simulated in the browser, no backend needed."
  echo "  Opens at http://localhost:4200 — DemoApiService kicks in automatically"
  echo "  in dev (or append ?demo=true explicitly)."
  echo ""
  cd "$UI_DIR"
  [[ -d node_modules ]] || npm install
  npm start
}

run_mock() {
  echo "▶ Mock backend mode — Angular dev server + tools/mock-api.js."
  echo "  Mimics the ESP32's real HTTP/WebSocket API contract."
  echo ""
  cd "$TOOLS_DIR"
  [[ -d node_modules ]] || npm install
  echo "  Starting mock-api.js on :3000..."
  node mock-api.js &
  pids+=($!)

  sleep 1

  cd "$UI_DIR"
  [[ -d node_modules ]] || npm install
  echo "  Starting ng serve (proxied to mock backend) on :4200..."
  npm run start:mock &
  pids+=($!)

  wait
}

run_flash() {
  echo "▶ Real hardware — flashing ESP32."
  local port
  port="$(require_port)" || exit 1
  echo "  Using port: $port"
  echo ""

  # First-time setup: if provisioning wasn't disabled and tools/.env has no
  # SSID at all yet, ask for it now instead of silently skipping the step.
  if [[ "$*" != *"--no-provision"* && "$*" != *"--provision-only"* ]]; then
    load_env_defaults
    if [[ -z "$ENV_SSID" ]]; then
      echo "  No WiFi credentials found in tools/.env yet — let's set them up."
      prompt_credentials
    fi
  fi

  cd "$SCRIPT_DIR"
  # deploy.sh's internal `pio run` calls pick this up automatically —
  # PlatformIO honors PLATFORMIO_UPLOAD_PORT as an override for upload_port.
  PLATFORMIO_UPLOAD_PORT="$port" bash deploy.sh "$@"

  echo ""
  echo "  If the device doesn't respond below (no boot log, WiFi not connecting,"
  echo "  serial commands ignored), press the physical RESET button once — the"
  echo "  post-flash reset handshake is occasionally unreliable on this board."
  echo ""
  echo "▶ Opening serial monitor so you can see what's happening (Ctrl+C to exit)…"
  run_monitor --scan-boot
}

run_provision() {
  echo "▶ (Re)send WiFi/key/hostname to an already-flashed board."
  local port
  port="$(require_port)" || exit 1
  echo "  Using port: $port"
  prompt_credentials
  echo ""
  cd "$SCRIPT_DIR"
  PLATFORMIO_UPLOAD_PORT="$port" bash deploy.sh --provision-only
}

run_live() {
  local host="${1:-dustgate.local}"
  echo "▶ Live mode — Angular dev server (hot reload) talking to REAL hardware at $host."
  echo "  This is the real device: the motor will actually move and outlets will"
  echo "  actually switch. Only the UI is served locally for fast iteration."
  echo ""

  local proxy_file
  proxy_file="$(mktemp -t dustgate-live-proxy).json"
  cat > "$proxy_file" <<EOF
{
  "/api": {
    "target": "http://${host}",
    "changeOrigin": true,
    "secure": false,
    "logLevel": "info"
  },
  "/ws": {
    "target": "ws://${host}",
    "ws": true,
    "changeOrigin": true
  }
}
EOF

  cd "$UI_DIR"
  [[ -d node_modules ]] || npm install
  echo "  Proxying /api and /ws → $host"
  # Use npx so this works even without the Angular CLI installed globally
  # (matches the "command not found: ng" issue seen earlier in this project).
  npx ng serve --configuration development --proxy-config "$proxy_file"
}

# run_monitor [--scan-boot]
# --scan-boot: briefly scan output for known problem signatures (failed
# LittleFS mount, failed WiFi connect) before handing off to the interactive
# monitor. Only used right after a flash, where there's fresh boot output
# worth checking — skipped for a plain "bash dev.sh monitor" against an
# already-running device, where it'd just be a pointless 5s delay.
run_monitor() {
  echo "▶ Serial monitor (Ctrl+C to exit)."
  local port
  port="$(require_port)" || exit 1
  echo "  Using port: $port"
  echo ""
  cd "$SCRIPT_DIR"

  if [[ "${1:-}" == "--scan-boot" ]]; then
    local boot_log line
    boot_log=""
    while IFS= read -r -t 5 line; do
      echo "$line"
      boot_log+="$line"$'\n'
    done < "$port"

    if echo "$boot_log" | grep -q "LittleFS mount failed"; then
      echo ""
      echo "  ⚠  LittleFS mount failed — the filesystem partition looks corrupted."
      echo "     Try a full chip erase and reflash: bash dev.sh erase && bash dev.sh flash"
    fi
    if echo "$boot_log" | grep -q "Connection failed"; then
      echo ""
      echo "  ⚠  WiFi connection failed — check the SSID/password are correct and the"
      echo "     network is in range, then retry: bash dev.sh provision"
    fi
  fi

  "$PIO" device monitor --port "$port"
}

run_erase() {
  echo "▶ Full chip erase — wipes firmware AND filesystem."
  echo "  Use this if you're seeing corrupted-partition symptoms (e.g."
  echo "  persistent 'LittleFS mount failed' after reflashing normally)."
  local port
  port="$(require_port)" || exit 1
  echo "  Using port: $port"
  echo ""
  cd "$SCRIPT_DIR"
  # esptool's post-erase hard reset occasionally fails to report back on this
  # board's native USB-CDC port ("Device not configured") even though the
  # erase itself completed — don't treat that as a failure. Separately, the
  # automatic bootloader-entry handshake ("No serial data received") is a
  # real failure (nothing happened yet) — prompt for a manual BOOT+RESET and
  # retry instead of giving up.
  local attempt log
  for attempt in 1 2 3 4 5; do
    log="$(mktemp)"
    if "$PIO" run --target erase --upload-port "$port" 2>&1 | tee "$log"; then
      rm -f "$log"
      return 0
    fi
    if grep -q "Could not configure port" "$log" && grep -q "Chip erase completed successfully" "$log"; then
      echo "  (Ignoring benign post-erase reset-handshake error — the erase itself succeeded.)"
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

show_menu() {
  echo ""
  echo "DustGate dev launcher"
  echo "====================="
  echo "  1) Demo       — browser only, fully simulated, no backend"
  echo "  2) Mock       — ng serve + tools/mock-api.js (real API contract)"
  echo "  3) Flash      — full deploy to real ESP32 (UI + firmware + filesystem)"
  echo "  4) Flash (firmware only)"
  echo "  5) Flash (UI/filesystem only)"
  echo "  6) Serial monitor"
  echo "  7) Full chip erase (fixes corrupted-partition weirdness)"
  echo "  8) (Re)send WiFi/key/hostname to an already-flashed board"
  echo "  9) Live — local UI + hot reload, talking to REAL hardware"
  echo "  q) Quit"
  echo ""
  read -rp "Choose: " choice
  case "$choice" in
    1) run_demo ;;
    2) run_mock ;;
    3) run_flash ;;
    4) run_flash --fw ;;
    5) run_flash --ui ;;
    6) run_monitor ;;
    7) run_erase ;;
    8) run_provision ;;
    9) read -rp "  Device host [dustgate.local]: " h; run_live "${h:-dustgate.local}" ;;
    q|Q) exit 0 ;;
    *) echo "Unknown choice."; show_menu ;;
  esac
}

case "${1:-}" in
  demo)      run_demo ;;
  mock)      run_mock ;;
  flash)     shift; run_flash "$@" ;;
  monitor)   run_monitor ;;
  erase)     run_erase ;;
  provision) run_provision ;;
  live)      shift; run_live "$@" ;;
  "")        show_menu ;;
  *)
    echo "Unknown mode: $1"
    echo "Usage: dev.sh [demo|mock|flash [--fw|--ui|--no-provision]|monitor|erase|provision|live [host]]"
    exit 1
    ;;
esac

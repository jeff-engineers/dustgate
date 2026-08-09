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
# Which PlatformIO env to build. Empty = platformio.ini's default_envs (the
# primary DevKitC). --node switches to the servo-only secondary.
PIO_ENV=""

for arg in "$@"; do
  case $arg in
    --ui) DO_FW=false ;;
    --fw) DO_UI=false; DO_FS=false ;;
    --no-provision) DO_PROVISION=false ;;
    --provision-only) DO_UI=false; DO_FW=false; DO_FS=false; FORCE_PROVISION=true ;;
    # A SECONDARY node: servo-only firmware, and no Angular bundle or LittleFS
    # image at all — a node's entire interface is the /nodelink WebSocket, which
    # is exactly why it fits on a 4MB board. Credentials still go over serial.
    --node) PIO_ENV="dustgate_node"; DO_UI=false; DO_FS=false ;;
  esac
done

# Assembled once so every pio invocation below targets the same env. Empty for
# the primary (pio then uses default_envs from platformio.ini).
#
# Expand with the ${arr[@]+"${arr[@]}"} guard everywhere: macOS ships bash 3.2,
# where "${arr[@]}" on an EMPTY array trips `set -u` as an unbound variable.
PIO_ENV_ARGS=()
[[ -n "$PIO_ENV" ]] && PIO_ENV_ARGS=(-e "$PIO_ENV")

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
  # A node has no agent chat and no web UI, so an Anthropic key is meaningless
  # there — don't push one, and don't let its presence stand in for having WiFi
  # credentials (which are the ONLY thing a node actually needs).
  if [[ "$PIO_ENV" == dustgate_node* ]]; then
    ANTHROPIC_KEY=""
  fi

  # Does this target speak USB straight from the MCU, or through a bridge chip?
  # It decides how we drive the modem control lines below, and the two cases want
  # OPPOSITE settings — see the long note at the pyserial block.
  NATIVE_USB=0
  case "$PIO_ENV" in
    dustgate_node*|adafruit_feather_esp32s2) NATIVE_USB=1 ;;
  esac

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
        # NB: do NOT echo $PAYLOAD here — it contains the WiFi password and the
        # Anthropic API key in cleartext. Point at the re-run instead, which
        # rebuilds the payload from tools/.env without printing any secret.
      fi
    fi
  fi
  echo ""
fi

echo "✓ Deploy complete."

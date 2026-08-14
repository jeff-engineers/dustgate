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
#   bash dev.sh flash --host shop --ssid Shop-WiFi        # override what tools/.env says
#   bash dev.sh flash --ask                               # prompt for all three, prefilled
#     A bare word is the hostname, as with flash-node: `dev.sh flash shop`.
#     Overrides apply to THAT flash only — add --save to write them to tools/.env.
#     --pass SECRET works but lands in your shell history; prefer --ssid alone
#     (it asks for the password hidden) or --ask.
#     Flashing the filesystem ERASES the saved shop (topology.json shares that
#     partition with the Angular bundle), so deploy.sh copies it off the device
#     first and puts it back at the end. Backups: .dustgate-backups/, restore by
#     hand with `bash tools/restore-topology.sh`. Skip with --no-topology-backup.
#   bash dev.sh flash-node [board] [host]  # flash a SECONDARY servo-only node (+ WiFi creds)
#                                    #   board: s3 (default) | c5
#                                    #   e.g. bash dev.sh flash-node c5 dustgate-node-c5
#   bash dev.sh monitor             # serial monitor (primary)
#   bash dev.sh monitor node        # serial monitor for a secondary node
#   bash dev.sh monitor c5          # ...for a XIAO C5 node
#   bash dev.sh ports               # list attached boards + which role each is pinned to
#   bash dev.sh ports --pin         # pin each board's USB SERIAL to a role (do this once)
#     DUSTGATE_PORT=/dev/cu.xxx     # force a port for this one command
#     DUSTGATE_PORT_PRIMARY=…       # force a port for a role, e.g. in ~/.zshrc
#     DUSTGATE_PORT_NODE=…
#   bash dev.sh erase                # full chip erase (fixes corrupted-partition weirdness)
#   bash dev.sh provision            # (re)send WiFi/key/hostname without reflashing
#   bash dev.sh provision --host shop --ssid Shop-WiFi   # ...without the prompts
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

# Board facts (native USB? which header?) derived from platformio.ini + config.h
# + boards/*.h rather than hardcoded here — see the note at the top of the file.
# shellcheck source=tools/boardinfo.sh
source "$SCRIPT_DIR/tools/boardinfo.sh"

# Node board shorthand → PlatformIO env. The shorthand exists because nobody at a
# bench wants to type "dustgate_node" in full, and the env names can't be
# shortened without breaking `pio run -e`.
node_env_for() {
  case "${1:-}" in
    ""|s3|qtpy|qtpy_s3|dustgate_node) echo "dustgate_node" ;;
    c5|xiao|xiao_c5)                  echo "xiao_c5" ;;
    *)                                echo "" ;;   # not a board word
  esac
}

UI_DIR="$SCRIPT_DIR/dustgate-ui"
TOOLS_DIR="$SCRIPT_DIR/tools"
ENV_FILE="$SCRIPT_DIR/tools/.env"

PIO="pio"
if ! command -v pio >/dev/null 2>&1; then
  if [[ -x "$HOME/.platformio/penv/bin/pio" ]]; then
    PIO="$HOME/.platformio/penv/bin/pio"
  fi
fi

# ── Board identification ─────────────────────────────────────────────────────
#
# /dev/cu.* PATHS ARE NOT STABLE. The same DevKitC has appeared as both
# cu.usbserial-110 and cu.usbserial-1110 in one afternoon — macOS derives the
# suffix from the USB topology, so moving hubs or ports renames the board. Any
# scheme built on "first matching glob" is therefore a coin flip the moment two
# boards are attached, and you learn which one you got by flashing it.
#
# USB SERIAL NUMBERS are stable, per-board, and reported by every chip we use.
# So: identify by VID (which family) and remember by serial (which board).
#
#   bridge → DevKitC primary, via a USB-serial chip:
#              10c4 = CP2102 (Silicon Labs), 1a86 = CH340, 0403 = FTDI
#   native → QT Py / Feather node, USB straight off the MCU:
#              303a = Espressif, 239a = Adafruit
#
# Once a role is pinned (dev.sh ports --pin), the serial goes in .dustgate-ports
# and every later command finds that exact board no matter what it's called this
# week. Overrides, highest priority first:
#
#   DUSTGATE_PORT=/dev/cu.xxx     one-shot, applies to whatever you're running
#   DUSTGATE_PORT_PRIMARY / _NODE per-role, e.g. in your shell profile
#   .dustgate-ports               pinned serials (gitignored)
#   VID family match              the fallback, and fine with one board per family

PORTS_FILE="$(dirname "${BASH_SOURCE[0]}")/.dustgate-ports"

# Emits one "port|vid|serial|description" line per attached board, bridges first.
# PlatformIO already knows how to enumerate with hwid, and shells out to nothing
# we'd otherwise have to write per-platform.
list_boards() {
  pio device list --json-output 2>/dev/null | python3 -c '
import json, re, sys
BRIDGE = {"10c4": "bridge", "1a86": "bridge", "0403": "bridge"}
NATIVE = {"303a": "native", "239a": "native"}
rows = []
try:
    devs = json.load(sys.stdin)
except Exception:
    sys.exit(0)
for d in devs:
    hwid = d.get("hwid") or ""
    m = re.search(r"VID:PID=([0-9A-Fa-f]{4}):([0-9A-Fa-f]{4})", hwid)
    if not m:
        continue                      # Bluetooth-Incoming-Port and friends
    vid = m.group(1).lower()
    fam = BRIDGE.get(vid) or NATIVE.get(vid)
    if not fam:
        continue                      # some other USB serial device, not ours
    ser = re.search(r"SER=(\S+)", hwid)
    rows.append((fam, d.get("port",""), vid, ser.group(1) if ser else "",
                 (d.get("description") or "").strip()))
rows.sort(key=lambda r: 0 if r[0] == "bridge" else 1)
for fam, port, vid, ser, desc in rows:
    print("%s|%s|%s|%s|%s" % (fam, port, vid, ser, desc))
' || true
}

# Serial pinned to a role in .dustgate-ports, if any.
pinned_serial() {
  local role="$1"
  [[ -f "$PORTS_FILE" ]] || return 0
  grep -E "^${role}=" "$PORTS_FILE" 2>/dev/null | tail -1 | cut -d= -f2- || true
}

# detect_port [bridge|native]
#
# The family hint is a PREFERENCE, not a filter: with one board attached the
# other family is still used, because that is nearly always what you meant.
detect_port() {
  local prefer="${1:-bridge}"
  local role; [[ "$prefer" == "native" ]] && role="node" || role="primary"

  if [[ -n "${DUSTGATE_PORT:-}" ]]; then echo "$DUSTGATE_PORT"; return; fi
  local envvar="DUSTGATE_PORT_$(echo "$role" | tr '[:lower:]' '[:upper:]')"
  if [[ -n "${!envvar:-}" ]]; then echo "${!envvar}"; return; fi

  local boards; boards="$(list_boards)"
  [[ -z "$boards" ]] && return 0

  # A pinned serial wins over family matching — it names one physical board,
  # which is the whole point of pinning it.
  local want; want="$(pinned_serial "$role")"
  if [[ -n "$want" ]]; then
    local hit
    hit="$(awk -F'|' -v s="$want" '$4 == s { print $2; exit }' <<<"$boards")"
    [[ -n "$hit" ]] && { echo "$hit"; return; }
  fi

  local same other
  same="$(awk -F'|' -v f="$prefer" '$1 == f { print $2; exit }' <<<"$boards")"
  other="$(awk -F'|' -v f="$prefer" '$1 != f { print $2; exit }' <<<"$boards")"
  echo "${same:-$other}"
}

# Warn when the choice was actually ambiguous, so a wrong guess is visible before
# it costs a flash rather than after.
report_port_choice() {
  local chosen="$1" prefer="${2:-bridge}"
  [[ -n "${DUSTGATE_PORT:-}" ]] && return
  local boards; boards="$(list_boards)"
  local n; n="$(grep -c . <<<"$boards" || true)"
  [[ "${n:-0}" -le 1 ]] && return

  local role; [[ "$prefer" == "native" ]] && role="node" || role="primary"
  echo "  ℹ  More than one board is attached:"
  # `local` is load-bearing. Bash scoping is DYNAMIC: an undeclared loop variable
  # named `port` here reassigns the caller's `port` — which is exactly what
  # require_port holds the chosen device in. Without this, require_port returned
  # the empty string left over after the last read, and every command that used it
  # ran with no --port at all.
  local fam port vid ser desc mark
  while IFS='|' read -r fam port vid ser desc; do
    [[ -z "$port" ]] && continue
    mark="  "; [[ "$port" == "$chosen" ]] && mark="→ "
    printf "     %s%-24s %-8s %s\n" "$mark" "$port" "$fam" "${desc:-$vid}"
  done <<<"$boards"
  if [[ -n "$(pinned_serial "$role")" ]]; then
    echo "     Chose the board pinned as '$role' in .dustgate-ports."
  else
    echo "     Chose by USB family. Pin it once and stop guessing:  bash dev.sh ports --pin"
  fi
}

# One-line identity for a port, so "Using port: …" names the BOARD and not just a
# path nobody can tell apart at a glance.
describe_port() {
  local port="$1"
  awk -F'|' -v p="$port" '$2 == p { printf "%s, %s", $5, $1; exit }' <<<"$(list_boards)"
}

# `dev.sh ports` — show what's attached; `--pin` records each board's SERIAL
# against a role so later commands are deterministic.
run_ports() {
  local boards; boards="$(list_boards)"
  if [[ -z "$boards" ]]; then
    echo "No ESP32 boards found. Use a DATA cable, and check 'pio device list'."
    return 1
  fi
  echo "Attached boards:"
  local fam port vid ser desc
  while IFS='|' read -r fam port vid ser desc; do
    [[ -z "$port" ]] && continue
    printf "  %-24s %-8s %-14s %s\n" "$port" "$fam" "${ser:0:12}" "${desc:-$vid}"
  done <<<"$boards"

  if [[ "${1:-}" != "--pin" ]]; then
    echo
    echo "Pinned roles ($PORTS_FILE):"
    if [[ -f "$PORTS_FILE" ]]; then sed 's/^/  /' "$PORTS_FILE"; else echo "  (none — run: bash dev.sh ports --pin)"; fi
    return 0
  fi

  # Pin the first board of each family. Two nodes on one bench is the case this
  # can't resolve by itself; DUSTGATE_PORT_NODE covers it.
  : > "$PORTS_FILE"
  local pser nser
  pser="$(awk -F'|' '$1 == "bridge" { print $4; exit }' <<<"$boards")"
  nser="$(awk -F'|' '$1 == "native" { print $4; exit }' <<<"$boards")"
  [[ -n "$pser" ]] && echo "primary=$pser" >> "$PORTS_FILE"
  [[ -n "$nser" ]] && echo "node=$nser"    >> "$PORTS_FILE"
  echo
  echo "Pinned by USB serial (survives replugging and renamed /dev paths):"
  sed 's/^/  /' "$PORTS_FILE"
  [[ -z "$nser" ]] && echo "  (no node attached — plug it in and re-run to pin it too)"
  [[ -z "$pser" ]] && echo "  (no primary attached — plug it in and re-run to pin it too)"
  return 0
}

# Waits (with retries) for the ESP32 to show up on USB, prompting for a manual
# BOOT+RESET if it doesn't appear right away — native USB-CDC boards don't
# always respond to the automatic 1200bps-touch reset.
require_port() {
  local prefer="${1:-bridge}"
  local port
  port="$(detect_port "$prefer")"
  if [[ -n "$port" ]]; then
    report_port_choice "$port" "$prefer" >&2
    echo "$port"
    return 0
  fi

  echo "  No ESP32 serial port detected (looked for usbserial / SLAB_USBtoUART /" >&2
  echo "  wchusbserial / usbmodem under /dev/cu.*)." >&2
  echo "  Checks: use a DATA USB cable (not charge-only); confirm the board shows up" >&2
  echo "  with 'ls /dev/cu.*'; a CP2102/CH340 DevKitC needs the matching macOS driver." >&2
  echo "  If it's a flashing-handshake issue: hold BOOT, tap RESET, release BOOT after" >&2
  echo "  ~1s — then this will retry." >&2
  for _ in $(seq 1 60); do
    sleep 1
    port="$(detect_port "$prefer")"
    if [[ -n "$port" ]]; then
      report_port_choice "$port" "$prefer" >&2
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
  ENV_SSID=""; ENV_PASS=""; ENV_HOST="dustgate"
  if [[ -f "$ENV_FILE" ]]; then
    while IFS='=' read -r k v; do
      [[ "$k" =~ ^#.*$ || -z "$k" ]] && continue
      v="${v%%#*}"; v="${v%"${v##*[![:space:]]}"}"
      case "$k" in
        WIFI_SSID)     ENV_SSID="$v" ;;
        WIFI_PASS)     ENV_PASS="$v" ;;
        HOSTNAME)      ENV_HOST="$v" ;;
      esac
    done < "$ENV_FILE"
  fi
  ENV_HOST="${ENV_HOST:-dustgate}"
}

# Writes the three provisioning values back to tools/.env, preserving anything
# else in the file (comments, API key, whatever else lands there later). Only
# ever called for --save: an override is one-shot by default, because silently
# rewriting the file from a one-off flash is how you end up provisioning the next
# board with a hostname you typed once and forgot.
save_env_defaults() {
  local tmp; tmp="$(mktemp)"
  [[ -f "$ENV_FILE" ]] && { grep -vE '^[[:space:]]*(WIFI_SSID|WIFI_PASS|HOSTNAME)=' "$ENV_FILE" || true; } > "$tmp"
  {
    printf 'WIFI_SSID=%s\n' "$WIFI_SSID"
    printf 'WIFI_PASS=%s\n' "$WIFI_PASS"
    printf 'HOSTNAME=%s\n'  "$HOSTNAME_CFG"
  } >> "$tmp"
  mkdir -p "$(dirname "$ENV_FILE")"
  mv "$tmp" "$ENV_FILE"
  chmod 600 "$ENV_FILE"
  echo "  ✓ Saved SSID and hostname to tools/.env (password written, not echoed)."
}

# Pulls provisioning overrides out of an argument list, so a primary can be
# flashed with a hostname/network other than the one in tools/.env — the same
# control flash-node has always had, which was missing here purely because the
# primary reads its values from a file instead of a prompt.
#
#   --host NAME | --host=NAME     mDNS name (device ends up at NAME.local)
#   --ssid NAME | --ssid=NAME     WiFi network
#   --pass SECRET                 WiFi password — see the history note below
#   --ask                         prompt for all three even though .env has them
#   --save                        write the result back to tools/.env
#   NAME                          a bare word is the hostname (as in flash-node)
#
# Everything it doesn't recognise is left in PROVISION_REST for deploy.sh, so
# --fw / --ui / --no-provision keep working and any future deploy.sh flag passes
# through without this function needing to know about it.
PROVISION_REST=()
OV_HOST=""; OV_SSID=""; OV_PASS=""; OV_ASK=0; OV_SAVE=0
parse_provision_overrides() {
  PROVISION_REST=()
  OV_HOST=""; OV_SSID=""; OV_PASS=""; OV_ASK=0; OV_SAVE=0
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --host)   OV_HOST="${2:-}"; shift 2 ;;
      --host=*) OV_HOST="${1#*=}"; shift ;;
      --ssid)   OV_SSID="${2:-}"; shift 2 ;;
      --ssid=*) OV_SSID="${1#*=}"; shift ;;
      --pass)   OV_PASS="${2:-}"; shift 2 ;;
      --pass=*) OV_PASS="${1#*=}"; shift ;;
      --ask)    OV_ASK=1; shift ;;
      --save)   OV_SAVE=1; shift ;;
      --*)      PROVISION_REST+=("$1"); shift ;;
      *)        OV_HOST="$1"; shift ;;    # bare word = hostname, as in flash-node
    esac
  done
}

# Settle on the three values and export them for deploy.sh (which prefers an
# exported var over re-reading tools/.env). Order of authority: an explicit flag
# beats tools/.env, and --ask puts a prompt in front of whatever won — prefilled,
# so Enter still takes the default.
#
# `interactive_when_empty` asks on a first-run device with no SSID stored, which
# is the one case where proceeding silently would flash a board that can never
# join a network.
apply_provision_overrides() {
  local interactive_when_empty="${1:-1}"
  load_env_defaults
  [[ -n "$OV_SSID" ]] && ENV_SSID="$OV_SSID"
  [[ -n "$OV_PASS" ]] && ENV_PASS="$OV_PASS"
  [[ -n "$OV_HOST" ]] && ENV_HOST="$OV_HOST"

  if (( OV_ASK )) || [[ -z "$ENV_SSID" && "$interactive_when_empty" == "1" ]]; then
    [[ -z "$ENV_SSID" ]] && echo "  No WiFi credentials found in tools/.env yet — let's set them up."
    prompt_credentials --prefilled
    return
  fi

  WIFI_SSID="$ENV_SSID"; WIFI_PASS="$ENV_PASS"; HOSTNAME_CFG="$ENV_HOST"
  # A new network with no password given is worth one hidden prompt rather than
  # a silent failure to associate — the old password is almost never right for it.
  if [[ -n "$OV_SSID" && -z "$OV_PASS" ]]; then
    local reply=""
    # `|| true`: read returns non-zero on EOF, and under `set -e` a Ctrl-D at this
    # prompt would abort the flash rather than fall through to the stored password.
    read -rsp "  WiFi Password for '$WIFI_SSID' [Enter keeps the stored one]: " reply || true
    echo
    [[ -n "$reply" ]] && WIFI_PASS="$reply"
  fi
  export WIFI_SSID WIFI_PASS HOSTNAME_CFG
  if [[ -n "$OV_HOST$OV_SSID$OV_PASS" ]]; then
    echo "  Provisioning as: $HOSTNAME_CFG  on '$WIFI_SSID'"
    (( OV_SAVE )) || echo "  (this flash only — add --save to keep it in tools/.env)"
  fi
  (( OV_SAVE )) && save_env_defaults
  return 0
}

# Interactively prompts for WiFi SSID/password and mDNS hostname — prefilled
# from tools/.env where available, Enter keeps the default. Exports
# WIFI_SSID/WIFI_PASS/HOSTNAME_CFG for
# deploy.sh to pick up directly (it prefers already-exported vars over
# re-reading the file).
#
# --prefilled means the ENV_* defaults have already been set (and possibly
# overridden by a flag) by the caller, so don't re-read the file over the top.
prompt_credentials() {
  [[ "${1:-}" == "--prefilled" ]] || load_env_defaults
  echo ""
  echo "  Provisioning details — press Enter to keep the default shown."
  read -rp "  WiFi SSID${ENV_SSID:+ [$ENV_SSID]}: " WIFI_SSID
  WIFI_SSID="${WIFI_SSID:-$ENV_SSID}"
  read -rsp "  WiFi Password${ENV_PASS:+ [unchanged, hidden]}: " WIFI_PASS; echo
  WIFI_PASS="${WIFI_PASS:-$ENV_PASS}"
  read -rp "  Hostname — device will be at http://<host>.local [$ENV_HOST]: " HOSTNAME_CFG
  HOSTNAME_CFG="${HOSTNAME_CFG:-$ENV_HOST}"
  export WIFI_SSID WIFI_PASS HOSTNAME_CFG
  (( ${OV_SAVE:-0} )) && save_env_defaults
  return 0
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
  local demo_url="http://localhost:4200/?demo=true"
  echo "▶ Demo mode — fully simulated in the browser, no backend needed."
  echo "  Opening ${demo_url}"
  echo "  (On localhost the app only enters demo mode with ?demo=true — plain"
  echo "   localhost:4200 talks to a real backend and will fail with no server up.)"
  echo ""
  cd "$UI_DIR"
  [[ -d node_modules ]] || npm install
  # ng serve blocks, so open the browser (with the required flag) once it's up.
  # Poll for readiness rather than a blind sleep, so we don't hit the browser
  # before the dev server is listening (a not-yet-ready open lands on an error
  # page or a stale tab). macOS `open`/`xdg-open` may just focus an existing
  # localhost:4200 tab instead of navigating to ?demo=true — so demo runs WITHOUT
  # the backend proxy (see --proxy-config below): even a bare, non-demo tab then
  # can't spam ECONNREFUSED against a backend demo never starts.
  (
    for _ in $(seq 1 60); do
      curl -sf -o /dev/null "http://localhost:4200/" && break
      sleep 0.5
    done
    open "$demo_url" 2>/dev/null || xdg-open "$demo_url" 2>/dev/null || true
  ) &
  # Demo is fully in-browser (DemoApiService) — it makes no /api or /ws calls, so
  # override the development proxy (which points at a backend demo never runs).
  npm start -- --proxy-config proxy.demo.json
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
  # --host/--ssid/--pass/--ask/--save come out here; everything else (--fw, --ui,
  # --no-provision, …) carries on to deploy.sh untouched.
  parse_provision_overrides "$@"
  set -- "${PROVISION_REST[@]+"${PROVISION_REST[@]}"}"

  echo "▶ Real hardware — flashing ESP32."
  local port
  port="$(require_port)" || exit 1
  echo "  Using port: $port"
  echo ""

  # Settle the provisioning values: a flag beats tools/.env, --ask prompts over
  # either, and an empty .env still prompts on its own so a first-run board can't
  # be flashed with no way onto a network.
  if [[ "$*" != *"--no-provision"* && "$*" != *"--provision-only"* ]]; then
    apply_provision_overrides 1
  elif [[ -n "$OV_HOST$OV_SSID$OV_PASS" ]]; then
    echo "  ⚠  Ignoring --host/--ssid/--pass: provisioning is disabled by --no-provision."
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

run_flash_node() {
  # First argument MAY be a board word (s3/c5). If it isn't, it's the hostname
  # — which is how this command has always been called, and stays working.
  local node_env="dustgate_node"
  local maybe_env
  maybe_env="$(node_env_for "${1:-}")"
  if [[ -n "$maybe_env" && -n "${1:-}" ]]; then
    node_env="$maybe_env"
    shift
  fi

  echo "▶ Secondary NODE — flashing the servo-only firmware."
  echo "  Target: $(describe_env "$node_env")"
  if [[ "$node_env" == "xiao_c5" ]]; then
    echo ""
    echo "  ⚠  The C5 rides the pioarduino platform, not the espressif32 one every"
    echo "     other target uses. Both publish a package called"
    echo "     framework-arduinoespressif32 into the same directory, so only one"
    echo "     core can be installed at a time. The scripts now swap it in and out"
    echo "     for you (first swap downloads, later ones are instant) — but never"
    echo "     build this env alongside another in one pio command."
    echo "     Pin map: firmware/wiring/xiao-c5.md — UNVERIFIED against hardware."
  fi
  echo ""
  echo "  A node is a dumb actuator bank: it drives up to four servo valves and"
  echo "  nothing else. No web UI, no stepper, no plug polling — the primary does"
  echo "  all the thinking and sends it already-resolved angles."
  echo ""

  # Prefer the port family this board actually uses, so a node and the primary
  # can be plugged in together without grabbing the wrong one. Derived from the
  # board header rather than assumed — every node so far is native-USB, but that
  # is a property of the board, not of the word "node".
  local port prefer=bridge
  board_has_native_usb "$node_env" && prefer=native
  port="$(require_port "$prefer")" || exit 1
  { what="$(describe_port "$port")"; echo "  Using port: $port${what:+  ($what)}"; }

  # WiFi creds first. The primary CANNOT provision a node over the network — the
  # node isn't on the network yet, which is the whole chicken-and-egg. So we push
  # credentials over this USB cable now, the same way the primary gets them.
  load_env_defaults
  echo ""
  echo "  WiFi credentials (a node needs these to reach the primary):"
  read -rp "  WiFi SSID${ENV_SSID:+ [$ENV_SSID]}: " WIFI_SSID
  WIFI_SSID="${WIFI_SSID:-$ENV_SSID}"
  read -rsp "  WiFi Password${ENV_PASS:+ [unchanged, hidden]}: " WIFI_PASS; echo
  WIFI_PASS="${WIFI_PASS:-$ENV_PASS}"

  # The hostname is LOAD-BEARING here, unlike on the primary. It's what the node
  # advertises over mDNS, what the Boards screen lists, and what gets written
  # into the topology as link.host. Two nodes sharing a hostname collide on the
  # network and the primary can only ever reach one of them — so this is a
  # required, distinct value, not a nicety.
  local suggested="${1:-}"
  if [[ -z "$suggested" ]]; then
    suggested="$(next_node_hostname)"
  fi
  echo ""
  read -rp "  Node hostname — must be unique per node [$suggested]: " HOSTNAME_CFG
  HOSTNAME_CFG="${HOSTNAME_CFG:-$suggested}"
  if [[ "$HOSTNAME_CFG" == "${ENV_HOST:-dustgate}" ]]; then
    echo ""
    echo "  ✗ '$HOSTNAME_CFG' is the PRIMARY's hostname — a node needs its own."
    echo "    Re-run and pick something like dustgate-node-1."
    exit 1
  fi

  export WIFI_SSID WIFI_PASS HOSTNAME_CFG

  echo ""
  echo "  Flashing as: $HOSTNAME_CFG  (will appear at $HOSTNAME_CFG.local)"
  echo ""
  cd "$SCRIPT_DIR"
  PLATFORMIO_UPLOAD_PORT="$port" bash deploy.sh "--node=$node_env" "${@:2}"

  echo ""
  echo "  ✓ Node flashed. Next:"
  echo "      1. Leave it powered on the same WiFi."
  echo "      2. Open the app → Boards → Scan for boards."
  echo "      3. '$HOSTNAME_CFG' should appear — tap Add."
  echo "      4. Then assign gates to it in Gates."
  echo ""
  echo "▶ Opening serial monitor (Ctrl+C to exit)…"
  run_monitor --scan-boot "$node_env"
}

# Suggest the next free dustgate-node-N by asking mDNS what's already out there.
# Falls back to -1 when dns-sd isn't available or nothing answers.
next_node_hostname() {
  local found n
  found="$(timeout 3 dns-sd -B _dustgate._tcp 2>/dev/null | grep -o 'dustgate-node-[0-9]*' || true)"
  for n in $(seq 1 20); do
    if ! grep -q "dustgate-node-$n\b" <<<"$found"; then
      echo "dustgate-node-$n"
      return
    fi
  done
  echo "dustgate-node-1"
}

run_provision() {
  # Same overrides as flash. Here they're arguably more useful: this is the
  # command for moving an already-flashed board onto a different network or
  # renaming it, which is exactly what a flag spares you re-typing.
  parse_provision_overrides "$@"

  echo "▶ (Re)send WiFi/key/hostname to an already-flashed board."
  local port
  port="$(require_port)" || exit 1
  echo "  Using port: $port"
  # No flags given → prompt, which is what this command has always done. With
  # flags, take them as said and don't ask.
  if [[ -z "$OV_HOST$OV_SSID$OV_PASS" ]]; then
    prompt_credentials
  else
    apply_provision_overrides 0
  fi
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

# run_monitor [--scan-boot] [env]
# --scan-boot: briefly scan output for known problem signatures (failed
# LittleFS mount, failed WiFi connect) before handing off to the interactive
# monitor. Only used right after a flash, where there's fresh boot output
# worth checking — skipped for a plain "bash dev.sh monitor" against an
# already-running device, where it'd just be a pointless 5s delay.
#
# env: which platformio.ini environment's monitor settings to apply. This is
# NOT optional dressing — see the -e note below. Defaults to the ini's
# default_envs (the DevKitC primary).
run_monitor() {
  echo "▶ Serial monitor (Ctrl+C to exit)."
  local scan_boot=false env=""
  local a
  for a in "$@"; do
    case "$a" in
      --scan-boot) scan_boot=true ;;
      *)           env="$a" ;;
    esac
  done

  # Same board-family split as run_flash_node, from the same source of truth:
  # pick the port that matches the env being monitored, so a node and the primary
  # can share the bench. Getting this wrong on a native-USB board is silent — the
  # monitor opens the other board's port and shows nothing.
  local prefer=bridge
  board_has_native_usb "$env" && prefer=native

  local port
  port="$(require_port "$prefer")" || exit 1
  local what; what="$(describe_port "$port")"
  echo "  Using port: $port${what:+  ($what)}"

  # A monitor left running from an earlier session holds the port open, and the
  # second one fails in ways that look like a board problem rather than a
  # bookkeeping one. Worse, an old monitor may be sitting on the OTHER board — we
  # found one running "--port <primary> -e dustgate_node", which is exactly the
  # kind of mismatch this whole port-pinning exercise exists to prevent.
  local stale
  stale="$(pgrep -fl "device monitor" 2>/dev/null | grep -v "^$$ " || true)"
  if [[ -n "$stale" ]]; then
    echo ""
    echo "  ⚠  A serial monitor is ALREADY running:"
    sed 's/^/       /' <<<"$stale"
    echo "     It holds the port; this one may fail or show nothing."
    echo "     Close it, or:  pkill -f 'device monitor'"
    echo ""
  fi
  echo ""
  cd "$SCRIPT_DIR"

  if $scan_boot; then
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

  # -e is REQUIRED, not a nicety. Without it pio applies default_envs' monitor
  # settings to whatever is plugged in — so monitoring a QT Py node picked up the
  # DevKitC's `monitor_dtr = 0`, and on a NATIVE-USB board that means TinyUSB
  # never reports the port connected and USBCDC::write() silently discards every
  # byte. Result: a working board that prints absolutely nothing. (The giveaway
  # was the exception decoder loading esp32dev_wroom32/firmware.elf while
  # monitoring an S3.)
  local env_args=()
  [[ -n "$env" ]] && env_args=(-e "$env")

  # --no-reconnect: pio's monitor otherwise retries forever when the port goes
  # away, and each retry reprints "--- forcing RTS inactive" etc. on a loop after
  # you unplug. Right for the DevKitC, whose CP2102 keeps the port alive across
  # an EN reset — so the port only disappears when the CABLE does.
  #
  # Native-USB boards (QT Py S3, Feather S2) drop the port on EVERY reset, so
  # reconnect is what lets the monitor survive a reboot. Keep it for those.
  local reconnect_arg="--no-reconnect"
  case "$env" in
    dustgate_node*|adafruit_feather_esp32s2) reconnect_arg="" ;;
  esac
  [[ "${DUSTGATE_MONITOR_RECONNECT:-0}" == "1" ]] && reconnect_arg=""

  "$PIO" device monitor --port "$port" \
      ${env_args[@]+"${env_args[@]}"} \
      ${reconnect_arg:+"$reconnect_arg"}
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
  echo "  n) Flash a SECONDARY node — servo-only board, + WiFi creds  (QT Py S3)"
  echo "     nc5 = the same, onto a XIAO ESP32C5 instead"
  echo "  6) Serial monitor            (6n = monitor a secondary NODE instead)"
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
    n|N)       run_flash_node ;;
    nc5|NC5)   run_flash_node c5 ;;
    6) run_monitor ;;
    6n|6N) run_monitor dustgate_node ;;
    6c5)   run_monitor xiao_c5 ;;
    7) run_erase ;;
    8) run_provision ;;
    9) read -rp "  Device host [dustgate.local]: " h; run_live "${h:-dustgate.local}" ;;
    q|Q) exit 0 ;;
    *) echo "Unknown choice."; show_menu ;;
  esac
}

case "${1:-}" in
  ports)     run_ports "${2:-}" ;;
  demo)      run_demo ;;
  mock)      run_mock ;;
  flash)     shift; run_flash "$@" ;;
  # "monitor node" targets a secondary: picks the node's native-USB port over the
  # primary's bridge port, and applies the node env's monitor settings.
  monitor)
    shift || true
    case "${1:-}" in
      node|n)     run_monitor dustgate_node ;;
      c5|s3)      run_monitor "$(node_env_for "$1")" ;;
      *)          run_monitor ;;
    esac
    ;;
  erase)     run_erase ;;
  provision) shift; run_provision "$@" ;;
  flash-node|node) shift; run_flash_node "$@" ;;
  live)      shift; run_live "$@" ;;
  "")        show_menu ;;
  *)
    echo "Unknown mode: $1"
    echo "Usage: dev.sh [demo|mock|flash [--fw|--ui|--no-provision] [--host N] [--ssid N] [--pass S] [--ask] [--save]|flash-node [s3|c5] [hostname]|monitor [node|c5]|ports [--pin]|erase|provision [--host N] [--ssid N]|live [host]]"
    exit 1
    ;;
esac

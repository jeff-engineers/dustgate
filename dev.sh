#!/usr/bin/env bash
# dev.sh — one entry point for every way to run DustGate.
#
# ONE BOARD, TWO ROLES. Every target is a XIAO ESP32C5; a board is a PRIMARY or a
# NODE depending only on which program you flash. So every command below is one
# or the other, and there are no board words to type.
#
# Interactive:
#   bash dev.sh
#
# Direct:
#   bash dev.sh demo                # browser-only, fully simulated, no backend
#   bash dev.sh mock                # ng serve + tools/mock-api.js (real HTTP/WS contract)
#
#   bash dev.sh flash               # PRIMARY: UI + firmware + filesystem + provision
#   bash dev.sh flash --fw          # firmware only
#   bash dev.sh flash --ui          # UI + filesystem only
#   bash dev.sh flash --no-provision
#   bash dev.sh flash shop          # a bare word is the hostname
#   bash dev.sh flash --host shop --ssid Shop-WiFi     # override what tools/.env says
#   bash dev.sh flash --ask         # prompt for all three, prefilled
#     A firmware flash always CONFIRMS the hostname, prefilled from tools/.env —
#     Enter keeps it. Overrides apply to THAT flash only; --save writes them to
#     tools/.env. --pass SECRET works but lands in your shell history; prefer
#     --ssid alone (it asks for the password hidden) or --ask.
#     Flashing the filesystem ERASES the saved shop (topology.json shares that
#     partition with the Angular bundle). The copy-off-and-restore step is
#     COMMENTED OUT as of 2026-08-22 — see "0. Save the shop layout" in
#     deploy.sh. Until it returns, save one by hand first if it matters:
#       curl -H "X-Api-Key: <key>" http://<host>/api/topology > my-shop.json
#       bash tools/restore-topology.sh my-shop.json
#
#   bash dev.sh flash-node [host]   # NODE: servo-only firmware + WiFi creds
#     A node is a dumb actuator bank — four servo valves, no UI, no plug polling.
#     Its hostname is load-bearing (mDNS, the Boards screen, link.host in the
#     topology) and must be unique per node.
#
#   THE SLIDER BOARD — add --slider to either flash command:
#
#   bash dev.sh flash --slider        # a PRIMARY that drives the rack
#   bash dev.sh flash-node --slider   # a NODE that drives the rack
#     Same XIAO C5, same two roles. What changes is that the four PWM channels
#     become one ST3215 serial bus servo on D6/D7 plus two endstops on D8/D9 —
#     PWM and serial never share a board, so this is a different program, not a
#     runtime option. config.h #errors if a pin map ever claims both.
#
#     A SLIDER HOMES BEFORE IT CAN MOVE, and the carriage sweeps to find its
#     datum on the first boot after flashing. That is not optional: the servo
#     counts steps and has no idea where it is, least of all after a power cycle.
#     A slider NODE does that sweep itself — the one thing in this design a node
#     decides for itself — and holds any move it is sent until the datum lands.
#
#   bash dev.sh monitor             # serial monitor (primary)
#   bash dev.sh monitor node        # ...a node instead
#   bash dev.sh ports               # list attached boards + which role each is pinned to
#   bash dev.sh ports --pin primary # pin the attached board to a role (do this once)
#   bash dev.sh ports --pin node
#     DUSTGATE_PORT=/dev/cu.xxx     # force a port for this one command
#     DUSTGATE_PORT_PRIMARY=…       # force a port for a role, e.g. in ~/.zshrc
#     DUSTGATE_PORT_NODE=…
#   bash dev.sh erase               # full chip erase (fixes corrupted-partition weirdness)
#   bash dev.sh provision           # (re)send WiFi/key/hostname without reflashing
#   bash dev.sh live [host]         # ng serve with hot reload, proxied to REAL hardware
#                                   #   (default host: dustgate.local)
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

# The two envs, which are the two ROLES. Same board, same carrier, same pin map;
# the difference is build_src_filter and -DDUSTGATE_SECONDARY.
PRIMARY_ENV="xiao_c5_primary"
NODE_ENV="xiao_c5"

# The SLIDER pair. Same board and the same two roles — what changes is that the
# four PWM channels are traded for one ST3215 on a serial bus, because PWM and
# serial never share a board. A slider board is therefore a THIRD thing to flash
# in each role, not a flag on the servo build, and `--slider` picks it.
LINEAR_PRIMARY_ENV="xiao_c5_linear_primary"
LINEAR_NODE_ENV="xiao_c5_linear"

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
# /dev/cu.* PATHS ARE NOT STABLE. macOS derives the suffix from the USB topology,
# so moving hubs or ports renames the board. Any scheme built on "first matching
# glob" is a coin flip the moment two boards are attached, and you learn which
# one you got by flashing it.
#
# USB SERIAL NUMBERS are stable and per-board. So: pin a role to a serial once,
# and every later command finds that exact board no matter what it is called
# this week.
#
# PINNING IS THE ONLY MECHANISM, not a nicety: both boards are C5s with the same
# VID and the same description, distinguishable only by serial. With two plugged
# in and nothing pinned, the first enumerated is a guess and is announced as one.
#
# Overrides, highest priority first:
#
#   DUSTGATE_PORT=/dev/cu.xxx     one-shot, applies to whatever you're running
#   DUSTGATE_PORT_PRIMARY / _NODE per-role, e.g. in your shell profile
#   .dustgate-ports               pinned serials (gitignored)
#   first enumerated              the fallback, and fine with one board attached

PORTS_FILE="$(dirname "${BASH_SOURCE[0]}")/.dustgate-ports"

# Emits one "port|vid|serial|description" line per attached board. PlatformIO
# already knows how to enumerate with hwid, and shells out to nothing we'd
# otherwise have to write per-platform.
list_boards() {
  pio device list --json-output 2>/dev/null | python3 -c '
import json, re, sys
OURS = {"303a"}                       # Espressif native USB — every board we have
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
    if vid not in OURS:
        continue                      # some other USB serial device, not ours
    ser = re.search(r"SER=(\S+)", hwid)
    rows.append((d.get("port",""), vid, ser.group(1) if ser else "",
                 (d.get("description") or "").strip()))
for port, vid, ser, desc in rows:
    print("%s|%s|%s|%s" % (port, vid, ser, desc))
' || true
}

# Serial pinned to a role in .dustgate-ports, if any.
pinned_serial() {
  local role="$1"
  [[ -f "$PORTS_FILE" ]] || return 0
  grep -E "^${role}=" "$PORTS_FILE" 2>/dev/null | tail -1 | cut -d= -f2- || true
}

# detect_port [primary|node]
detect_port() {
  local role="${1:-primary}"

  if [[ -n "${DUSTGATE_PORT:-}" ]]; then echo "$DUSTGATE_PORT"; return; fi
  local envvar="DUSTGATE_PORT_$(echo "$role" | tr '[:lower:]' '[:upper:]')"
  if [[ -n "${!envvar:-}" ]]; then echo "${!envvar}"; return; fi

  local boards; boards="$(list_boards)"
  [[ -z "$boards" ]] && return 0

  # A pinned serial names one physical board, which is the whole point.
  local want; want="$(pinned_serial "$role")"
  if [[ -n "$want" ]]; then
    local hit
    hit="$(awk -F'|' -v s="$want" '$3 == s { print $1; exit }' <<<"$boards")"
    [[ -n "$hit" ]] && { echo "$hit"; return; }
  fi

  # Nothing pinned: take the board the OTHER role isn't pinned to, then fall back
  # to the first enumerated. Two unpinned boards is a guess — report_port_choice
  # says so.
  local other_role other_want candidate
  [[ "$role" == "primary" ]] && other_role="node" || other_role="primary"
  other_want="$(pinned_serial "$other_role")"
  if [[ -n "$other_want" ]]; then
    candidate="$(awk -F'|' -v s="$other_want" '$3 != s { print $1; exit }' <<<"$boards")"
    [[ -n "$candidate" ]] && { echo "$candidate"; return; }
  fi
  awk -F'|' '{ print $1; exit }' <<<"$boards"
}

# Warn when the choice was actually ambiguous, so a wrong guess is visible before
# it costs a flash rather than after.
report_port_choice() {
  local chosen="$1" role="${2:-primary}"
  [[ -n "${DUSTGATE_PORT:-}" ]] && return
  local boards; boards="$(list_boards)"
  local n; n="$(grep -c . <<<"$boards" || true)"
  [[ "${n:-0}" -le 1 ]] && return

  echo "  ℹ  More than one board is attached, and they are identical:"
  # `local` is load-bearing. Bash scoping is DYNAMIC: an undeclared loop variable
  # named `port` here reassigns the caller's `port` — which is exactly what
  # require_port holds the chosen device in. Without this, require_port returned
  # the empty string left over after the last read, and every command that used
  # it ran with no --port at all.
  local port vid ser desc mark
  while IFS='|' read -r port vid ser desc; do
    [[ -z "$port" ]] && continue
    mark="  "; [[ "$port" == "$chosen" ]] && mark="→ "
    printf "     %s%-24s %-14s %s\n" "$mark" "$port" "${ser:0:12}" "${desc:-$vid}"
  done <<<"$boards"
  if [[ -n "$(pinned_serial "$role")" ]]; then
    echo "     Chose the board pinned as '$role' in .dustgate-ports."
  else
    echo "     Nothing pinned as '$role' — this is a GUESS. Pin it once:"
    echo "       bash dev.sh ports --pin $role"
  fi
}

# One-line identity for a port, so "Using port: …" names the BOARD and not just a
# path nobody can tell apart at a glance.
describe_port() {
  local port="$1"
  awk -F'|' -v p="$port" '$1 == p { printf "%s, serial %s", $4, substr($3,1,12); exit }' <<<"$(list_boards)"
}

# `dev.sh ports` — show what's attached; `--pin ROLE` records a board's SERIAL
# against that role so later commands are deterministic.
#
# One role at a time, because the boards are indistinguishable: the honest
# workflow is to plug in the one you mean and say which it is. With more than one
# attached it asks rather than guessing.
run_ports() {
  local boards; boards="$(list_boards)"
  if [[ -z "$boards" ]]; then
    echo "No ESP32 boards found. Use a DATA cable, and check 'pio device list'."
    return 1
  fi
  echo "Attached boards:"
  local port vid ser desc
  while IFS='|' read -r port vid ser desc; do
    [[ -z "$port" ]] && continue
    printf "  %-24s %-14s %s\n" "$port" "${ser:0:12}" "${desc:-$vid}"
  done <<<"$boards"

  if [[ "${1:-}" != "--pin" ]]; then
    echo
    echo "Pinned roles ($PORTS_FILE):"
    if [[ -f "$PORTS_FILE" ]]; then sed 's/^/  /' "$PORTS_FILE"; else echo "  (none — run: bash dev.sh ports --pin primary)"; fi
    return 0
  fi

  local role="${2:-}"
  case "$role" in
    primary|node) ;;
    *) echo; echo "Which role? Usage: bash dev.sh ports --pin [primary|node]"; return 1 ;;
  esac

  local n; n="$(grep -c . <<<"$boards")"
  local chosen_ser chosen_port
  if [[ "$n" -eq 1 ]]; then
    chosen_port="$(awk -F'|' '{ print $1; exit }' <<<"$boards")"
    chosen_ser="$(awk -F'|'  '{ print $3; exit }' <<<"$boards")"
  else
    echo
    echo "More than one board attached — which one is the $role?"
    local i=1
    while IFS='|' read -r port vid ser desc; do
      [[ -z "$port" ]] && continue
      printf "  %d) %-24s %s\n" "$i" "$port" "${ser:0:12}"
      i=$((i+1))
    done <<<"$boards"
    local pick; read -rp "  Number: " pick
    chosen_port="$(awk -F'|' -v k="$pick" 'NF { c++ } c == k { print $1; exit }' <<<"$boards")"
    chosen_ser="$(awk -F'|'  -v k="$pick" 'NF { c++ } c == k { print $3; exit }' <<<"$boards")"
    [[ -z "$chosen_ser" ]] && { echo "  Not a listed board."; return 1; }
  fi

  local tmp; tmp="$(mktemp)"
  [[ -f "$PORTS_FILE" ]] && { grep -vE "^${role}=" "$PORTS_FILE" || true; } > "$tmp"
  echo "${role}=${chosen_ser}" >> "$tmp"
  mv "$tmp" "$PORTS_FILE"
  echo
  echo "Pinned $role to $chosen_port (serial ${chosen_ser:0:12}) — survives replugging"
  echo "and renamed /dev paths."
  sed 's/^/  /' "$PORTS_FILE"
  return 0
}

# Waits (with retries) for the ESP32 to show up on USB, prompting for a manual
# BOOT+RESET if it doesn't appear right away — native USB-CDC boards don't
# always respond to the automatic 1200bps-touch reset.
require_port() {
  local role="${1:-primary}"
  local port
  port="$(detect_port "$role")"
  if [[ -n "$port" ]]; then
    report_port_choice "$port" "$role" >&2
    echo "$port"
    return 0
  fi

  echo "  No ESP32 serial port detected (looked for a usbmodem under /dev/cu.*)." >&2
  echo "  Checks: use a DATA USB cable (not charge-only); confirm the board shows up" >&2
  echo "  with 'ls /dev/cu.*'. The C5's port comes straight off the MCU, so it also" >&2
  echo "  disappears whenever the board is in the bootloader or unpowered." >&2
  echo "  If it's a flashing-handshake issue: hold BOOT, tap RESET, release BOOT after" >&2
  echo "  ~1s — then this will retry." >&2
  for _ in $(seq 1 60); do
    sleep 1
    port="$(detect_port "$role")"
    if [[ -n "$port" ]]; then
      report_port_choice "$port" "$role" >&2
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
# Which env a primary flash targets. One board, one answer — kept as a variable
# only because the serial monitor afterwards needs the env name.
FLASH_ENV="$PRIMARY_ENV"
# Set by prompt_credentials so a caller can tell whether the full interactive
# path already ran — run_flash asks for the hostname on its own otherwise, and
# asking twice in one flash is worse than not asking at all.
PROVISION_PROMPTED=0
parse_provision_overrides() {
  PROVISION_REST=()
  OV_HOST=""; OV_SSID=""; OV_PASS=""; OV_ASK=0; OV_SAVE=0
  FLASH_ENV="$PRIMARY_ENV"
  PROVISION_PROMPTED=0
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
      # The rack board: one ST3215 on a serial bus and two endstops, instead of
      # four PWM channels. Consumed here rather than passed through, because the
      # env it selects is handed to deploy.sh as --env= below.
      --slider|--linear|--rack) FLASH_ENV="$LINEAR_PRIMARY_ENV"; shift ;;
      # Two primary envs now, and --slider picks between them, so these say
      # nothing. Accepted and ignored rather than failing a flash on muscle memory.
      --env)    shift 2 ;;
      --env=*|--c5) shift ;;
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
  PROVISION_PROMPTED=1
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

# Ask the primary's hostname on every firmware flash (menu 3 and 4).
#
# It used to be asked exactly once — on a first run, when tools/.env had no SSID
# yet — and silently reused forever after. That is the wrong default for the one
# value the whole shop types into a phone: reflashing a second controller, or
# renaming one, went through with the old name and the two boards then fought
# over the same mDNS record. Cheap to confirm, expensive to get wrong.
#
# Skipped when the answer is already settled or can't apply:
#   - prompt_credentials just asked (PROVISION_PROMPTED) — don't ask twice
#   - an explicit --host/bare word was given — the flag IS the answer
#   - --ui, which pushes the filesystem only and rewrites no NVS
confirm_primary_hostname() {
  (( PROVISION_PROMPTED )) && return 0
  [[ -n "$OV_HOST" ]] && return 0
  [[ "$*" == *"--ui"* ]] && return 0

  local suggested="${HOSTNAME_CFG:-${ENV_HOST:-dustgate}}"
  echo ""
  read -rp "  Hostname — device will be at http://<host>.local [$suggested]: " HOSTNAME_CFG
  HOSTNAME_CFG="${HOSTNAME_CFG:-$suggested}"
  export HOSTNAME_CFG
  (( ${OV_SAVE:-0} )) && save_env_defaults
  return 0
}

run_flash() {
  # --host/--ssid/--pass/--ask/--save come out here; everything else (--fw, --ui,
  # --no-provision, …) carries on to deploy.sh untouched.
  parse_provision_overrides "$@"
  set -- "${PROVISION_REST[@]+"${PROVISION_REST[@]}"}"

  if [[ "$FLASH_ENV" == "$LINEAR_PRIMARY_ENV" ]]; then
    echo "▶ Real hardware — flashing a SLIDER PRIMARY (XIAO C5 + ST3215)."
    echo "  Target: $(describe_env "$FLASH_ENV")"
    echo ""
    echo "  The routing brain, on the board that drives the rack. Everything a"
    echo "  primary has — topology, web UI, Shelly polling, NodeLink, the screen"
    echo "  — with the four PWM channels traded for one bus servo on D6/D7 and"
    echo "  two endstops on D8/D9."
    echo ""
    echo "  It HOMES BEFORE IT CAN MOVE: a step-counting servo has no datum of"
    echo "  its own. Expect the carriage to sweep after the first boot."
    echo ""
  else
    echo "▶ Real hardware — flashing a PRIMARY (XIAO C5)."
  fi
  local port
  port="$(require_port primary)" || exit 1
  echo "  Using port: $port"
  echo ""

  # Settle the provisioning values: a flag beats tools/.env, --ask prompts over
  # either, and an empty .env still prompts on its own so a first-run board can't
  # be flashed with no way onto a network.
  if [[ "$*" != *"--no-provision"* && "$*" != *"--provision-only"* ]]; then
    apply_provision_overrides 1
    confirm_primary_hostname "$@"
  elif [[ -n "$OV_HOST$OV_SSID$OV_PASS" ]]; then
    echo "  ⚠  Ignoring --host/--ssid/--pass: provisioning is disabled by --no-provision."
  fi

  cd "$SCRIPT_DIR"
  # The name the board is answering to RIGHT NOW, which is not HOSTNAME_CFG once
  # the prompt above has renamed it. deploy.sh needs it to read the shop layout
  # off the device before the flash erases it — see backup_candidates() there.
  # Passed explicitly rather than left for deploy.sh to re-read from tools/.env:
  # with --save that file has already been rewritten to the NEW name by now, and
  # the old one would be gone.
  export DUSTGATE_PREV_HOST="${ENV_HOST:-}"
  # deploy.sh's internal `pio run` calls pick this up automatically —
  # PlatformIO honors PLATFORMIO_UPLOAD_PORT as an override for upload_port.
  PLATFORMIO_UPLOAD_PORT="$port" bash deploy.sh "--env=$FLASH_ENV" "$@"

  echo ""
  echo "  If the device doesn't respond below (no boot log, WiFi not connecting,"
  echo "  serial commands ignored), press the physical RESET button once — the"
  echo "  post-flash reset handshake is occasionally unreliable on this board."
  echo ""
  echo "▶ Opening serial monitor so you can see what's happening (Ctrl+C to exit)…"
  run_monitor --scan-boot ${FLASH_ENV:+"$FLASH_ENV"}
}

run_flash_node() {
  # A bare argument is the hostname; --slider picks the rack build.
  local node_env="$NODE_ENV" args=()
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --slider|--linear|--rack) node_env="$LINEAR_NODE_ENV"; shift ;;
      *) args+=("$1"); shift ;;
    esac
  done
  set -- "${args[@]+"${args[@]}"}"

  if [[ "$node_env" == "$LINEAR_NODE_ENV" ]]; then
    echo "▶ Secondary NODE — flashing the SLIDER firmware (XIAO C5 + ST3215)."
    echo "  Target: $(describe_env "$node_env")"
    echo ""
    echo "  This node drives ONE rack instead of a servo bank: one bus servo on"
    echo "  D6/D7, two endstops on D8/D9, and its own 12V supply at the gate. The"
    echo "  primary sends it an absolute position in mm and it turns that into"
    echo "  encoder counts."
    echo ""
    echo "  IT HOMES AT BOOT, and that is the one thing this node decides for"
    echo "  itself. A step-counting servo has no datum and comes back from a"
    echo "  power cycle holding nothing, so the carriage WILL sweep on the first"
    echo "  boot after flashing. Moves sent during the sweep are held, not lost."
    echo ""
    echo "  + the same SSD1306 status screen and wake button as any other board."
    echo "    On a slider it also says 'not homed' — which is the state in which"
    echo "    the node accepts moves and holds them."
    echo ""
  else
  echo "▶ Secondary NODE — flashing the servo-only firmware."
  echo "  Target: $(describe_env "$node_env")"
  echo ""
  echo "  A node is a dumb actuator bank: it drives up to four servo valves and"
  echo "  nothing else. No web UI, no plug polling — the primary does all the"
  echo "  thinking and sends it already-resolved angles."
  echo ""
  echo "  + SSD1306 status screen and its wake button, compiled in and probed for"
  echo "    at boot — no panel, one line on serial, carry on. A node's screen"
  echo "    answers one question: can the brain reach me. It blanks itself after"
  echo "    two minutes; the button toggles it."
  echo ""
  fi

  local port
  port="$(require_port node)" || exit 1
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
  PLATFORMIO_UPLOAD_PORT="$port" bash deploy.sh "--node=$node_env"

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
# NOT optional dressing — see the -e note below. Defaults to the primary.
run_monitor() {
  echo "▶ Serial monitor (Ctrl+C to exit)."
  local scan_boot=false env="$PRIMARY_ENV"
  local a
  for a in "$@"; do
    case "$a" in
      --scan-boot) scan_boot=true ;;
      *)           env="$a" ;;
    esac
  done

  # Which physical board, from which env: the two are pinned to roles, and
  # getting it wrong is silent — the monitor opens the other board's port and
  # shows nothing.
  local role=primary
  [[ "$env" == "$NODE_ENV" || "$env" == "$LINEAR_NODE_ENV" ]] && role=node

  local port
  port="$(require_port "$role")" || exit 1
  local what; what="$(describe_port "$port")"
  echo "  Using port: $port${what:+  ($what)}"

  # A monitor left running from an earlier session holds the port open, and the
  # second one fails in ways that look like a board problem rather than a
  # bookkeeping one. Worse, an old monitor may be sitting on the OTHER board —
  # the mismatch this whole port-pinning exercise exists to prevent, and easy to
  # hit when the two boards look identical.
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

  # -e is REQUIRED, not a nicety: it picks which build's .elf the exception
  # decoder loads, so the wrong env decodes crash addresses against the wrong
  # binary. Both envs share DTR/RTS handling, so a mismatch is quiet.
  local env_args=()
  [[ -n "$env" ]] && env_args=(-e "$env")

  # `pio device monitor -e` reads that env's platform, so it has to look in the
  # same core dir the build used — otherwise monitoring the C5 asks the OFFICIAL
  # installation for a platform only the fork's has, and pio starts trying to
  # install it. Same call the build makes; see tools/boardinfo.sh.
  use_core_for_env "$env" >/dev/null

  # RECONNECT IS ON: the C5's port comes straight off the MCU and disappears on
  # EVERY reset, so reconnect is what lets the monitor survive a reboot instead of
  # exiting at the first one. DUSTGATE_MONITOR_NO_RECONNECT=1 turns it off.
  local reconnect_arg=""
  [[ "${DUSTGATE_MONITOR_NO_RECONNECT:-0}" == "1" ]] && reconnect_arg="--no-reconnect"

  "$PIO" device monitor --port "$port" \
      ${env_args[@]+"${env_args[@]}"} \
      ${reconnect_arg:+"$reconnect_arg"}
}

run_erase() {
  echo "▶ Full chip erase — wipes firmware AND filesystem."
  echo "  Use this if you're seeing corrupted-partition symptoms (e.g."
  echo "  persistent 'LittleFS mount failed' after reflashing normally)."
  echo "  Erases whichever board is attached — role doesn't matter here."
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
  echo "  Every board is a XIAO C5. Primary or node is which program you flash."
  echo ""
  echo "  1) Demo       — browser only, fully simulated, no backend"
  echo "  2) Mock       — ng serve + tools/mock-api.js (real API contract)"
  echo "  3) Live       — local UI + hot reload, talking to REAL hardware"
  echo ""
  echo "  4) Flash a PRIMARY      — UI + firmware + filesystem + provision"
  echo "     4f = firmware only     4u = UI/filesystem only"
  echo "     4s = the SLIDER primary (ST3215 rack instead of PWM valves)"
  echo "  5) Flash a NODE         — servo-only firmware + WiFi creds"
  echo "     5s = a SLIDER node (one rack, homes itself at boot)"
  echo ""
  echo "  6) Monitor the PRIMARY      (6n = monitor a NODE instead)"
  echo "  7) Ports — list attached boards, and pin one to a role"
  echo "  8) (Re)send WiFi/key/hostname to an already-flashed board"
  echo "  9) Full chip erase (fixes corrupted-partition weirdness)"
  echo "  q) Quit"
  echo ""
  read -rp "Choose: " choice
  case "$choice" in
    1) run_demo ;;
    2) run_mock ;;
    3) read -rp "  Device host [dustgate.local]: " h; run_live "${h:-dustgate.local}" ;;
    4) run_flash ;;
    4f|4F) run_flash --fw ;;
    4u|4U) run_flash --ui ;;
    4s|4S) run_flash --slider ;;
    5) run_flash_node ;;
    5s|5S) run_flash_node --slider ;;
    6) run_monitor ;;
    6n|6N) run_monitor "$NODE_ENV" ;;
    7) run_ports ;;
    8) run_provision ;;
    9) run_erase ;;
    q|Q) exit 0 ;;
    *) echo "Unknown choice."; show_menu ;;
  esac
}

case "${1:-}" in
  ports)     run_ports "${2:-}" "${3:-}" ;;
  demo)      run_demo ;;
  mock)      run_mock ;;
  flash)     shift; run_flash "$@" ;;
  # "monitor node" targets a secondary: picks the board pinned as the node, and
  # applies the node env's monitor settings.
  monitor)
    shift || true
    case "${1:-}" in
      node|n)     run_monitor "$NODE_ENV" ;;
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
    echo "Usage: dev.sh [demo|mock|live [host]"
    echo "              |flash [--fw|--ui|--slider|--no-provision] [--host N] [--ssid N] [--pass S] [--ask] [--save]"
    echo "              |flash-node [--slider] [hostname]"
    echo "              |monitor [node]|ports [--pin primary|node]|erase|provision [--host N] [--ssid N]]"
    exit 1
    ;;
esac

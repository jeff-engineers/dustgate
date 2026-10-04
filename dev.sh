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
#   bash dev.sh flash-node [host]   # NODE: node firmware + WiFi creds
#     Three servo valves, a CT clamp, no UI, no plug polling. NOT "a dumb
#     actuator bank" any more — a node owns any control loop faster than a WiFi
#     round trip (a homing sweep, a 60 Hz RMS) and no interpretation of the
#     document. Its hostname is load-bearing (mDNS, the Boards screen, link.host
#     in the topology) and must be unique per node.
#
#   THE COLLECTOR BOARD — no flag, and that is the point (2026-09-16):
#
#   bash dev.sh flash collector       # a PRIMARY at the collector
#   bash dev.sh flash-node collector  # ...or a NODE, if the brain is elsewhere
#     There is no --collector any more, because there is no collector BUILD.
#     One pin map serves every PWM board — CT on D0, bin on D6, servo channels
#     0/1/2 on D7/D8/D9, transmitter on D10 — so what makes a board a collector
#     is the LAYOUT pointing it at a bin, a clamp and a remote.
#     ⚠️ A NODE still has neither the bin sensor nor the RF transmitter: both
#     live in firmware.ino and were never moved. Clamps DO work on a node.
#     firmware/WIRING.md#9-bin-sensor has the table of what works where.
#
#   THE SLIDER BOARD — add --slider to either flash command:
#
#   bash dev.sh flash --slider        # a PRIMARY that drives the rack
#   bash dev.sh flash-node --slider   # a NODE that drives the rack
#     Same XIAO C5, same two roles. What changes is that the three PWM channels
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
#   BENCH COMMANDS — typed at the serial console (bash dev.sh monitor):
#
#     press                   Fire the 315 MHz transmitter ONCE, now. Bypasses
#                             the retry policy — no cooldown, no spin-up grace,
#                             no sensor needed. Falls back to D10 (PIN_RF_TX,
#                             moved from D9 on 2026-09-16) and the measured
#                             Rockler address when no layout names one, so it
#                             works before any control.rf block exists.
#                             ⚠️ MOVE THE MODULE'S DATA WIRE if this board was
#                             built before that date — D9 is servo channel 2 now.
#                             ⚠️ Put a LAMP in the receiver's outlet, not the
#                             collector: a blower cannot spin up and coast down
#                             fast enough to read.
#
#     rfscan                  Try the four ways a DIP switch gets copied wrong
#                             — as entered, inverted, reversed, both — and keep
#                             whichever the collector answers. Needs the
#                             collector's SENSOR plug paired: the method is
#                             press-and-see, and that plug is the seeing.
#                             SETUP ONLY. An inverted address is a valid address
#                             for someone else's receiver; watch it run.
#
#     stroke <1-3> <from> <to> [reps] [dwellMs]
#                             Press and release a servo, repeatably, then
#                             DETACH. For finding out whether a 9g servo can
#                             throw a given switch — there is no torque number
#                             to read, so the measurement is watching it try.
#                             e.g. stroke 1 20 90 5
#                             Try a SHORTER ARM before concluding you need metal
#                             gears: torque at the switch is force x radius.
#
#     ct [n]                  Read the CT clamp n times, one per second. Prints
#                             amps, the DC bias point and the sample rate.
#                             SERIAL ONLY, on purpose: the screen's charge pump
#                             is a noise source for this very measurement, so a
#                             reading you can only see by lighting the thing that
#                             corrupts it is no reading at all.
#                             ⚠️ It refuses to let 0.000 A pass unqualified when
#                             the bias is RAILED — a railed pin reads a constant,
#                             and the variance of a constant is zero, which looks
#                             exactly like a perfectly quiet sensor. D0 should
#                             sit at ~1650 mV.
#
#     servo <1-3> <deg>       Move one servo. `servo N detach` de-energises it.
#     mdnsprobe               What answers mDNS here, and how fast.
#     sweep [from] [to]       Knock on every address looking for a Tasmota.
#     probe <ip>              Why one address did not answer.
#     help                    Everything. There is no longer a collector build
#                             for these to be "the collector commands" of —
#                             every PWM board has the CT, the bin pad and the
#                             transmitter, and the layout decides who uses them.
#
#   bash dev.sh monitor             # serial monitor (primary)
#   bash dev.sh monitor node        # ...a node instead
#   bash dev.sh monitor both        # primary + node interleaved, one clock, logged
#                                   #   to .monitor-logs/; type `p: cmd` / `n: cmd`
#   bash dev.sh monitor … --take    # stop whatever already holds that board's port
#   bash dev.sh ota [host] [--slider] [--no-nodes|--nodes-only]
#                                   # update over WiFi: the primary (rolls back if it cannot stay on
#                                   #   WiFi) and the node images it serves; tap "update" per board in
#                                   #   the app. Needs the OTA partition tables, one cable flash each.
#   bash dev.sh linklog [host]      # the primary's link log over WiFi, saved + summarised
#   bash dev.sh log [host] [--once] # follow the brain's serial output over WiFi, no cable
#                                   #   (drops, outages, node reboots, rejoins) — no USB needed
#   bash dev.sh ports               # list attached boards + which role each is pinned to
#   bash dev.sh ports --pin primary # pin the attached board to a role (do this once)
#   bash dev.sh ports --pin node
#     DUSTGATE_PORT=/dev/cu.xxx     # force a port for this one command
#     DUSTGATE_PORT_PRIMARY=…       # force a port for a role, e.g. in ~/.zshrc
#     DUSTGATE_PORT_NODE=…
#   bash dev.sh erase               # full chip erase (fixes corrupted-partition weirdness)
#   bash dev.sh provision           # (re)send WiFi/key/hostname without reflashing
#   bash dev.sh provision node      # ...to the board pinned as the NODE
#     Works on a node since 2026-09-16. Before that the node program had no
#     serial reader outside the captive portal, so this command — and the
#     hostname `flash-node` prompts for — were silently dropped on any node that
#     already had WiFi credentials in NVS.
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
# three PWM channels are traded for one ST3215 on a serial bus, because PWM and
# serial never share a board. A slider board is therefore a THIRD thing to flash
# in each role, not a flag on the servo build, and `--slider` picks it.
LINEAR_PRIMARY_ENV="xiao_c5_linear_primary"
LINEAR_NODE_ENV="xiao_c5_linear"

# THE COLLECTOR ENVS ARE GONE (2026-09-16). There used to be a pair here, a
# fourth thing to flash in each role, because a CT and a transmitter would not
# fit beside four servo channels. Three channels puts the transmitter on D10 and
# every pad gets one owner, so a collector board is an ordinary primary or an
# ordinary node. `--collector` is still ACCEPTED below and does nothing but
# print, so muscle memory does not fail a flash.

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

# What each physical NODE was last flashed as, keyed by its USB serial.
#
# WHY NODES AND NOT THE PRIMARY. The primary's name is a convenience and is
# already remembered in tools/.env when you pass --save. A node's name is
# LOAD-BEARING and is not remembered at all, which is exactly backwards: it is
# what the board advertises over mDNS, what the Boards screen lists, and what the
# topology stores as link.host. Reflash a node and type a different name — or
# accept the suggestion, which is a generic dustgate-node-N — and three things
# break at once, all silently:
#
#   - the primary's NodeRegistry still holds the OLD name, paired and persisted,
#     and re-resolves it over mDNS forever. That is a ghost entry taking the
#     shop-wide querier lock every 15s for a board that no longer exists.
#   - the layout's link.host points at the old name, so its gates stop moving.
#   - allNodesLinked() can never be true again, so the primary sits on ONLINE
#     rather than READY and the status light never goes green.
#
# The serial is the right key because it is the board, not the port: the C5's
# USB comes off the MCU, so the /dev path moves around and the serial does not.
# Same identifier PORTS_FILE already pins roles by.
NODE_NAMES_FILE="$(dirname "${BASH_SOURCE[0]}")/.dustgate-node-names"

# The serial of the board currently on $1, or empty.
serial_for_port() {
  local want="$1"
  list_boards | awk -F'|' -v p="$want" '$1 == p { print $3; exit }'
}

node_name_for_serial() {
  [[ -f "$NODE_NAMES_FILE" && -n "${1:-}" ]] || return 0
  awk -F'=' -v s="$1" '$1 == s { print $2; exit }' "$NODE_NAMES_FILE"
}

remember_node_name() {
  local ser="${1:-}" name="${2:-}"
  [[ -n "$ser" && -n "$name" ]] || return 0
  local tmp; tmp="$(mktemp)"
  [[ -f "$NODE_NAMES_FILE" ]] && grep -v "^${ser}=" "$NODE_NAMES_FILE" > "$tmp" 2>/dev/null
  echo "${ser}=${name}" >> "$tmp"
  mv "$tmp" "$NODE_NAMES_FILE"
}

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
    # …EXCEPT for a node, when exactly one OTHER board is attached. A shop has
    # many nodes and one pin, so "the pinned node is not here" is the ordinary
    # state while adding boards — refusing it sent a perfectly good new board to
    # "no board for node is attached" (2026-09-28). Safe because it is exactly
    # one board and never the pinned primary; two or more is a guess and still
    # refuses, which is the case the strict rule below exists for.
    if [[ "$role" == "node" ]]; then
      local prim cands
      prim="$(pinned_serial primary)"
      cands="$(awk -F'|' -v p="$prim" 'NF && (p == "" || $3 != p) { print $1 }' <<<"$boards")"
      if [[ "$(grep -c . <<<"$cands")" -eq 1 ]]; then echo "$cands"; return; fi
    fi
    # PINNED BUT NOT ATTACHED IS AN ANSWER, not a reason to guess. This used to
    # fall through to "first board enumerated", so a stale pin (a board since
    # swapped out) quietly sent BOTH roles to the same port — `monitor` and
    # `monitor node` then fought over one board and the second died on a port
    # lock, which read as a monitoring bug rather than a pinning one
    # (2026-09-27). Empty output here; require_port explains why.
    return 0
  fi

  # Nothing pinned for this role: take a board the OTHER role isn't pinned to.
  # NEVER the other role's board — with the primary pinned and plugged in alone,
  # `monitor node` used to fall back to "first enumerated", which was the primary.
  local other_role other_want
  [[ "$role" == "primary" ]] && other_role="node" || other_role="primary"
  other_want="$(pinned_serial "$other_role")"
  # Two unpinned boards is still a guess — report_port_choice says so.
  awk -F'|' -v s="$other_want" 's == "" || $3 != s { print $1; exit }' <<<"$boards"
}

# Why detect_port came back empty, for a role — so a failure names the fix
# instead of waiting a minute for a board that is already plugged in.
explain_no_port() {
  local role="$1" boards want other_role other_want
  boards="$(list_boards)"
  [[ -z "$boards" ]] && return 1          # genuinely nothing attached: caller's text
  want="$(pinned_serial "$role")"
  [[ "$role" == "primary" ]] && other_role="node" || other_role="primary"
  other_want="$(pinned_serial "$other_role")"
  echo "  ✗ No board for '$role' is attached." >&2
  if [[ -n "$want" ]]; then
    echo "    '$role' is pinned to serial $want, and no attached board has it." >&2
  else
    echo "    Every attached board is pinned as '$other_role'." >&2
  fi
  echo "    Attached:" >&2
  local port vid ser desc tag
  while IFS='|' read -r port vid ser desc; do
    [[ -z "$port" ]] && continue
    tag=""; [[ -n "$other_want" && "$ser" == "$other_want" ]] && tag="  (pinned: $other_role)"
    printf "      %-24s %s%s\n" "$port" "$ser" "$tag" >&2
  done <<<"$boards"
  echo "    Plug that board in, or re-pin:  bash dev.sh ports --pin $role" >&2
  return 0
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
    printf "     %s%-24s %-20s %s\n" "$mark" "$port" "$ser" "${desc:-$vid}"
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
  awk -F'|' -v p="$port" '$1 == p { printf "%s, serial %s", $4, $3; exit }' <<<"$(list_boards)"
}

# `dev.sh ports` — show what's attached; `--pin ROLE` records a board's SERIAL
# against that role so later commands are deterministic.
#
# One role at a time, because the boards are indistinguishable: the honest
# workflow is to plug in the one you mean and say which it is. With more than one
# attached it asks rather than guessing.
# board_network_names — one "mac|hostname|ip" line per DustGate board on the LAN.
#
# WHICH USB CABLE IS WHICH BOARD. Every board is the same part with the same VID,
# so the port list alone cannot say which one is the primary — the question
# `ports --pin` asks and nobody could answer without walking to the OLED. But the
# USB serial of a C5's USB-JTAG unit IS the chip's base MAC, and the WiFi station
# uses that same MAC (checked 2026-09-27: 10:BD:A3:C8:4B:0C is dustgate.local,
# 38:44:BE:BE:92:28 is dustgate-planer.local). So: browse _dustgate._tcp, resolve
# each name, ping once to fill the ARP cache, and read the MAC back from it.
#
# A CONVENIENCE, NEVER A REQUIREMENT — CLAUDE.md's rule about multicast applies to
# the bench tools too. Boards with WiFi off, unprovisioned, or on a network that
# blocks mDNS simply come back without a name; nothing here fails because of it.
# DUSTGATE_PORTS_NO_NET=1 skips the lookup entirely (~2s).
board_network_names() {
  [[ "${DUSTGATE_PORTS_NO_NET:-0}" == "1" ]] && return 0
  command -v dns-sd >/dev/null 2>&1 || return 0
  local names name ip mac
  # dns-sd -B never exits on its own; perl's alarm is the portable timeout (macOS
  # has no `timeout`). Column 7 onward is the instance name, which is the hostname
  # DustGate registers (the same string MDNS.begin() was given).
  names="$(perl -e 'alarm 2; exec @ARGV' dns-sd -B _dustgate._tcp local. 2>/dev/null \
            | awk '$2 == "Add" { n=$7; for (i=8; i<=NF; i++) n=n" "$i; print n }' | sort -u)"
  # One board per background job, so a shop of N boards costs ~1s, not N.
  while IFS= read -r name; do
    [[ -z "$name" ]] && continue
    (
      # dns-sd -G, NOT dscacheutil: dscacheutil holds its IPv4 answer for ~5s
      # waiting on an AAAA the ESP32 never sends. dns-sd prints the A record the
      # moment it lands; the alarm only bounds a board that does not answer.
      ip="$(perl -e 'alarm 1; exec @ARGV' dns-sd -G v4 "$name.local" 2>/dev/null \
              | awk '$2 == "Add" && $6 ~ /^[0-9.]+$/ { print $6; exit }')"
      [[ -z "$ip" ]] && exit 0
      ping -c1 -t1 "$ip" >/dev/null 2>&1 || true
      # arp drops leading zeros ("4b:c"); pad each octet so it compares to the serial.
      mac="$(arp -n "$ip" 2>/dev/null | awk '{ print $4 }' \
              | awk -F: 'NF == 6 { for (i=1; i<=6; i++) printf "%s%02s", (i>1?":":""), toupper($i); print "" }' \
              | tr ' ' '0')"
      [[ -n "$mac" ]] && echo "$mac|$name|$ip"
    ) &
  done <<<"$names"
  wait
}

# "hostname (ip)" for a USB serial, from board_network_names output, or "".
name_for_serial() {
  local ser="$1" names="$2"
  awk -F'|' -v s="$(tr '[:lower:]' '[:upper:]' <<<"$ser")" \
      '$1 == s { printf "%s.local (%s)", $2, $3; exit }' <<<"$names"
}

run_ports() {
  local boards; boards="$(list_boards)"
  if [[ -z "$boards" ]]; then
    echo "No ESP32 boards found. Use a DATA cable, and check 'pio device list'."
    return 1
  fi
  local names; names="$(board_network_names)"
  echo "Attached boards:"
  local port vid ser desc net role pinned_as
  while IFS='|' read -r port vid ser desc; do
    [[ -z "$port" ]] && continue
    net="$(name_for_serial "$ser" "$names")"
    pinned_as=""
    for role in primary node; do
      [[ "$(pinned_serial "$role")" == "$ser" ]] && pinned_as="  [pinned: $role]"
    done
    printf "  %-24s %-20s %s%s\n" "$port" "$ser" "${net:-(not seen on the network)}" "$pinned_as"
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
      printf "  %d) %-24s %-20s %s\n" "$i" "$port" "$ser" "$(name_for_serial "$ser" "$names")"
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
  echo "Pinned $role to $chosen_port (serial ${chosen_ser}) — survives replugging"
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
  # Boards ARE attached, just not this role's — waiting a minute will not help.
  explain_no_port "$role" && return 1

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
      # three PWM channels. Consumed here rather than passed through, because the
      # env it selects is handed to deploy.sh as --env= below.
      --slider|--linear|--rack) FLASH_ENV="$LINEAR_PRIMARY_ENV"; shift ;;
      # No longer selects anything — kept so an old command line still works.
      # The ordinary primary IS the collector build now; the banner says so.
      --collector|--dc) FLASH_COLLECTOR=1; shift ;;
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
    # Says WHERE the kept password comes from: "the stored one" read as the
    # DEVICE's password, and it never was — it is tools/.env's, which is paired
    # here with a NEW SSID, so the difference matters (asked 2026-09-28).
    read -rsp "  WiFi Password for '$WIFI_SSID' [Enter keeps the one in tools/.env]: " reply || true
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

  if [[ "${FLASH_COLLECTOR:-0}" == "1" ]]; then
    echo "▶ Real hardware — flashing a board for the COLLECTOR."
    echo "  Target: $(describe_env "$FLASH_ENV")"
    echo ""
    echo "  THERE IS NO COLLECTOR BUILD ANY MORE (2026-09-16). This is the"
    echo "  ordinary primary, and one pin map serves every PWM board:"
    echo ""
    echo "        D0  CT clamp         the only analog pad on the edge"
    echo "        D6  bin sensor       opto output, LOW = full"
    echo "        D7  servo channel 0  the gate, if this board drives one"
    echo "        D8  servo channel 1  fob servo, ON"
    echo "        D9  servo channel 2  fob servo, OFF"
    echo "        D10 315 MHz TX       and nothing else shares it"
    echo ""
    echo "  What makes this a collector is the LAYOUT — a bin, a clamp and a"
    echo "  remote pointed at this board — not the firmware on it."
    echo ""
    echo "  ⚠️  WHAT ACTUALLY WORKS TODAY is less than that list implies:"
    echo "        fob servos    yes — ordinary servo channels (servo / stroke)"
    echo "        RF TX         yes on a PRIMARY; NOT on a node (still in .ino)"
    echo "        bin sensor    yes on a PRIMARY; NOT on a node (still in .ino)"
    echo "        CT clamp      code is in on both, NEVER RUN on either"
    echo "      firmware/WIRING.md#9-bin-sensor has the table."
    echo ""
    echo "  ⚠️  THIS IS A COMPLETE PRIMARY — web UI, topology, plug polling."
    echo "      A one-collector shop is a whole shop, so that is right. In a shop"
    echo "      that ALREADY has a routing brain, flash the node instead:"
    echo "      two primaries on one network fight over the topology and the"
    echo "      mDNS name. Either way, give this board its own hostname."
    echo ""
    echo "  Bench commands once it is up (bash dev.sh monitor):"
    echo "      press                       fire the RF transmitter once"
    echo "      rfscan                      find the fob's address by trying"
    echo "      stroke <1-3> <from> <to> [n]  press a switch, repeatably"
    echo "      ct [n]                      read the clamp (serial only)"
    echo ""
  elif [[ "$FLASH_ENV" == "$LINEAR_PRIMARY_ENV" ]]; then
    echo "▶ Real hardware — flashing a SLIDER PRIMARY (XIAO C5 + ST3215)."
    echo "  Target: $(describe_env "$FLASH_ENV")"
    echo ""
    echo "  The routing brain, on the board that drives the rack. Everything a"
    echo "  primary has — topology, web UI, Shelly polling, NodeLink, the screen"
    echo "  — with the three PWM channels traded for one bus servo on D6/D7 and"
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

  # NOTHING ELSE MAY HAVE THE PORT OPEN WHILE WE FLASH. macOS lets two programs
  # open one serial device, and a monitor reading alongside esptool steals the
  # chip's replies — on 2026-09-27 that killed a filesystem flash at 14% ("The
  # chip stopped responding") and left the board without its UI or layout. A
  # monitor is exactly what is usually running at the bench, so refuse up front.
  port_is_free "$port" false flash || exit 1
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
  # The board's port, under OUR name — deploy.sh passes it to PlatformIO as
  # --upload-port on the upload steps only. NOT as PLATFORMIO_UPLOAD_PORT: that
  # variable folds into PlatformIO's project config, which is hashed into the
  # build-folder checksum, so every flash to a different port (primary 1401 vs
  # node 1101) wiped and rebuilt every env from scratch (found 2026-09-28).
  DUSTGATE_UPLOAD_PORT="$port" bash deploy.sh "--env=$FLASH_ENV" "$@"

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
      # Accepted and ignored: the ordinary node IS the collector node now.
      --collector|--dc) shift ;;
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
  echo "▶ Secondary NODE — flashing the node firmware."
  echo "  Target: $(describe_env "$node_env")"
  echo ""
  echo "  Up to THREE servo valves (D7/D8/D9) and a CT clamp (D0). No web UI, no"
  echo "  plug polling: the primary owns the document and sends already-resolved"
  echo "  angles, never state names."
  echo ""
  echo "  NOT \"a dumb actuator bank\" — that phrasing was wrong twice. A node"
  echo "  owns any control loop FASTER THAN A WIFI ROUND TRIP and no"
  echo "  interpretation of the document: a slider node owns its homing sweep,"
  echo "  and this one owns a 60 Hz RMS loop, because neither can round-trip per"
  echo "  sample. What it never owns is what the reading MEANS."
  echo ""
  echo "  ⚠️  A node still has NO bin sensor and NO RF transmitter — both live in"
  echo "     firmware.ino and were never moved. The clamp does work here."
  echo ""
  echo "  + SSD1306 status screen and its wake button, compiled in and probed for"
  echo "    at boot — no panel, one line on serial, carry on. A node's screen"
  echo "    answers one question: can the brain reach me. It blanks itself after"
  echo "    two minutes; the button toggles it."
  echo ""
  fi

  local port
  port="$(require_port node)" || exit 1

  # NOTHING ELSE MAY HAVE THE PORT OPEN WHILE WE FLASH. macOS lets two programs
  # open one serial device, and a monitor reading alongside esptool steals the
  # chip's replies — on 2026-09-27 that killed a filesystem flash at 14% ("The
  # chip stopped responding") and left the board without its UI or layout. A
  # monitor is exactly what is usually running at the bench, so refuse up front.
  port_is_free "$port" false flash || exit 1
  { what="$(describe_port "$port")"; echo "  Using port: $port${what:+  ($what)}"; }

  # WiFi creds first. The primary CANNOT provision a node over the network — the
  # node isn't on the network yet, which is the whole chicken-and-egg. So we push
  # credentials over this USB cable now, the same way the primary gets them.
  # STORED CREDENTIALS ARE USED WITHOUT ASKING, the same as the primary — jeff,
  # 2026-09-17. This prompted every time even with tools/.env fully populated,
  # which is two questions per node whose only right answer is Enter; and a shop
  # is flashed one node at a time, so the prompts scale with the boards.
  #
  # The flags behave exactly as they do on a primary, because they go through the
  # same parser: --ask forces the questions, --ssid/--pass override for this flash,
  # --save writes them back. Only a genuinely empty tools/.env still prompts,
  # which is the first-run case that must not be silent.
  parse_provision_overrides "$@"
  set -- "${PROVISION_REST[@]+"${PROVISION_REST[@]}"}"
  load_env_defaults
  if (( OV_ASK )) || [[ -z "$ENV_SSID" ]]; then
    [[ -z "$ENV_SSID" ]] && echo "  No WiFi credentials found in tools/.env yet — let's set them up."
    echo ""
    echo "  WiFi credentials (a node needs these to reach the primary):"
    read -rp "  WiFi SSID${ENV_SSID:+ [$ENV_SSID]}: " WIFI_SSID
    WIFI_SSID="${WIFI_SSID:-$ENV_SSID}"
    read -rsp "  WiFi Password${ENV_PASS:+ [unchanged, hidden]}: " WIFI_PASS; echo
    WIFI_PASS="${WIFI_PASS:-$ENV_PASS}"
  else
    WIFI_SSID="${OV_SSID:-$ENV_SSID}"
    WIFI_PASS="${OV_PASS:-$ENV_PASS}"
    echo ""
    echo "  WiFi: '$WIFI_SSID' (from tools/.env — --ask to change)"
  fi

  # The hostname is LOAD-BEARING here, unlike on the primary. It's what the node
  # advertises over mDNS, what the Boards screen lists, and what gets written
  # into the topology as link.host. Two nodes sharing a hostname collide on the
  # network and the primary can only ever reach one of them — so this is a
  # required, distinct value, not a nicety.
  # SUGGEST THE NAME THIS BOARD ALREADY HAD, keyed by its USB serial — added
  # 2026-09-17. Reflashing a node is the common case, and the common case should
  # not be the one that silently renames a board (see NODE_NAMES_FILE for what
  # that costs). An explicit argument still wins; the remembered name beats the
  # generic dustgate-node-N, which is only right for a board being named for the
  # first time.
  local node_ser; node_ser="$(serial_for_port "$port")"
  local remembered; remembered="$(node_name_for_serial "$node_ser")"
  # AN EXPLICIT NAME IS AN ANSWER, NOT A SUGGESTION — and it arrives two ways.
  # `flash-node <name>` leaves it in $1, while `--host <name>` lands in OV_HOST;
  # parse_provision_overrides also folds a BARE word into OV_HOST (see its `*)`
  # case), which is why $1 can be empty even though a name was typed. That swallow
  # is what made this prompt appear anyway on 2026-09-18.
  local explicit="${1:-${OV_HOST:-}}"
  local suggested="${explicit:-${remembered:-$(next_node_hostname)}}"

  echo ""
  if [[ -n "$remembered" && "$remembered" != "$explicit" ]]; then
    echo "  This board was last flashed as '$remembered'."
    echo "  KEEP THAT NAME unless you mean to re-identify it: the primary still has"
    echo "  the old one paired, and the layout points its gates at it."
    echo ""
  fi

  if [[ -n "$explicit" ]]; then
    # Same rule the primary follows: a name given on the command line is not
    # re-asked. Printed, so it is never silent about which name it is using.
    HOSTNAME_CFG="$explicit"
    echo "  Node hostname: $HOSTNAME_CFG  (given on the command line)"
  else
    read -rp "  Node hostname — must be unique per node [$suggested]: " HOSTNAME_CFG
    HOSTNAME_CFG="${HOSTNAME_CFG:-$suggested}"
  fi
  # TWO names are refused, and the second one was learnt the hard way.
  #
  #   - whatever tools/.env calls the primary, which is the obvious collision
  #   - the literal string "dustgate", ALWAYS, because that is
  #     DEFAULT_HOSTNAME in firmware/utils/WiFiConfig.h — the name an
  #     unprovisioned board answers to. A shop whose primary is called
  #     `dustgate-shop` sails straight past the first check while the node
  #     still ends up owning `dustgate.local`, which is what happened on
  #     2026-09-16.
  if [[ "$HOSTNAME_CFG" == "${ENV_HOST:-dustgate}" || "$HOSTNAME_CFG" == "dustgate" ]]; then
    echo ""
    echo "  ✗ '$HOSTNAME_CFG' is not a name a NODE may have."
    if [[ "$HOSTNAME_CFG" == "dustgate" ]]; then
      echo "    It is the firmware's built-in default (DEFAULT_HOSTNAME), so an"
      echo "    unprovisioned board already answers to it — a node taking it"
      echo "    fights the brain for dustgate.local and wins about half the time."
    else
      echo "    It is the PRIMARY's hostname."
    fi
    echo "    Re-run and pick something like $(next_node_hostname), or name it"
    echo "    after the machine it sits on: dustgate-planer, dustgate-tablesaw."
    exit 1
  fi

  export WIFI_SSID WIFI_PASS HOSTNAME_CFG

  echo ""
  echo "  Flashing as: $HOSTNAME_CFG  (will appear at $HOSTNAME_CFG.local)"
  if [[ -n "$remembered" && "$remembered" != "$HOSTNAME_CFG" ]]; then
    echo ""
    echo "  ⚠  RENAMING this board: '$remembered' → '$HOSTNAME_CFG'."
    echo "     The primary still has '$remembered' paired and will keep dialling it."
    echo "     After this: unpair '$remembered' in the app, add '$HOSTNAME_CFG',"
    echo "     and re-point any gates the layout gave it."
  fi
  echo ""
  cd "$SCRIPT_DIR"
  DUSTGATE_UPLOAD_PORT="$port" bash deploy.sh "--node=$node_env"

  # Recorded AFTER the flash, so a failed upload does not claim a name the board
  # is not actually running.
  remember_node_name "$node_ser" "$HOSTNAME_CFG"

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
  # `provision node` targets the board pinned as the NODE, the same way
  # `monitor node` does. Without it this command could only ever reach the
  # primary-pinned port, which is the wrong half of the shop for the thing it is
  # most often needed for: a node that came up on the wrong hostname.
  local role=""
  if [[ "${1:-}" == "node" || "${1:-}" == "n" ]]; then role="node"; shift; fi

  # Same overrides as flash. Here they're arguably more useful: this is the
  # command for moving an already-flashed board onto a different network or
  # renaming it, which is exactly what a flag spares you re-typing.
  parse_provision_overrides "$@"

  # A NODE MUST BE NAMED, AND NOT AFTER THE PRIMARY (2026-09-28). The hostname
  # defaults to the saved one — which is the PRIMARY's (`dustgate`) — so
  # `provision node --ssid X` quietly renamed a node to the brain's own name,
  # and the two then fight over dustgate.local while the primary dials neither
  # reliably. The same default is what the port mix-up wrote into a node on
  # 2026-09-27. Asking for the name costs one flag; guessing it costs a node.
  if [[ "$role" == "node" ]]; then
    local primary_name="${ENV_HOST:-dustgate}"
    if [[ -z "$OV_HOST" ]]; then
      echo "  ✗ A node needs its own name. Say which:"
      echo "      bash dev.sh provision node --host dustgate-<name> [--ssid …]"
      echo "    (The saved default, '$primary_name', is the PRIMARY's name.)"
      echo "    Names on the network now:  bash dev.sh ports"
      exit 1
    fi
    if [[ "${OV_HOST%.local}" == "${primary_name%.local}" || "${OV_HOST%.local}" == "dustgate" ]]; then
      echo "  ✗ '$OV_HOST' is the primary's name — a node given it fights the primary for it."
      echo "    Pick the node's own name, e.g. --host dustgate-planer"
      exit 1
    fi
  fi

  echo "▶ (Re)send WiFi/key/hostname to an already-flashed ${role:-primary} board."
  local port
  port="$(require_port $role)" || exit 1
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
  # --env names the NODE build for a node, so the provision step describes the
  # right target. Without it deploy.sh fell back to the primary env and printed
  # "Target: xiao_c5_primary" while provisioning a node — the same C5 part, so it
  # wrote correctly, but the log claimed the planer was a brain (2026-09-28).
  DUSTGATE_UPLOAD_PORT="$port" bash deploy.sh --provision-only ${role:+"--env=$NODE_ENV"}
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

# port_is_free <port> <take:true|false>
# True when nothing holds the serial device. Otherwise names the holder — the
# PID and its command line, from lsof on the device itself — and either stops it
# (take=true, i.e. --take) or says how to, and returns false.
port_is_free() {
  local port="$1" take="${2:-false}" for_flash="${3:-}" pids pid
  pids="$(lsof -t "$port" 2>/dev/null | sort -u || true)"
  [[ -z "$pids" ]] && return 0
  echo ""
  echo "  ⚠  $port is already open in another program:"
  for pid in $pids; do
    printf "       %s  %s\n" "$pid" "$(ps -o command= -p "$pid" 2>/dev/null | cut -c1-110)"
  done
  if [[ "$take" == "true" ]]; then
    echo "     --take: stopping it."
    # shellcheck disable=SC2086
    kill $pids 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      [[ -z "$(lsof -t "$port" 2>/dev/null)" ]] && { echo ""; return 0; }
      sleep 0.3
    done
    echo "     It did not let go. Stop it by hand:  kill -9 $pids"
    return 1
  fi
  if [[ -n "$for_flash" ]]; then
    # --take is a MONITOR option; a flash says how to free the port instead of
    # offering a flag it does not have. Usually this is the monitor the previous
    # flash opened at its end, still running in its terminal window.
    echo "     A flash needs the port to itself. Press Ctrl+C in that program's window"
    echo "     (usually the monitor the last flash opened), or:  kill $(echo $pids)"
  else
    echo "     Only one program can have a serial port open. If that is a monitor you"
    echo "     still want, use it; otherwise close it, or rerun with --take."
    echo "     (A monitor on a DIFFERENT board is fine — only this port matters.)"
  fi
  return 1
}

# run_ota [--slider] [--no-nodes | --nodes-only] [host]
# Update over WiFi — no cable, no filesystem wipe, the saved layout is untouched.
#
#   1. builds the primary AND both node programs, so all three come from one commit
#      (a shop whose boards disagree about NodeLink is the failure this prevents);
#   2. posts the primary firmware to POST /api/ota and waits for the board to come
#      back and prove itself — the new image is on probation (firmware/utils/
#      OtaGuard.h) and the board rolls back by itself if it cannot stay on WiFi;
#   3. stages the node images on the primary (POST /api/node-image). It does NOT
#      push them to the nodes: the Boards screen shows "update available" per
#      board and YOU tap it, one at a time, when the shop is quiet.
#
# Needs a board whose partition table already has two app slots — ONE cable flash
# (`bash dev.sh flash`) installs that; /api/info says "ota":"nogo" until then.
# Does NOT touch the filesystem partition's contents, so a UI change still wants
# `flash --ui`. The API key comes off the board's own /api/info, as linklog does.
#   --no-nodes     primary firmware only (skips the two node builds)
#   --nodes-only   stage the node images, leave the primary alone
#   --bad          ROLLBACK TEST: images that never become healthy (see below)
run_ota() {
  local host="" env="$PRIMARY_ENV" do_primary=1 do_nodes=1 bad=0
  for a in "$@"; do
    case "$a" in
      --bad)        bad=1 ;;
      --slider|--linear|--rack) env="$LINEAR_PRIMARY_ENV" ;;
      --no-nodes)   do_nodes=0 ;;
      --nodes-only) do_primary=0 ;;
      *) host="$a" ;;
    esac
  done
  host="${host:-${DUSTGATE_HOST:-dustgate.local}}"
  local info key ota
  # RESOLVE ONCE, then talk to the address. The .local name goes through mDNS, which
  # on a busy network answers some lookups and times out on others; a wait loop that
  # re-resolves every poll can miss a board that is up and proven (2026-10-04).
  local ip
  ip="$(curl -sS --max-time 10 -o /dev/null -w '%{remote_ip}' "http://$host/api/info" 2>/dev/null || true)"
  [[ -n "$ip" && "$ip" != "0.0.0.0" ]] && host="$ip"
  info="$(curl -fsS --max-time 10 "http://$host/api/info" 2>/dev/null || true)"
  key="$(printf '%s' "$info" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("apiKey",""))' 2>/dev/null || true)"
  if [[ -z "$key" ]]; then
    echo "  ✗ $host did not answer /api/info. Is the primary on the network? Try its IP:"
    echo "      bash dev.sh ota 192.168.x.y"
    exit 1
  fi
  ota="$(printf '%s' "$info" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("ota","none"))' 2>/dev/null || echo none)"
  local was; was="$(printf '%s' "$info" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("build","?"), d.get("slot","?"))' 2>/dev/null)"
  echo "▶ OTA to $host — running: $was  (ota: $ota)"
  if [[ $do_primary == 1 ]]; then
    case "$ota" in
      nogo) echo "  ✗ This board's partition table has one app slot. Flash it ONCE by cable"
            echo "    (bash dev.sh flash) to install the OTA table; every update after that is this command."
            exit 1 ;;
      probation) echo "  ✗ The running image is still on probation (not yet marked valid). Wait a minute and retry."
                 exit 1 ;;
      none) echo "  ✗ Firmware older than the OTA feature — it cannot take this. Flash by cable once."
            exit 1 ;;
    esac
  fi

  # --bad: build images that NEVER report healthy, to prove rollback on hardware.
  # PlatformIO reads this from the environment, so the flag is on for these builds
  # only and the next ordinary run recompiles without it. After the test, run
  # `bash dev.sh ota` (no --bad) to put good images back — the brain will otherwise
  # keep offering the bad node image.
  if [[ $bad == 1 ]]; then
    export PLATFORMIO_BUILD_FLAGS="-DDUSTGATE_OTA_TEST_BAD"
    echo "⚠ --bad: these images will refuse to become healthy and must ROLL BACK by themselves (~3 min)."
  else
    unset PLATFORMIO_BUILD_FLAGS
  fi

  # Build everything first: a failure here must not leave the shop half-updated.
  local penv_bin nbins=()
  if [[ $do_primary == 1 ]]; then
    use_core_for_env "$env" >/dev/null
    echo "▶ Building the primary ($(describe_env "$env"))…"
    "$PIO" run -j 1 -e "$env" >/dev/null || { echo "  ✗ Build failed — run: pio run -e $env"; exit 1; }
    penv_bin="$SCRIPT_DIR/.pio.nosync/build/$env/firmware.bin"
    [[ -f "$penv_bin" ]] || { echo "  ✗ No $penv_bin"; exit 1; }
  fi
  if [[ $do_nodes == 1 ]]; then
    local ne
    for ne in "$NODE_ENV" "$LINEAR_NODE_ENV"; do
      use_core_for_env "$ne" >/dev/null
      echo "▶ Building the node image ($(describe_env "$ne"))…"
      "$PIO" run -j 1 -e "$ne" >/dev/null || { echo "  ✗ Build failed — run: pio run -e $ne"; exit 1; }
    done
  fi

  if [[ $do_primary == 1 ]]; then
    local md5 size; md5="$(md5 -q "$penv_bin" 2>/dev/null || md5sum "$penv_bin" | cut -d' ' -f1)"
    size="$(wc -c < "$penv_bin" | tr -d ' ')"
    echo "▶ Uploading the primary: $size bytes (md5 ${md5:0:8}…)"
    local reply
    reply="$(curl -sS --max-time 180 -X POST -H "X-Api-Key: $key" -H "X-Md5: $md5" -H 'Expect:' \
              -H 'Content-Type: application/octet-stream' --data-binary "@$penv_bin" "http://$host/api/ota" 2>&1)" || true
    if ! printf '%s' "$reply" | grep -q '"rebooting":true'; then
      echo "  ✗ The board refused it: $reply"
      exit 1
    fi
    echo "  Accepted — the board is rebooting into the new slot."
    echo "▶ Waiting for it to come back and prove itself (~30 s on WiFi)…"
    sleep 6
    local i now proven=0
    for i in $(seq 1 45); do
      now="$(curl -fsS --max-time 3 "http://$host/api/info" 2>/dev/null | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("build","?"), d.get("slot","?"), d.get("ota","?"))' 2>/dev/null || true)"
      if [[ -n "$now" ]]; then
        case "$now" in *valid) echo "  now running: $now"; echo "  ✓ Marked valid — the update is permanent."; proven=1; break ;; esac
      fi
      sleep 4
    done
    if [[ $proven == 0 ]]; then
      echo "  ⚠ It has not been marked valid — it may have rolled back, or is still on probation."
      echo "    Check:  curl http://$host/api/info    (slot/ota)   and   bash dev.sh log $host"
      exit 1
    fi
    # The key survives the reboot (NVS), but take it fresh in case it was regenerated.
    key="$(curl -fsS --max-time 5 "http://$host/api/info" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("apiKey",""))')"
  fi

  if [[ $do_nodes == 1 ]]; then
    local kind ne2 bin fw nmd5 nsize
    for kind in pwm linear; do
      [[ $kind == pwm ]] && ne2="$NODE_ENV" || ne2="$LINEAR_NODE_ENV"
      bin="$SCRIPT_DIR/.pio.nosync/build/$ne2/firmware.bin"
      fw="$(cat "$SCRIPT_DIR/.pio.nosync/build/$ne2/fw.stamp" 2>/dev/null || true)"
      [[ -f "$bin" && -n "$fw" ]] || { echo "  ✗ No node image / stamp for $ne2"; exit 1; }
      nmd5="$(md5 -q "$bin" 2>/dev/null || md5sum "$bin" | cut -d' ' -f1)"
      nsize="$(wc -c < "$bin" | tr -d ' ')"
      echo "▶ Staging the $kind node image on the primary ($nsize bytes, $fw)…"
      local r
      r="$(curl -sS --max-time 120 -X POST -H "X-Api-Key: $key" -H "X-Fw: $fw" -H "X-Md5: $nmd5" -H 'Expect:' \
            -H 'Content-Type: application/octet-stream' --data-binary "@$bin" "http://$host/api/node-image?kind=$kind" 2>&1)" || true
      printf '%s' "$r" | grep -q '"ok":true' || { echo "  ✗ The primary refused it: $r"; exit 1; }
    done
    echo "  ✓ Node images staged. Open the Boards screen — a board whose firmware differs"
    echo "    shows \"update available\". Tap it for one board at a time, with the shop quiet."
  fi
}

# resolve_host HOST — print HOST's IP if it answers, else HOST unchanged.
# A .local name goes through mDNS, which on a busy network answers some lookups and
# times out on others; every command that makes several requests should resolve ONCE
# and then use the address, or a flaky lookup reads as a dead board (2026-10-04).
resolve_host() {
  local h="$1" ip
  ip="$(curl -sS --max-time 10 -o /dev/null -w '%{remote_ip}' "http://$h/api/info" 2>/dev/null || true)"
  if [[ -n "$ip" && "$ip" != "0.0.0.0" ]]; then echo "$ip"; else echo "$h"; fi
}

# run_linklog [host]
# Pull the primary's link log over WiFi, save it, and summarise it — the way to
# watch a shop with no laptop on any board. See firmware/utils/LinkLog.h and
# tools/linklog-summary.py. The API key comes from the board's own /api/info.
run_linklog() {
  local host="${1:-${DUSTGATE_HOST:-dustgate.local}}"
  host="$(resolve_host "$host")"
  echo "▶ Link log from $host"
  local key
  key="$(curl -fsS --max-time 10 "http://$host/api/info" 2>/dev/null \
          | python3 -c 'import json,sys; print(json.load(sys.stdin).get("apiKey",""))' 2>/dev/null || true)"
  if [[ -z "$key" ]]; then
    echo "  ✗ $host did not answer /api/info. Is the primary on the network? Try its IP:"
    echo "      bash dev.sh linklog 192.168.x.y"
    exit 1
  fi
  local dir="$SCRIPT_DIR/.monitor-logs"; mkdir -p "$dir"
  local out="$dir/linklog-$(date +%Y%m%d-%H%M%S).jsonl"
  # The rotated file first, so the saved copy reads oldest to newest.
  curl -fsS --max-time 20 -H "X-Api-Key: $key" "http://$host/api/linklog?old=1" >  "$out" 2>/dev/null || true
  curl -fsS --max-time 20 -H "X-Api-Key: $key" "http://$host/api/linklog"       >> "$out" 2>/dev/null || true
  if [[ ! -s "$out" ]]; then
    rm -f "$out"
    echo "  The board has no link log yet (firmware older than 2026-09-27, or just flashed)."
    exit 0
  fi
  echo "  Saved → ${out#$SCRIPT_DIR/}"
  echo ""
  python3 "$SCRIPT_DIR/tools/linklog-summary.py" "$out"
}

# run_log [host] [--once]
# Follow the brain's serial output over WiFi (GET /api/serial, utils/SerialLog.h),
# saved to .monitor-logs/brainlog-*.log as it arrives. The same text the USB
# monitor shows, for a board in the shop or a test run without a cable. Needs
# primary firmware from 2026-10-03 or later. Key from /api/info, like linklog.
run_log() {
  local host="" once=""
  for a in "$@"; do
    case "$a" in
      --once) once="--once" ;;
      *)      host="$a" ;;
    esac
  done
  host="${host:-${DUSTGATE_HOST:-dustgate.local}}"
  host="$(resolve_host "$host")"
  local key
  key="$(curl -fsS --max-time 10 "http://$host/api/info" 2>/dev/null \
          | python3 -c 'import json,sys; print(json.load(sys.stdin).get("apiKey",""))' 2>/dev/null || true)"
  if [[ -z "$key" ]]; then
    echo "  ✗ $host did not answer /api/info. Is the primary on the network? Try its IP:"
    echo "      bash dev.sh log 192.168.x.y"
    exit 1
  fi
  local code
  code="$(curl -sS --max-time 15 -o /dev/null -w '%{http_code}' -H "X-Api-Key: $key" "http://$host/api/serial?from=0" 2>/dev/null || echo 000)"
  if [[ "$code" == "404" ]]; then
    echo "  ✗ $host has no /api/serial — its firmware is older than 2026-10-03. Reflash the primary."
    exit 1
  elif [[ "$code" != "200" ]]; then
    echo "  ✗ $host answered /api/info but /api/serial gave HTTP $code (000 = no answer in 15 s). Try again."
    exit 1
  fi
  local dir="$SCRIPT_DIR/.monitor-logs"; mkdir -p "$dir"
  local out="$dir/brainlog-$(date +%Y%m%d-%H%M%S).log"
  echo "▶ Brain log from $host over WiFi → ${out#$SCRIPT_DIR/}  (Ctrl+C to stop)"
  python3 "$SCRIPT_DIR/tools/brain-log.py" "$host" "$key" "$out" $once
}

# run_monitor_both [--take]
# The primary and the node interleaved on one screen with one clock, and logged
# to .monitor-logs/ — see tools/monitor-both.py for why order is the point.
run_monitor_both() {
  local take=false
  [[ "${1:-}" == "--take" ]] && take=true
  echo "▶ Serial monitor — primary AND node (Ctrl+C to exit)."
  local p_port n_port
  p_port="$(require_port primary)" || exit 1
  n_port="$(require_port node)"    || exit 1
  if [[ "$p_port" == "$n_port" ]]; then
    # Both roles resolved to one device: at most one role is pinned and only
    # one board is attached. Watching it twice would only hide that.
    echo "  ✗ Primary and node both resolve to $p_port — only one of them is attached,"
    echo "    or they are not pinned. See:  bash dev.sh ports"
    exit 1
  fi
  echo "  Primary: $p_port  ($(describe_port "$p_port"))"
  echo "  Node:    $n_port  ($(describe_port "$n_port"))"
  port_is_free "$p_port" "$take" || exit 1
  port_is_free "$n_port" "$take" || exit 1

  # PlatformIO's own interpreter: it already carries pyserial, which a stock
  # macOS python3 does not.
  local py; py="$(dirname "$(command -v "$PIO")")/python"
  [[ -x "$py" ]] || py="python3"
  local log="$SCRIPT_DIR/.monitor-logs/$(date +%Y%m%d-%H%M%S).log"
  "$py" "$SCRIPT_DIR/tools/monitor-both.py" "$p_port" "$n_port" "$log"
}

# run_monitor [--scan-boot] [--take] [--port P] [env]
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
  local scan_boot=false env="$PRIMARY_ENV" explicit_port="" take=false
  local a
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --scan-boot) scan_boot=true; shift ;;
      # Stop whatever already holds this board's port, then open it.
      --take)      take=true; shift ;;
      # AN EXPLICIT PORT, because roles only go two deep and a shop does not.
      # Roles are pinned one per name (primary, node), so a bench with TWO nodes
      # has no way to say WHICH node — and watching the primary and a node at the
      # same time is exactly what diagnosing a link needs. `dev.sh ports` lists
      # what is attached with serials; paste one here.
      --port)      explicit_port="$2"; shift 2 ;;
      --port=*)    explicit_port="${1#--port=}"; shift ;;
      *)           env="$1"; shift ;;
    esac
  done

  # Which physical board, from which env: the two are pinned to roles, and
  # getting it wrong is silent — the monitor opens the other board's port and
  # shows nothing.
  local role=primary
  [[ "$env" == "$NODE_ENV" || "$env" == "$LINEAR_NODE_ENV" ]] && role=node

  local port
  if [[ -n "$explicit_port" ]]; then
    if [[ ! -e "$explicit_port" ]]; then
      echo "  ✗ No such port: $explicit_port"
      echo "    Attached boards:"; run_ports | sed -n '2,20p'
      exit 1
    fi
    port="$explicit_port"
  else
    port="$(require_port "$role")" || exit 1
  fi
  local what; what="$(describe_port "$port")"
  echo "  Using port: $port${what:+  ($what)}"

  # A monitor left running from an earlier session holds the port open, and the
  # second one fails in ways that look like a board problem rather than a
  # bookkeeping one. Worse, an old monitor may be sitting on the OTHER board —
  # the mismatch this whole port-pinning exercise exists to prevent, and easy to
  # hit when the two boards look identical.
  #
  # ASK THE PORT, NOT THE PROCESS LIST. This used to warn about ANY running
  # `device monitor` — so watching the primary and a node at once, which is
  # exactly what a link needs, printed a scary warning for a monitor on the
  # OTHER board, and then carried on into pio's "Could not exclusively lock
  # port" when the port really was taken. lsof names the one process that
  # actually holds THIS device, whatever it is (a stray pio, screen, an IDE).
  if ! port_is_free "$port" "$take"; then exit 1; fi
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
  # Show what a flash would provision WITH, because the commonest surprise is a
  # board that comes up on last month's network. load_env_defaults is cheap and
  # read-only.
  load_env_defaults
  if [[ -n "$ENV_SSID" ]]; then
    echo "  WiFi: '$ENV_SSID'   hostname: '${ENV_HOST:-dustgate}'   (w = change)"
  else
    echo "  WiFi: not set yet — a flash will ask."
  fi
  echo ""
  echo "  4) Flash a PRIMARY      — UI + firmware + filesystem + provision"
  echo "     4f = firmware only     4u = UI/filesystem only"
  echo "     4s = the SLIDER primary (ST3215 rack instead of PWM valves)"
  echo "     4c = the same primary, with the collector wiring explained"
  echo "  5) Flash a NODE         — node firmware + WiFi creds"
  echo "     5s = a SLIDER node (one rack, homes itself at boot)"
  echo "     5c = the same node — there is no collector build any more"
  echo ""
  echo "  w) Set the WiFi credentials and hostname used by every flash above"
  echo ""
  echo "  6) Monitor the PRIMARY      (6n = monitor a NODE instead)"
  echo "     bench commands, once connected:"
  echo "       press   fire the RF transmitter once (lamp in the outlet, not the blower)"
  echo "       rfscan  find the fob's address by trying the 4 ways a DIP gets misread"
  echo "       ct [n]  read the clamp n times — serial only, see the note in the header"
  echo "       stroke <1-3> <from> <to> [reps]   press a switch repeatably, then detach"
  echo "  7) Ports — list attached boards, and pin one to a role"
  echo "  8) (Re)send WiFi/key/hostname to an already-flashed board"
  echo "     8n = ...to the board pinned as the NODE"
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
    4c|4C) run_flash --collector ;;
    5) run_flash_node ;;
    5s|5S) run_flash_node --slider ;;
    5c|5C) run_flash_node --collector ;;
    # Prompt for SSID/password/hostname and SAVE them, then come back to the
    # menu. Separate from a flash on purpose: changing the network is a thing
    # you do once, and making every flash ask is how people stop reading prompts.
    w|W) OV_HOST=""; OV_SSID=""; OV_PASS=""; OV_ASK=1; OV_SAVE=1
         apply_provision_overrides
         echo "  Saved to tools/.env — every flash uses these until you change them."
         show_menu ;;
    6) run_monitor ;;
    6n|6N) run_monitor "$NODE_ENV" ;;
    7) run_ports ;;
    8) run_provision ;;
    8n|8N) run_provision node ;;
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
  linklog)   shift; run_linklog "$@" ;;
  ota)       shift; run_ota "$@" ;;
  log)       shift; run_log "$@" ;;
  monitor)
    shift || true
    case "${1:-}" in
      # FORWARD THE REST. Both arms used to stop at the role word and drop
      # everything after it, so `monitor node --port /dev/cu.X` silently lost the
      # port and opened whichever board the role was pinned to — which is the
      # failure the flag exists to work around.
      node|n)     shift; run_monitor "$NODE_ENV" "$@" ;;
      both|b)     shift; run_monitor_both "$@" ;;
      slider|linear) shift; run_monitor "$LINEAR_NODE_ENV" "$@" ;;
      *)          run_monitor "$@" ;;
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
    echo "              |monitor [node|both] [--port /dev/cu.X] [--take]|ports [--pin primary|node]|erase|provision [--host N] [--ssid N]]"
    exit 1
    ;;
esac

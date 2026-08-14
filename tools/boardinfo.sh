#!/usr/bin/env bash
# boardinfo.sh — answer questions about a PlatformIO env by following the same
# chain the compiler does. Sourced by dev.sh and deploy.sh.
#
# WHY THIS EXISTS: both scripts used to carry a hardcoded list of "which envs
# have native USB" —
#
#     case "$PIO_ENV" in dustgate_node*|adafruit_feather_esp32s2) NATIVE_USB=1 ;;
#
# — and that list is a copy of a fact the board headers already state. Adding
# the XIAO C5 env didn't update the copy, so the scripts would have driven a
# native-USB board the bridge-chip way. That failure is SILENT: with DTR
# deasserted, TinyUSB never reports the port connected and USBCDC::write()
# discards every byte, so the board boots, joins WiFi and prints nothing —
# indistinguishable from a dead board (see the long note in deploy.sh).
#
# So derive it instead, along the real chain:
#
#     [env:NAME] build_flags  ->  -DBOARD_X
#     config.h                ->  #if defined(BOARD_X) -> boards/y.h
#     boards/y.h              ->  #define BOARD_HAS_NATIVE_USB 0|1
#
# A new board that declares the macro is handled with no script change at all.

BOARDINFO_ROOT="${BOARDINFO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# The env PlatformIO would build with no -e flag.
pio_default_env() {
  awk -F= '/^[[:space:]]*default_envs[[:space:]]*=/ { gsub(/[[:space:]]/,"",$2); print $2; exit }' \
    "$BOARDINFO_ROOT/platformio.ini"
}

# -DBOARD_* for an env. Handles both the one-line and the indented-continuation
# spellings of build_flags, because platformio.ini uses both.
env_board_macro() {
  local env="${1:-}"
  [[ -z "$env" ]] && env="$(pio_default_env)"
  awk -v want="[env:$env]" '
    $0 == want            { inenv = 1; next }
    /^\[/                 { inenv = 0 }
    inenv                 { print }
  ' "$BOARDINFO_ROOT/platformio.ini" | grep -oE -- '-DBOARD_[A-Z0-9_]+' | head -1 | sed 's/^-D//'
}

# BOARD_X -> the boards/*.h config.h includes for it.
board_header_for_macro() {
  local macro="${1:-}"
  [[ -z "$macro" ]] && return 1
  awk -v m="$macro" '
    $0 ~ ("defined\\(" m "\\)") { hit = 1; next }
    hit && /#include "boards\// {
      match($0, /boards\/[a-z0-9_]+\.h/); print substr($0, RSTART, RLENGTH); exit
    }
  ' "$BOARDINFO_ROOT/firmware/config.h"
}

# True when this env's board talks USB straight from the MCU (no bridge chip).
# Decides how DTR/RTS must be driven, and which serial port to prefer when a
# primary and a node share the bench.
board_has_native_usb() {
  local env="${1:-}" macro header
  macro="$(env_board_macro "$env")"                 || return 1
  header="$(board_header_for_macro "$macro")"       || return 1
  [[ -n "$header" ]]                                || return 1
  grep -qE '^#define[[:space:]]+BOARD_HAS_NATIVE_USB[[:space:]]+1' \
    "$BOARDINFO_ROOT/firmware/$header"
}

# Human-readable, for logs: "xiao_c5 (BOARD_XIAO_C5, boards/xiao_c5.h, native USB)"
describe_env() {
  local env="${1:-}" macro header usb
  [[ -z "$env" ]] && env="$(pio_default_env)"
  macro="$(env_board_macro "$env")"
  header="$(board_header_for_macro "$macro")"
  if board_has_native_usb "$env"; then usb="native USB"; else usb="USB-serial bridge"; fi
  echo "$env (${macro:-no -DBOARD_*}, ${header:-unknown header}, $usb)"
}

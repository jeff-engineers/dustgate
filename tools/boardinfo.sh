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

# -----------------------------------------------------------------------------
# Arduino-core swapping.
#
# The xiao_c5 env overrides `platform` with the pioarduino fork; every other env
# uses official espressif32. BOTH publish a package named
# `framework-arduinoespressif32` into ONE shared directory
# (~/.platformio/packages), so only one core can be installed at a time.
#
# The comment in platformio.ini said the loser "reinstalls its own core in ~1
# min". It does not. PlatformIO leaves the other platform's package sitting in
# the directory, decides it doesn't satisfy the spec, and hands the builder a
# None path — which surfaces four frames deep in SCons as:
#
#     TypeError: expected str, bytes or os.PathLike object, not NoneType
#
# naming no package and reading like a PlatformIO bug. The build never recovers
# on its own; you have to clear the directory by hand.
#
# So do that automatically, and keep the evicted core instead of deleting it —
# swapping back is a directory rename rather than another download.

BOARDINFO_PKG_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages"
BOARDINFO_CORE_PKG="framework-arduinoespressif32"

# The platform spec an env actually builds with: its own override if it has one,
# otherwise the [env] default that every other env inherits.
env_platform() {
  local env="${1:-}" spec
  [[ -z "$env" ]] && env="$(pio_default_env)"
  spec="$(awk -v want="[env:$env]" '
    $0 == want { inenv = 1; next }
    /^\[/      { inenv = 0 }
    inenv && /^[[:space:]]*platform[[:space:]]*=/ {
      sub(/^[^=]*=[[:space:]]*/, ""); print; exit
    }
  ' "$BOARDINFO_ROOT/platformio.ini")"
  if [[ -z "$spec" ]]; then
    spec="$(awk '
      $0 == "[env]" { inenv = 1; next }
      /^\[/         { inenv = 0 }
      inenv && /^[[:space:]]*platform[[:space:]]*=/ {
        sub(/^[^=]*=[[:space:]]*/, ""); print; exit
      }
    ' "$BOARDINFO_ROOT/platformio.ini")"
  fi
  echo "$spec"
}

# Which family of core a platform spec pulls. A URL/zip spec is the pioarduino
# fork (that is the only way to install it); a registry spec is official.
platform_core_class() {
  case "${1:-}" in
    http*|*.zip) echo fork ;;
    *)           echo official ;;
  esac
}

# Same question about the core currently sitting in the packages directory.
# Empty = nothing installed, so nothing to evict.
installed_core_class() {
  local piopm="$BOARDINFO_PKG_DIR/$BOARDINFO_CORE_PKG/.piopm"
  [[ -f "$piopm" ]] || return 0
  python3 - "$piopm" <<'PY'
import json, sys
try:
    spec = json.load(open(sys.argv[1])).get("spec") or {}
except Exception:
    sys.exit(0)
# The registry copy is owned by "platformio" with no uri; the fork's is a
# tarball URL owned by "espressif".
print("official" if spec.get("owner") == "platformio" and not spec.get("uri") else "fork")
PY
}

# Make the installed core match what this env's platform wants. Call before any
# build. No-op in the common case (one platform, nothing to do).
ensure_core_for_env() {
  local env="${1:-}" want have pkg stash_have stash_want
  want="$(platform_core_class "$(env_platform "$env")")"
  have="$(installed_core_class)"
  [[ -z "$have" || "$have" == "$want" ]] && return 0

  pkg="$BOARDINFO_PKG_DIR/$BOARDINFO_CORE_PKG"
  stash_have="$BOARDINFO_PKG_DIR/.dustgate-core-$have"
  stash_want="$BOARDINFO_PKG_DIR/.dustgate-core-$want"

  echo "  Arduino core installed is '$have'; ${env:-this env} needs '$want' — swapping."
  rm -rf "$stash_have"
  mv "$pkg" "$stash_have"
  if [[ -d "$stash_want" ]]; then
    mv "$stash_want" "$pkg"
    echo "  (restored a previously stashed '$want' core — no download)"
  else
    echo "  (no stashed '$want' core yet — PlatformIO will download it once)"
  fi
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

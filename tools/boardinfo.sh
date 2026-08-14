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
# Platform isolation.
#
# The xiao_c5 env overrides `platform` with the pioarduino fork; every other env
# uses official espressif32. BOTH publish packages under the SAME names —
# `framework-arduinoespressif32` and `toolchain-riscv32-esp` — so sharing one
# PlatformIO core directory means they overwrite each other. Two distinct
# failures came out of that, neither of which PlatformIO detects or repairs:
#
#   1. The Arduino core. The loser's package is left in place, judged not to
#      satisfy the spec, and the builder is handed a None path — surfacing four
#      frames deep in SCons as
#        TypeError: expected str, bytes or os.PathLike object, not NoneType
#      naming no package and reading like a PlatformIO bug.
#
#   2. The riscv toolchain, worse. The official platform half-removes it: the
#      sysroot is deleted but .piopm still claims 14.2.0, so `pio pkg install`
#      says "Already up-to-date" while every compile fails with
#      `riscv32-esp-elf-g++: command not found` or
#      `fatal error: stdint.h: No such file or directory`. Recovery is deleting
#      the package by hand; the fix is ~2.3 GB and ~40 minutes.
#
# An earlier version of this file swapped the core package in and out around
# each build. That fixed (1) and not (2), which is the shape of the whole
# problem: the shared directory has an unknown number of collisions in it, and
# each one is found the same expensive way.
#
# So don't share it. A fork platform gets its OWN PLATFORMIO_CORE_DIR, and the
# two installations never see each other. Costs a second copy of the toolchains
# (7.6 GB measured, downloaded once); buys a build that cannot be broken by which env you
# built last.
# -----------------------------------------------------------------------------

# Whatever pio would have used if we said nothing — captured at source time so a
# PLATFORMIO_CORE_DIR the user set for their own reasons still wins for every
# non-fork env.
BOARDINFO_DEFAULT_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
# Override if ~ is small; measured at 7.6 GB.
BOARDINFO_FORK_CORE_DIR="${DUSTGATE_FORK_CORE_DIR:-$HOME/.platformio-pioarduino}"

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

# Which core directory an env belongs in.
core_dir_for_env() {
  if [[ "$(platform_core_class "$(env_platform "${1:-}")")" == "fork" ]]; then
    echo "$BOARDINFO_FORK_CORE_DIR"
  else
    echo "$BOARDINFO_DEFAULT_CORE_DIR"
  fi
}

# Point THIS SHELL's pio at the right core directory. Call before any build.
#
# Exported rather than passed per-command because a build shells out to pio more
# than once (run, uploadfs, device monitor) and every one of them has to agree —
# a single call landing in the wrong directory is how the collision came back.
#
# Running pio by hand needs the same thing:
#   PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e xiao_c5
use_core_for_env() {
  local env="${1:-}" dir
  dir="$(core_dir_for_env "$env")"
  export PLATFORMIO_CORE_DIR="$dir"
  [[ "$dir" == "$BOARDINFO_DEFAULT_CORE_DIR" ]] && return 0

  echo "  Platform: pioarduino fork — using its own core dir, $dir"
  if [[ ! -d "$dir/packages" ]]; then
    echo "  First build against it: PlatformIO downloads the platform and the"
    echo "  riscv toolchain into that directory. 7.6 GB, and the download is most"
    echo "  of the wall time — check you have the room before starting."
    echo "  Nothing in $BOARDINFO_DEFAULT_CORE_DIR is touched."
  fi
  return 0
}

# WHICH kind of USB — and therefore what DTR/RTS mean. Three cases, and the two
# native ones want OPPOSITE handling, so "native or not" is not enough:
#
#   bridge   CP2102/CH340 (DevKitC): DTR->GPIO0, RTS->EN is an auto-reset
#            circuit. Asserting either reboots the board. Hold LOW.
#   tinyusb  TinyUSB CDC (QT Py S3, Feather S2): pure CDC line state, but
#            USBCDC::write() DISCARDS output unless DTR is asserted. Hold HIGH.
#   jtag     USB Serial/JTAG (XIAO C5, QT Py C3): DTR+RTS is the ROM's
#            download-mode trigger. Assert both and the chip leaves the app and
#            enumerates as the bootloader — port gone, monitor exits, no output,
#            buttons dead. Hold LOW.
#
# Getting this wrong produces a board that looks broken rather than misconfigured
# in all three cases, which is why it is derived from the header rather than
# remembered.
board_usb_kind() {
  local env="${1:-}" macro header
  macro="$(env_board_macro "$env")"
  header="$(board_header_for_macro "$macro")"
  if [[ -n "$header" ]] && grep -qE '^#define[[:space:]]+BOARD_USB_SERIAL_JTAG[[:space:]]+1' \
       "$BOARDINFO_ROOT/firmware/$header"; then
    echo jtag
  elif board_has_native_usb "$env"; then
    echo tinyusb
  else
    echo bridge
  fi
}

# Human-readable, for logs: "xiao_c5 (BOARD_XIAO_C5, boards/xiao_c5.h, USB Serial/JTAG)"
describe_env() {
  local env="${1:-}" macro header usb
  [[ -z "$env" ]] && env="$(pio_default_env)"
  macro="$(env_board_macro "$env")"
  header="$(board_header_for_macro "$macro")"
  case "$(board_usb_kind "$env")" in
    jtag)    usb="USB Serial/JTAG, hold DTR/RTS low" ;;
    tinyusb) usb="native USB (TinyUSB CDC), DTR asserted" ;;
    *)       usb="USB-serial bridge" ;;
  esac
  echo "$env (${macro:-no -DBOARD_*}, ${header:-unknown header}, $usb)"
}

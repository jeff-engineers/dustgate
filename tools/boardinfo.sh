#!/usr/bin/env bash
# boardinfo.sh — answer questions about a PlatformIO env by following the same
# chain the compiler does. Sourced by dev.sh and deploy.sh.
#
# WHY DERIVED, not hardcoded: a script-side list of "which envs have native USB"
# is a copy of a fact the board headers already state, and when the copy goes
# stale the failure is SILENT — the board boots, joins WiFi and prints nothing.
# So follow the real chain:
#
#     [env:NAME] build_flags  ->  -DBOARD_X
#     config.h                ->  #if defined(BOARD_X) -> boards/y.h
#     boards/y.h              ->  #define BOARD_HAS_NATIVE_USB 0|1
#
# One board today, so every answer is the same answer. The ST3215 slider node is
# the next -DBOARD_*, and should need no change here.

BOARDINFO_ROOT="${BOARDINFO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# The env PlatformIO would build with no -e flag.
pio_default_env() {
  awk -F= '/^[[:space:]]*default_envs[[:space:]]*=/ { gsub(/[[:space:]]/,"",$2); print $2; exit }' \
    "$BOARDINFO_ROOT/platformio.ini"
}

# The raw text of one [env:NAME] section.
env_section() {
  awk -v want="[env:${1:-}]" '
    $0 == want            { inenv = 1; next }
    /^\[/                 { inenv = 0 }
    inenv                 { print }
  ' "$BOARDINFO_ROOT/platformio.ini"
}

# -DBOARD_* for an env. Handles both the one-line and the indented-continuation
# spellings of build_flags, because platformio.ini uses both.
env_board_macro() {
  local env="${1:-}"
  [[ -z "$env" ]] && env="$(pio_default_env)"
  env_section "$env" | grep -oE -- '-DBOARD_[A-Z0-9_]+' | head -1 | sed 's/^-D//'
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

# WHICH kind of USB — and therefore what DTR/RTS mean. Three cases, and the two
# native ones want OPPOSITE handling, so "native or not" is not enough:
#
#   bridge   CP2102/CH340: DTR->GPIO0, RTS->EN is an auto-reset circuit.
#            Asserting either reboots the board. Hold LOW.
#   tinyusb  TinyUSB CDC: pure CDC line state, but USBCDC::write() DISCARDS
#            output unless DTR is asserted. Hold HIGH.
#   jtag     USB Serial/JTAG (the XIAO C5): DTR+RTS is the ROM's download-mode
#            trigger. Assert both and the chip leaves the app and enumerates as
#            the bootloader — port gone, monitor exits, no output, buttons dead.
#            Hold LOW.
#
# Only `jtag` is reachable today. Kept because getting this wrong produces a
# board that looks broken rather than misconfigured, in all three cases.
board_usb_kind() {
  local env="${1:-}" header
  header="$(board_header_for_macro "$(env_board_macro "$env")")"
  [[ -n "$header" ]] || { echo bridge; return; }
  if grep -qE '^#define[[:space:]]+BOARD_USB_SERIAL_JTAG[[:space:]]+1' "$BOARDINFO_ROOT/firmware/$header"; then
    echo jtag
  elif grep -qE '^#define[[:space:]]+BOARD_HAS_NATIVE_USB[[:space:]]+1' "$BOARDINFO_ROOT/firmware/$header"; then
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

# -----------------------------------------------------------------------------
# The core directory. Every env rides the pioarduino fork (official `espressif32`
# has no ESP32-C5) and builds against its own PLATFORMIO_CORE_DIR, which is where
# its ~7.6 GB lives. ~/.platformio holds an unused official installation.
#
# Exported rather than passed per-command because a build shells out to pio more
# than once (run, uploadfs, device monitor) and every one has to agree. By hand:
#   PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e xiao_c5_primary
# -----------------------------------------------------------------------------

# Override if ~ is small; measured at 7.6 GB.
BOARDINFO_FORK_CORE_DIR="${DUSTGATE_FORK_CORE_DIR:-$HOME/.platformio-pioarduino}"

# Point THIS SHELL's pio at it. Call before any build. Takes an env name for
# call-site symmetry and ignores it — there is one platform.
use_core_for_env() {
  export PLATFORMIO_CORE_DIR="$BOARDINFO_FORK_CORE_DIR"
  if [[ ! -d "$BOARDINFO_FORK_CORE_DIR/packages" ]]; then
    echo "  First build against the pioarduino fork: PlatformIO downloads the"
    echo "  platform and the riscv toolchain into $BOARDINFO_FORK_CORE_DIR."
    echo "  7.6 GB, and the download is most of the wall time — check the room."
  fi
  return 0
}

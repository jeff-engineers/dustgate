// =============================================================================
// utils/SerialLog.h — the brain's serial output, kept in memory so it can be read
// over WiFi (GET /api/serial, `bash dev.sh log`, the app's Brain log screen).
//
// WHY: a board in the shop has no laptop on its USB port, and a bench test is
// easier to follow when whoever is debugging can read the brain without a cable.
// The link log (utils/LinkLog.h) keeps a few kinds of event across reboots; this
// keeps EVERYTHING the board prints, for as long as 32 KB lasts, and is gone on
// a restart.
//
// HOW IT CATCHES EVERYTHING. Two sources, because there are two ways out:
//
//   1. Our own Serial.print / printf / DEBUG_PRINT — about a thousand call sites.
//      Rather than touch them, the primary builds are force-included with
//      utils/SerialCapture.h, which redefines `Serial` (already a macro for
//      HWCDCSerial on this core) to a tee that writes both to USB and here.
//   2. Library and core errors — log_e(), "[E][NetworkClient.cpp:435] ...".
//      Those go through esp_rom_printf to the ROM's output channels, not through
//      Serial, so a tee alone misses exactly the lines most worth reading. ROM
//      channel 1 is UART0's console — no pins we use, nothing listening on a
//      XIAO — so begin() points it here instead. Channel 2, the USB console, is
//      left alone and still prints to the monitor.
//
// Ring buffer in PSRAM: the primary's INTERNAL heap is the tight one (~65 KB
// free, ~35 KB largest block with a node linked), and PSRAM has 8 MB doing
// nothing. Readers address bytes by a running count since boot, so a poller asks
// "everything after byte N" and learns if it fell behind (the buffer wrapped).
//
// UNVERIFIED ON HARDWARE (2026-10-03): the ROM-channel hook. Compiles; that the
// C5's esp_rom_printf calls channel 1 alongside channel 2 is from the IDF header's
// own description, not from a board.
// =============================================================================
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace seriallog {

// Allocate the buffer and hook the ROM output. Call FIRST in setup(), before
// Serial.begin(); anything printed before it is forwarded to USB but not kept.
void begin();

// Append bytes. Safe from any task; a no-op until begin().
void write(const uint8_t* data, size_t len);

// Bytes written since boot (including any the buffer has since dropped).
uint32_t total();

// Random per boot, so a reader can tell "the board restarted" from "the board
// went quiet" — a restart resets total() to 0.
uint32_t bootId();

// Copy out up to `cap` bytes starting at byte `from`. If `from` has already been
// overwritten, copying starts at the oldest byte still held; `*start` says where
// the copy actually began (start > from means bytes were missed). `*next` is the
// value to pass as `from` next time. Returns the number of bytes copied.
size_t read(uint32_t from, char* out, size_t cap, uint32_t* start, uint32_t* next);

} // namespace seriallog

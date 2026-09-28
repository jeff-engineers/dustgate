// =============================================================================
// utils/LinkLog.h — the primary's own record of every link event, kept on flash.
//
// WHY THIS EXISTS (2026-09-27). The link between the primary and its nodes was
// debugged all evening from serial logs — which only exist while a laptop is
// plugged into the board, and are gone the moment it is not. The shop is about to
// run for days with no laptop on any board, and the one question that decides
// what to do next (ESP-NOW or not) is HOW OFTEN the guest network cuts a node off
// and HOW LONG it takes to come back. Nobody can answer that from memory; this
// file answers it from the board.
//
// WHAT IT RECORDS, one JSON object per line (JSON Lines), newest last:
//
//   {"ts":1790000000,"up":123456,"boot":7,"ev":"link_down","node":"dustgate-planer"}
//
//   ts    Unix seconds, or 0 before NTP has answered (then read `boot` + `up`)
//   up    millis() on this boot
//   boot  this primary's boot counter (NVS), so lines from different boots
//         never interleave ambiguously in a log with no wall clock
//   ev    boot | wifi_up | wifi_down | link_up | link_down | refused | rejoin | hourly
//
// plus event-specific fields (see the call sites). Deliberately NOT here: SET,
// SENSE, anything per-move. This is a log of the LINK, a handful of lines a day,
// so flash wear and file size stay irrelevant.
//
// THREADING. Events come from three tasks: the main loop, each NodeLink task,
// and the WiFi event task. event() only formats into a small in-RAM queue under
// a spinlock — never touches the filesystem — and the main loop's flush() is the
// one writer. So a link task can log from inside a socket callback and the flash
// is only ever written from one place.
//
// ROTATION. /linklog.txt rolls to /linklog.1.txt at kRotateBytes, dropping the
// previous .1: two files, at most ~2 × 48 KB, which at a few hundred bytes a day
// plus 24 hourly lines is weeks of history.
//
// ⚠️ A FILESYSTEM FLASH ERASES IT, along with the UI and the topology. Pull it
// first: `bash dev.sh linklog` (deploy.sh does so on its own before a UI flash).
// =============================================================================
#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>
#include "ResetReason.h"

namespace linklog {

static const char*  kPath        = "/linklog.txt";
static const char*  kPathOld     = "/linklog.1.txt";
static const size_t kRotateBytes = 48 * 1024;
static const size_t kLineMax     = 240;
static const size_t kQueueLen    = 24;   // lines between flushes; flush runs every loop

struct State {
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    char     q[kQueueLen][kLineMax];
    uint8_t  head = 0, count = 0;
    uint32_t dropped = 0;        // lines lost to a full queue — logged when it drains
    uint32_t boot = 0;
    bool     ready = false;      // LittleFS mounted and begin() done
};
inline State& S() { static State s; return s; }

inline uint32_t bootNo() { return S().boot; }

// Unix seconds once NTP has answered, else 0. 1.7e9 = late 2023: anything before
// it is the chip's default epoch, not a real time.
inline uint32_t unixNow() {
    const time_t t = time(nullptr);
    return (t > 1700000000) ? (uint32_t)t : 0;
}

// Record one event. `extra` is zero or more JSON members, already formatted and
// comma-separated WITHOUT surrounding braces — e.g. "\"downMs\":95000". Safe from
// any task; never blocks on the filesystem.
inline void event(const char* ev, const char* node = nullptr, const char* extra = nullptr) {
    char line[kLineMax];
    int n = snprintf(line, sizeof(line), "{\"ts\":%lu,\"up\":%lu,\"boot\":%lu,\"ev\":\"%s\"",
                     (unsigned long)unixNow(), (unsigned long)millis(),
                     (unsigned long)S().boot, ev ? ev : "?");
    if (node && *node && n > 0 && (size_t)n < sizeof(line))
        n += snprintf(line + n, sizeof(line) - n, ",\"node\":\"%s\"", node);
    if (extra && *extra && n > 0 && (size_t)n < sizeof(line))
        n += snprintf(line + n, sizeof(line) - n, ",%s", extra);
    if (n > 0 && (size_t)n < sizeof(line) - 1) { line[n] = '}'; line[n + 1] = '\0'; }
    else { line[sizeof(line) - 2] = '}'; line[sizeof(line) - 1] = '\0'; }   // truncated, still one line

    State& s = S();
    portENTER_CRITICAL(&s.mux);
    if (s.count < kQueueLen) {
        const uint8_t slot = (s.head + s.count) % kQueueLen;
        memcpy(s.q[slot], line, sizeof(line));
        s.count++;
    } else {
        s.dropped++;
    }
    portEXIT_CRITICAL(&s.mux);
    // Echoed to serial too, so a bench session sees exactly what the shop log gets.
    Serial.print(F("[LINKLOG] ")); Serial.println(line);
}

inline void rotateIfBig() {
    File f = LittleFS.open(kPath, "r");
    if (!f) return;
    const size_t sz = f.size();
    f.close();
    if (sz < kRotateBytes) return;
    LittleFS.remove(kPathOld);
    LittleFS.rename(kPath, kPathOld);
}

// Main loop only. Writes whatever is queued; cheap when nothing is.
inline void flush() {
    State& s = S();
    if (!s.ready || s.count == 0) return;
    // Take the lines out under the lock, write them outside it. STATIC: ~5.7 KB
    // would not fit on the main loop's stack (≈13 KB free after setup), and only
    // the main loop ever calls this.
    static char batch[kQueueLen][kLineMax];
    uint8_t n = 0;
    uint32_t dropped = 0;
    portENTER_CRITICAL(&s.mux);
    while (s.count && n < kQueueLen) {
        memcpy(batch[n++], s.q[s.head], kLineMax);
        s.head = (s.head + 1) % kQueueLen;
        s.count--;
    }
    dropped = s.dropped; s.dropped = 0;
    portEXIT_CRITICAL(&s.mux);

    rotateIfBig();
    File f = LittleFS.open(kPath, "a");
    if (!f) return;   // lines are lost, but the board carries on — a log is not worth a fault
    for (uint8_t i = 0; i < n; i++) f.println(batch[i]);
    if (dropped) {
        char note[96];
        snprintf(note, sizeof(note), "{\"ts\":%lu,\"up\":%lu,\"boot\":%lu,\"ev\":\"dropped\",\"lines\":%lu}",
                 (unsigned long)unixNow(), (unsigned long)millis(), (unsigned long)s.boot,
                 (unsigned long)dropped);
        f.println(note);
    }
    f.close();
}

// Call once LittleFS is mounted. Bumps the boot counter and records WHY this
// boot happened — "panic"/"task_wdt" here is a crash worth chasing; "usb" is a
// flash or a monitor on the bench.
inline void begin() {
    Preferences p;
    p.begin("linklog", false);
    S().boot = p.getUInt("boots", 0) + 1;
    p.putUInt("boots", S().boot);
    p.end();
    S().ready = true;
    char extra[64];
    snprintf(extra, sizeof(extra), "\"rst\":\"%s\"", resetreason::now());
    event("boot", nullptr, extra);
}

// A JSON-safe copy of a free-text value (hostnames are safe; this is for the
// odd reason string). Drops quotes and backslashes rather than escaping them.
inline void safeCopy(char* dst, size_t n, const char* src) {
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 1 < n; i++)
        if (src[i] != '"' && src[i] != '\\' && (unsigned char)src[i] >= 0x20) dst[j++] = src[i];
    dst[j] = '\0';
}

} // namespace linklog

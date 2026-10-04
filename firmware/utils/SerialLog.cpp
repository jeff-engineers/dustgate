// utils/SerialLog.cpp — see SerialLog.h. Also defines the Serial tee that
// utils/SerialCapture.h swaps in for `Serial`.
#include <Arduino.h>
#include <esp_rom_sys.h>
#include <sys/time.h>
#include <time.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <esp_attr.h>
#include <esp_system.h>
#include "SerialLog.h"
#include "SerialCapture.h"

namespace seriallog {
namespace {

constexpr size_t kCap = 32 * 1024;

char*        s_buf   = nullptr;
size_t       s_cap   = 0;
uint32_t     s_total = 0;
uint32_t     s_boot  = 0;
portMUX_TYPE s_mux   = portMUX_INITIALIZER_UNLOCKED;

// ── ROM output (log_e and friends) ───────────────────────────────────────────
// esp_rom_printf calls this one character at a time, from wherever the print
// happened — a task, an ISR, or code running while flash is being written and
// the cache is OFF. In that last state any instruction or data fetched from
// flash or PSRAM crashes the chip on the spot. So this runs from IRAM and only
// touches a small buffer in internal RAM; the PSRAM ring is filled from it later,
// by the next ordinary write, from a task where that is always safe.
//
// It used to write straight into the PSRAM ring from flash-resident code, and
// the first build to carry it panicked once on a bench (2026-10-03, ~107 s into
// a boot, cause not captured). That is exactly the shape of crash this rules out.
constexpr size_t kStageCap = 1024;
DRAM_ATTR char         s_stage[kStageCap];
DRAM_ATTR size_t       s_stageLen = 0;
DRAM_ATTR uint32_t     s_stageLost = 0;
DRAM_ATTR portMUX_TYPE s_stageMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR romPutc(char c) {
    portENTER_CRITICAL_SAFE(&s_stageMux);
    if (s_stageLen < kStageCap) s_stage[s_stageLen++] = c;
    else s_stageLost++;
    portEXIT_CRITICAL_SAFE(&s_stageMux);
}

void ringWrite(const uint8_t* data, size_t len) {
    portENTER_CRITICAL_SAFE(&s_mux);
    if (len > s_cap) { data += len - s_cap; s_total += len - s_cap; len = s_cap; }
    size_t pos = s_total % s_cap;
    const size_t first = (len < s_cap - pos) ? len : s_cap - pos;
    memcpy(s_buf + pos, data, first);
    if (len > first) memcpy(s_buf, data + first, len - first);
    s_total += len;
    portEXIT_CRITICAL_SAFE(&s_mux);
}

// Move whatever the ROM printed into the ring. Task context only.
void drainStage() {
    if (!s_stageLen || xPortInIsrContext()) return;
    static char local[kStageCap];
    static portMUX_TYPE drainMux = portMUX_INITIALIZER_UNLOCKED;   // two tasks draining at once
    portENTER_CRITICAL(&drainMux);
    portENTER_CRITICAL(&s_stageMux);
    const size_t n = s_stageLen;
    memcpy(local, s_stage, n);
    s_stageLen = 0;
    portEXIT_CRITICAL(&s_stageMux);
    ringWrite((const uint8_t*)local, n);
    portEXIT_CRITICAL(&drainMux);
}

// ── crash record ─────────────────────────────────────────────────────────────
// A panic prints its reason and backtrace to USB and reboots — and in the shop
// nothing is on USB, so all that survived was the link log's "rst":"panic". The
// core's panic hook (needs -Wl,--wrap=esp_panic_handler, platformio.ini) hands
// us the reason, the PC and the code addresses on the stack; they go into
// .noinit RAM, which a panic reset does not clear, and reportCrash() prints them
// on the next boot — into this log, so GET /api/serial carries them.
// Decode on a laptop with the same build's ELF:
//   riscv32-esp-elf-addr2line -pfiaC -e .pio.nosync/build/xiao_c5_primary/firmware.elf <addrs>
constexpr uint32_t kCrashMagic = 0xC2A5B007;
constexpr size_t   kCrashAddrs = 24;
struct CrashRecord {
    uint32_t magic;
    uint32_t pc;
    uint32_t n;
    uint32_t addrs[kCrashAddrs];
    char     reason[64];
};
__NOINIT_ATTR CrashRecord s_crash;

void IRAM_ATTR onPanic(arduino_panic_info_t* info, void*) {
    s_crash.pc = (uint32_t)info->pc;
    // backtrace[0] is the stack pointer on RISC-V (esp32-hal-misc.c); skip it.
    uint32_t n = 0;
    for (unsigned i = 1; i < info->backtrace_len && n < kCrashAddrs; i++) s_crash.addrs[n++] = info->backtrace[i];
    s_crash.n = n;
    size_t k = 0;
    if (info->reason) for (; k < sizeof(s_crash.reason) - 1 && info->reason[k]; k++) s_crash.reason[k] = info->reason[k];
    s_crash.reason[k] = 0;
    s_crash.magic = kCrashMagic;
}

} // namespace

void reportCrash() {
    if (s_crash.magic != kCrashMagic) return;
    s_crash.magic = 0;                         // say it once
    if (esp_reset_reason() != ESP_RST_PANIC) return;   // stale: a power cut since
    g_serialTee.printf("[CRASH] the last boot panicked: %s\n", s_crash.reason);
    g_serialTee.printf("[CRASH] PC 0x%08lx  stack:", (unsigned long)s_crash.pc);
    for (uint32_t i = 0; i < s_crash.n && i < kCrashAddrs; i++) g_serialTee.printf(" 0x%08lx", (unsigned long)s_crash.addrs[i]);
    g_serialTee.printf("\n[CRASH] decode with this build's ELF: riscv32-esp-elf-addr2line -pfiaC -e firmware.elf <addresses>\n");
}

void begin() {
    if (s_buf) return;
    s_buf = (char*)heap_caps_malloc(kCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) return;            // no PSRAM: the tee still prints, nothing is kept
    s_cap  = kCap;
    s_boot = esp_random();
    esp_rom_install_channel_putc(1, romPutc);
    set_arduino_panic_handler(onPanic, nullptr);
}

void write(const uint8_t* data, size_t len) {
    if (!s_buf) return;
    drainStage();
    if (len) ringWrite(data, len);
}

// Input queue. Writer: the web server's task. Reader: loop(). Same lock idea.
namespace {
constexpr size_t kInCap = 256;
char         s_in[kInCap];
size_t       s_inHead = 0, s_inLen = 0;
portMUX_TYPE s_inMux = portMUX_INITIALIZER_UNLOCKED;
}

bool inject(const char* line, size_t len) {
    bool ok = false;
    portENTER_CRITICAL(&s_inMux);
    if (s_inLen + len + 1 <= kInCap) {
        for (size_t i = 0; i < len; i++) s_in[(s_inHead + s_inLen++) % kInCap] = line[i];
        s_in[(s_inHead + s_inLen++) % kInCap] = '\n';
        ok = true;
    }
    portEXIT_CRITICAL(&s_inMux);
    return ok;
}
int injectedAvailable() { return (int)s_inLen; }
int injectedPeek() {
    int c = -1;
    portENTER_CRITICAL(&s_inMux);
    if (s_inLen) c = (uint8_t)s_in[s_inHead];
    portEXIT_CRITICAL(&s_inMux);
    return c;
}
int injectedRead() {
    int c = -1;
    portENTER_CRITICAL(&s_inMux);
    if (s_inLen) { c = (uint8_t)s_in[s_inHead]; s_inHead = (s_inHead + 1) % kInCap; s_inLen--; }
    portEXIT_CRITICAL(&s_inMux);
    return c;
}

uint32_t total()  { return s_total; }
uint32_t bootId() { return s_boot; }

size_t read(uint32_t from, char* out, size_t cap, uint32_t* start, uint32_t* next) {
    if (s_buf) drainStage();
    size_t n = 0;
    portENTER_CRITICAL_SAFE(&s_mux);
    const uint32_t end    = s_total;
    const uint32_t oldest = end > s_cap ? end - s_cap : 0;
    // A cursor from a previous boot (past the end) starts over from the oldest.
    uint32_t at = (from > end || from < oldest) ? oldest : from;
    if (s_buf) {
        n = end - at;
        if (n > cap) n = cap;
        const size_t pos   = at % s_cap;
        const size_t first = (n < s_cap - pos) ? n : s_cap - pos;
        memcpy(out, s_buf + pos, first);
        if (n > first) memcpy(out + first, s_buf, n - first);
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
    if (start) *start = at;
    if (next)  *next  = at + n;
    return n;
}

} // namespace seriallog

// ── the tee ──────────────────────────────────────────────────────────────────
// HWCDCSerial by name: in this file `Serial` means the tee itself.
SerialTee g_serialTee;

// ── line stamps ──────────────────────────────────────────────────────────────
// Every line the tee carries starts with when it was printed, so a log read over
// WiFi, or off a monitor, says WHEN without help from the reader. The clock is
// the one the link log uses: NTP, in UTC ("15:35:45.123Z"), once the network has
// answered; before that, uptime ("+63.512s"). Library lines that arrive through
// the ROM channel (log_e) are not stamped — they are copied into the ring later,
// from a task, and a stamp taken then would be wrong by however long that took.
//
// Two tasks printing at once can still interleave mid-line, as they always could;
// the flag below is not locked, so the worst case is a stamp missing or doubled
// on one of those interleaved lines.
static bool s_lineStart = true;

static size_t stampPrefix(char* out, size_t cap) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv.tv_sec > 1700000000) {
        struct tm t;
        gmtime_r(&tv.tv_sec, &t);
        return snprintf(out, cap, "%02d:%02d:%02d.%03dZ ", t.tm_hour, t.tm_min, t.tm_sec, (int)(tv.tv_usec / 1000));
    }
    const unsigned long ms = millis();
    return snprintf(out, cap, "+%lu.%03lus ", ms / 1000, ms % 1000);
}

size_t SerialTee::write(const uint8_t* buf, size_t len) {
    size_t i = 0;
    while (i < len) {
        if (s_lineStart && buf[i] != '\n' && buf[i] != '\r') {
            char p[24];
            const size_t n = stampPrefix(p, sizeof(p));
            seriallog::write((const uint8_t*)p, n);
            HWCDCSerial.write((const uint8_t*)p, n);
            s_lineStart = false;
        }
        size_t j = i;
        while (j < len && buf[j] != '\n') j++;
        if (j < len) j++;                              // keep the newline with its line
        seriallog::write(buf + i, j - i);
        HWCDCSerial.write(buf + i, j - i);
        if (buf[j - 1] == '\n') s_lineStart = true;
        i = j;
    }
    return len;
}
size_t SerialTee::write(uint8_t c) { return write(&c, 1); }
void SerialTee::begin(unsigned long baud) { HWCDCSerial.begin(baud); }
// A command from the app goes first: it was sent on purpose, a moment ago.
int  SerialTee::available()                { return seriallog::injectedAvailable() + HWCDCSerial.available(); }
int  SerialTee::read()                     { int c = seriallog::injectedRead(); return c >= 0 ? c : HWCDCSerial.read(); }
int  SerialTee::peek()                     { int c = seriallog::injectedPeek(); return c >= 0 ? c : HWCDCSerial.peek(); }
void SerialTee::flush()                    { HWCDCSerial.flush(); }
int  SerialTee::availableForWrite()        { return HWCDCSerial.availableForWrite(); }
SerialTee::operator bool() const           { return (bool)HWCDCSerial; }

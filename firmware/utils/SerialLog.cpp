// utils/SerialLog.cpp — see SerialLog.h. Also defines the Serial tee that
// utils/SerialCapture.h swaps in for `Serial`.
#include <Arduino.h>
#include <esp_rom_sys.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
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

// Called by esp_rom_printf, one character at a time, from wherever log_e() ran.
// That can be an ISR, and an ISR may run while flash is being written — when
// the cache is off and PSRAM cannot be touched at all. Task context is safe even
// then: on this single-core chip a flash write suspends every other task. So
// drop what an ISR prints rather than risk the one read that crashes the board.
void romPutc(char c) {
    if (xPortInIsrContext()) return;
    const uint8_t b = (uint8_t)c;
    write(&b, 1);
}

} // namespace

void begin() {
    if (s_buf) return;
    s_buf = (char*)heap_caps_malloc(kCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) return;            // no PSRAM: the tee still prints, nothing is kept
    s_cap  = kCap;
    s_boot = esp_random();
    esp_rom_install_channel_putc(1, romPutc);
}

void write(const uint8_t* data, size_t len) {
    if (!s_buf || !len) return;
    portENTER_CRITICAL_SAFE(&s_mux);
    if (len > s_cap) { data += len - s_cap; s_total += len - s_cap; len = s_cap; }
    size_t pos = s_total % s_cap;
    const size_t first = (len < s_cap - pos) ? len : s_cap - pos;
    memcpy(s_buf + pos, data, first);
    if (len > first) memcpy(s_buf, data + first, len - first);
    s_total += len;
    portEXIT_CRITICAL_SAFE(&s_mux);
}

uint32_t total()  { return s_total; }
uint32_t bootId() { return s_boot; }

size_t read(uint32_t from, char* out, size_t cap, uint32_t* start, uint32_t* next) {
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

size_t SerialTee::write(uint8_t c) {
    seriallog::write(&c, 1);
    return HWCDCSerial.write(c);
}
size_t SerialTee::write(const uint8_t* buf, size_t len) {
    seriallog::write(buf, len);
    return HWCDCSerial.write(buf, len);
}
void SerialTee::begin(unsigned long baud) { HWCDCSerial.begin(baud); }
int  SerialTee::available()                { return HWCDCSerial.available(); }
int  SerialTee::read()                     { return HWCDCSerial.read(); }
int  SerialTee::peek()                     { return HWCDCSerial.peek(); }
void SerialTee::flush()                    { HWCDCSerial.flush(); }
int  SerialTee::availableForWrite()        { return HWCDCSerial.availableForWrite(); }
SerialTee::operator bool() const           { return (bool)HWCDCSerial; }

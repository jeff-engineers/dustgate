// =============================================================================
// test/shim/Arduino.h — just enough Arduino to host-test sensing/CtTrip.h.
//
// WHY THIS EXISTS AND NOTHING ELSE USES IT. Every other decision-making header
// in this project is PURE (STL + ArduinoJson, no Arduino.h) precisely so the
// host tests can drive it — BinSensor.h says so at the top. CtTrip.h cannot be:
// it reads an ADC through CtSensor, which busy-waits on millis(). So the choice
// was between leaving the logic that decides WHETHER A GATE OPENS with no
// coverage at all, or shimming the three functions it actually needs. This is
// the second one.
//
// The shim is deliberately dumb. It fakes a clock and an ADC and throws away
// everything printed. It is NOT an Arduino emulator and must never grow into
// one: if a header needs more of Arduino than this, that header wants to be
// split the way BinSensor was, not accommodated here.
//
// THE CLOCK ADVANCES ON A DIVIDER, which is the one non-obvious decision in the
// file and it took a failing run to get right. CtSensor::read() is
// `while (millis() - t0 < windowMs)`, so a frozen clock is an infinite loop.
// But one millisecond per call is also wrong: read() refuses a window that
// collected fewer than 100 samples (`if (n < 100) return r;` — valid stays
// false), so a 60 ms window at one sample per millisecond returns NOTHING, and
// every test reads as "no floor learnt" for a reason that has nothing to do with
// the code under test.
//
// 25 calls per millisecond is both large enough to clear that guard and honest:
// the real board measured 25-29 kSPS (RFC §5.5b), so a 60 ms window yields ~1500
// samples here against ~1600 there.
// =============================================================================
#pragma once
#include <cstdint>
#include <cstddef>
#include <cmath>

// ── the fake clock ──────────────────────────────────────────────────────────
namespace ardushim {
inline uint32_t& clockMs() { static uint32_t t = 0; return t; }

// What analogRead() will return, as a peak amplitude in counts around midpoint.
// The test sets this; the "signal" is a triangle rather than a sine because
// nothing here measures harmonics and a triangle needs no <cmath> per sample.
inline int& amplitude() { static int a = 0; return a; }
inline int& midpoint()  { static int m = 2048; return m; }
inline uint32_t& sampleIndex() { static uint32_t i = 0; return i; }

inline uint32_t& subTick();
inline void reset(int amp = 0) {
    clockMs() = 0; amplitude() = amp; midpoint() = 2048; sampleIndex() = 0;
    subTick() = 0;
}
}  // namespace ardushim

namespace ardushim {
// Sub-millisecond call counter — see the header note on the divider.
inline uint32_t& subTick() { static uint32_t t = 0; return t; }
static constexpr uint32_t kCallsPerMs = 25;
}  // namespace ardushim

inline uint32_t millis() {
    if (++ardushim::subTick() >= ardushim::kCallsPerMs) {
        ardushim::subTick() = 0;
        ardushim::clockMs()++;
    }
    return ardushim::clockMs();
}
inline uint32_t micros() { return ardushim::clockMs() * 1000u; }
inline void     delay(uint32_t ms) { ardushim::clockMs() += ms; }

// A square wave at the requested amplitude: RMS == amplitude exactly, which
// makes every expected value in the test arithmetic rather than a tolerance.
inline uint32_t analogRead(int) {
    const uint32_t i = ardushim::sampleIndex()++;
    return (uint32_t)(ardushim::midpoint() + ((i & 1) ? ardushim::amplitude()
                                                      : -ardushim::amplitude()));
}
// 0.61 mV/LSB at 11 dB, the figure CtSensor's own comment cites.
inline uint32_t analogReadMilliVolts(int) {
    return (uint32_t)(ardushim::midpoint() * 0.61f);
}

inline void pinMode(int, int) {}
inline void analogSetPinAttenuation(int, int) {}
#define INPUT 0
#define ADC_11db 3

// ── a Serial that goes nowhere ──────────────────────────────────────────────
#define F(x) (x)
struct FakeSerial {
    void print(const char*)        {}
    void print(float, int = 2)     {}
    void print(int)                {}
    void print(unsigned)           {}
    void println()                 {}
    void println(const char*)      {}
    void println(float, int = 2)   {}
    void println(int)              {}
    void flush()                   {}
};
inline FakeSerial Serial;

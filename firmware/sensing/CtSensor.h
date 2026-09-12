// =============================================================================
// CtSensor.h — a split-core CT clamp, read as RMS current.
//
// Lifted from firmware/bench/ct_bench.cpp, which is where every decision in it
// was made and where the failures that shaped it are recorded. Read
// firmware/wiring/ct-bench.md §5.5 before trusting a number: **the noise floor
// is unresolved and the screen is part of it.**
//
// THERE IS NO RECTIFIER. A CT puts out AC about zero; the bias network moves
// that to mid-rail so the ADC can see it, and the RMS is taken arithmetically —
// sample flat out, subtract the MEASURED mean, take the RMS of what is left.
// A diode would drop 0.7 V against a signal whose full scale is 1 V RMS, and
// would delete the small end, which is the entire question. See
// wiring/collector-node.md §3.
//
// SUBTRACTING THE MEASURED MEAN is what makes the divider's exact midpoint
// irrelevant — a lazy divider and a drifting reference both come out in the
// wash, which is why there is no trim anywhere here.
//
// NOT A PowerSensor, and that is deliberate (2026-09-11). PowerSensor's contract
// is a VERDICT — isActive() — and a CT cannot reach one yet: §5.4a of the
// tool-sensing RFC says the threshold shape for a CT is an open question (a
// learned per-tool baseline, not a wattage), and the noise floor under it is
// unresolved. Wiring this to the routing brain before either is settled would
// be shipping a guess. It reports; nothing consumes it but the console.
// =============================================================================

#pragma once
#include <Arduino.h>

class CtSensor {
public:
    // SCT-013-030: 30 A RMS through the jaw gives 1 V RMS out, burden resistor
    // built in. Linear, so amps are volts times this.
    static constexpr float kAmpsPerVolt = 30.0f;

    explicit CtSensor(int pin) : _pin(pin) {}

    struct Reading {
        float    amps    = 0;   // RMS of the AC component
        float    hz      = 0;   // dominant frequency, from zero crossings
        uint32_t dcMv    = 0;   // the bias point — see isRailed()
        float    dcCounts = 0;
        float    kSps    = 0;   // sample rate actually achieved
        bool     valid   = false;
    };

    // ⚠️ THE ONLY CHECK THAT MATTERS BEFORE BELIEVING AN AMP READING.
    //
    // A railed input reads a CONSTANT, and the variance of a constant is zero —
    // which looks exactly like a perfectly quiet sensor. 0.000 A from a railed
    // pin is indistinguishable from 0.000 A from a genuinely idle tool, and the
    // first version of the bench rig spent a whole session reporting the former
    // as the latter (wiring/ct-bench.md).
    //
    // Healthy is ~1650 mV, the divider halving 3.3 V. Anything near either rail
    // means the bias network is not doing its job and every number is fiction.
    static bool isRailed(const Reading& r) {
        return r.dcMv < 200 || r.dcMv > 3100;
    }

    // Sample for `windowMs` and return the RMS.
    //
    // 200 ms is 12 cycles at 60 Hz — long enough that a half-cycle error is
    // noise, short enough to feel live.
    //
    // RAW COUNTS, NOT MILLIVOLTS, and that is a correction rather than a
    // preference. analogReadMilliVolts() returns WHOLE millivolts; with the
    // screen off the input is quieter than that, so every sample came back the
    // same integer, the variance was exactly zero, and the meter printed 0.000 A
    // for minutes. That is not a measurement, it is a floor made of rounding —
    // it hid everything below 1 mV RMS, which is 0.03 A on this CT.
    //
    // analogRead() is ~0.61 mV/LSB at 11 dB: 1.6x finer, and faster, so more
    // samples per window too. The mV-per-count scale is derived ONCE per window
    // from a single analogReadMilliVolts() of the same input, which keeps the
    // per-chip ADC calibration without paying for it thousands of times.
    Reading read(uint32_t windowMs = 200) {
        Reading r;
        const uint32_t t0 = millis();
        uint32_t n = 0, crossings = 0;
        double sum = 0, sumSq = 0;
        bool above = false, seeded = false;

        while (millis() - t0 < windowMs) {
            const uint32_t c = analogRead(_pin);
            sum   += c;
            sumSq += (double)c * (double)c;
            n++;
            // Crossings are counted against the PREVIOUS window's mean, which is
            // stable, rather than this window's, which is not known yet.
            if (_prevMean > 1) {
                const bool nowAbove = ((float)c > _prevMean);
                if (!seeded) { above = nowAbove; seeded = true; }
                else if (nowAbove != above) { crossings++; above = nowAbove; }
            }
        }
        if (n < 100) return r;                  // valid stays false

        const uint32_t took = millis() - t0;
        const double mean = sum / n;
        const double var  = (sumSq / n) - (mean * mean);
        const double rmsCounts = (var > 0 ? sqrt(var) : 0);

        const uint32_t meanMv = analogReadMilliVolts(_pin);
        const double mvPerCount = (mean > 1) ? ((double)meanMv / mean) : 0.61;

        _prevMean   = (float)mean;
        r.kSps      = (float)n / (float)took;   // samples/ms == kSPS
        r.hz        = (crossings / 2.0f) * (1000.0f / (float)took);
        r.dcMv      = meanMv;
        r.dcCounts  = (float)mean;
        r.amps      = (float)(rmsCounts * mvPerCount / 1000.0 * kAmpsPerVolt);
        r.valid     = true;
        return r;
    }

private:
    int   _pin;
    float _prevMean = 0;
};

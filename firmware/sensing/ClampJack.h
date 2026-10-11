// =============================================================================
// sensing/ClampJack.h — is a current clamp plugged into this board's jack? (2026-10-09)
//
// It replaced the per-board "clamp" switch in the layout. caps.ct is the pin map — true of every PWM board, plugged or
// not — so the layout used to carry `clamp: true` for a person to say "this one has a clamp". The switched 3.5 mm jack
// makes that a reading instead: with no plug in, the jack's switch ties the tip to 3V3 through 10 kΩ (WIRING.md §8), and
// the board enables D0's own pull-up so a pad with nothing wired to it reads the same. A clamp holds the pin at the
// bias midpoint (~1.65 V). So: railed high = nothing plugged in; mid-rail = a clamp.
//
// RAILED LOW (under 200 mV) also reads as no clamp: it is a pad shorted to ground or a broken bias divider, and in
// neither case is there a clamp anyone can read. The thresholds are CtSensor::isRailed()'s, so a reading this calls
// plugged in is one the trip logic will also accept.
//
// The MEAN, over about one mains cycle, is what is judged: a running tool swings the pin around the midpoint and the
// average stays there, so a saw starting cannot read as an unplugged clamp.
//
// PURE — no Arduino.h. The board passes its own ADC read and delay to probeMeanMv(). test_clampjack.cpp.
// ⚠ HARDWARE-UNVERIFIED: that the C5's ADC keeps D0's internal pull-up enabled (gpio_pullup_en after the ADC is set
// up). Without it, a board with NOTHING on D0 floats and may read as plugged in; a jack build is unaffected (its 10 kΩ).
// =============================================================================
#pragma once
#include <cstdint>

namespace sensing {

class ClampJack {
public:
    static constexpr uint32_t kProbeEveryMs = 1000;   // a jack is a slow fact; once a second costs ~16 ms of loop
    static constexpr uint32_t kLowRailMv    = 200;    // CtSensor::isRailed()'s two thresholds
    static constexpr uint32_t kHighRailMv   = 3100;
    static constexpr int      kSamples      = 16;     // ~1 ms apart: one 60 Hz cycle, so a running tool averages out
    static constexpr uint32_t kSampleGapUs  = 1000;

    // The mean of kSamples reads, spread over about one mains cycle.
    template <typename ReadMv, typename DelayUs>
    static uint32_t probeMeanMv(ReadMv readMv, DelayUs delayUs) {
        uint32_t sum = 0;
        for (int i = 0; i < kSamples; i++) { sum += (uint32_t)readMv(); if (i + 1 < kSamples) delayUs(kSampleGapUs); }
        return sum / kSamples;
    }

    static bool looksPlugged(uint32_t meanMv) { return meanMv >= kLowRailMv && meanMv <= kHighRailMv; }

    bool due(uint32_t nowMs) const { return !_probed || (uint32_t)(nowMs - _lastProbeMs) >= kProbeEveryMs; }

    // One probe. The FIRST decides at once (a board should say what it has as soon as it links); after that a change
    // must hold for two probes in a row, so one disturbed read — a plug half-way in — does not flap the report.
    // Returns true when the answer changed.
    bool update(uint32_t meanMv, uint32_t nowMs) {
        _probed = true; _lastProbeMs = nowMs;
        const int v = looksPlugged(meanMv) ? 1 : 0;
        if (_state < 0) { _state = v; _pending = -1; return true; }
        if (v == _state) { _pending = -1; return false; }
        if (_pending == v) { _state = v; _pending = -1; return true; }
        _pending = v;
        return false;
    }

    bool known()   const { return _state >= 0; }
    bool plugged() const { return _state == 1; }

private:
    int      _state = -1;        // -1 not yet probed, 0 empty, 1 plugged in
    int      _pending = -1;      // a different answer seen once, waiting for a second
    bool     _probed = false;
    uint32_t _lastProbeMs = 0;
};

} // namespace sensing

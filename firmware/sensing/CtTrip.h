// =============================================================================
// sensing/CtTrip.h — is the clamped motor RUNNING? One answer, one copy.
//
// CtSensor measures; this decides. The split matters because the measurement is
// a fact about the hardware and the decision is a policy with two provisional
// numbers in it, and only the second one is likely to change.
//
// WHY IT IS ITS OWN FILE. This logic was written once inside the node
// (firmware/node/dustgate_node.cpp) and then needed a second time on the
// PRIMARY, because a clamp at the collector is wired to the board that IS the
// brain. Copying it would have repeated the exact mistake ct_bench.cpp made
// against CtSensor: a divergent second copy that was missing the one-sample
// scale fix and the settle gate, and read plausibly wrong for weeks. "We should
// be using the same CT code between all the variants" — 2026-09-16, and this is
// what that means for the part above the ADC.
//
// WHAT IT OWNS
//   - the sampling CADENCE, which is not free: CtSensor::read() busy-waits for
//     its whole window
//   - the settle gate, the railed check, and the moving-actuator hold
//   - the board's own noise FLOOR, learnt once
//   - the trip point derived from it, and the on/off bit
//
// WHAT IT DOES NOT OWN: anything about the document. `sensorId` never reaches
// here. A caller asks "is the clamp reading a running motor" and gets a bool —
// the node turns that into a SENSE frame, the primary turns it into a local
// reading, and neither interpretation belongs in this file. That is the node/
// primary invariant (shared/device-model/nodelink.js) applied one layer down.
// =============================================================================
#pragma once
#include <Arduino.h>
#include "CtSensor.h"

namespace sensing {

// How often the clamp is actually SAMPLED, as opposed to reported.
//
// Sampling is the expensive part: CtSensor::read() busy-waits for its whole
// window, so reading every loop() pass would hand a quarter of a board's time to
// the ADC and coarsen every servo sweep it also has to drive. 250 ms puts
// worst-case detection at ~310 ms, comfortably inside the 500 ms the RFC asks of
// a tool-on event and irrelevant beside the collector's 4 s spin-up grace.
static constexpr uint32_t kSampleIntervalMs = 250;
static constexpr uint32_t kSampleWindowMs   = 60;   // ~3.5 cycles at 60 Hz

// ⚠️ PROVISIONAL, BOTH OF THEM. The clamp's SCALE is confirmed to ~1%
// (RFC §5.5a) but the FLOOR is not — the perfboard rigs were rebuilt from
// 10k/10k to 1k/1k and nothing has been re-measured on them (see TODO). These
// are sized to be obviously safe rather than tight:
//
//   kTripRatio     4x the learned floor. The measured gap between a quiet board
//                  and a running motor was ~80x, so 4 is not a close call.
//   kMinTripCounts an absolute guard, because a ratio against a floor that
//                  learns near zero trips on nothing. ~8 counts is roughly
//                  0.24 A on this clamp — far below any real tool, far above
//                  the quantisation floor.
//
// Re-derive both from the rebuilt divider before trusting this on a tool that
// matters.
static constexpr float kTripRatio     = 4.0f;
static constexpr float kMinTripCounts = 8.0f;

class CtTrip {
public:
    struct Tick {
        bool  sampled = false;  // false: nothing was measured this pass
        bool  on      = false;  // the answer, valid only when `sampled`
        float rmsCounts = 0;    // raw, for the log — see CtSensor::Reading
        float trip      = 0;    // the point `rmsCounts` was judged against
        // Multiple of the trip point. DIAGNOSTIC ONLY: it rides the SENSE frame
        // so a human can see how close a call was, and nothing routes on it.
        float level     = -1.0f;
    };

    // Call every loop(). Does real work at most every kSampleIntervalMs, and
    // only when the caller says there is something to watch.
    //
    // `actuatorBusy` holds sampling off while a gate is moving: a blocking
    // sample mid-sweep makes the sweep jerk, and the information is worthless
    // anyway — if this board is moving a gate then the collector is already
    // running, and the tool is noticed a couple of hundred ms later, which
    // nothing downstream can tell.
    //
    // `petWatchdog` is passed in rather than called directly so this file needs
    // no dependency on the watchdog: the read busy-waits through its whole
    // window and both callers arm a watchdog around it.
    Tick update(CtSensor& ct, uint32_t nowMs, bool actuatorBusy,
                void (*petWatchdog)() = nullptr) {
        Tick t;

        // NOTHING IS MEASURED BEFORE THE BIAS ARRIVES. A midpoint still charging
        // makes every sample ride a moving reference, and the floor learnt from
        // it would be wrong for the whole session — wrong HIGH, so a running
        // tool reads as idle. isRailed() cannot catch it; a charging midpoint
        // passes straight through the healthy band. See CtSensor::kBiasSettleMs.
        if (!ct.settled()) return t;
        if (actuatorBusy)  return t;

        if (_lastSampleMs && (uint32_t)(nowMs - _lastSampleMs) < kSampleIntervalMs) return t;
        _lastSampleMs = nowMs;

        if (petWatchdog) petWatchdog();
        const CtSensor::Reading r = ct.read(kSampleWindowMs);
        if (petWatchdog) petWatchdog();
        if (!r.valid || r.settling || CtSensor::isRailed(r)) return t;

        // THE FLOOR IS THIS BOARD'S OWN NOISE, learnt once (RFC §5.4b). The
        // wiring guarantees it is valid: the supply is tapped UPSTREAM of the
        // tool's own switch and the clamp sits downstream, so at boot the motor
        // is off by construction and there is nothing on the clamped conductor
        // to measure.
        if (!_floorLearnt) {
            _floorCounts = r.rmsCounts;
            _floorLearnt = true;
            Serial.print(F("[CT] floor learnt: ")); Serial.print(_floorCounts, 1);
            Serial.println(F(" counts — everything above this is a running tool."));
        }

        t.trip = (_floorCounts * kTripRatio) > kMinTripCounts
               ? (_floorCounts * kTripRatio) : kMinTripCounts;
        t.sampled   = true;
        t.rmsCounts = r.rmsCounts;
        t.on        = r.rmsCounts > t.trip;
        t.level     = t.trip > 0.0f ? r.rmsCounts / t.trip : -1.0f;
        return t;
    }

    bool  floorLearnt() const { return _floorLearnt; }
    float floorCounts() const { return _floorCounts; }

private:
    uint32_t _lastSampleMs = 0;
    float    _floorCounts  = 0.0f;
    bool     _floorLearnt  = false;
};

} // namespace sensing

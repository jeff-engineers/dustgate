// =============================================================================
// sensing/CtTrip.h — is the clamped motor RUNNING? One answer, one copy.
//
// CtSensor measures; this decides. The split matters because the measurement is
// a fact about the hardware and the decision is a policy. Since 2026-09-17 that
// policy's three numbers arrive from the PRIMARY (TripParams, below) rather than
// being baked into whatever board happens to be holding the clamp — which is
// what lets a node's firmware be finished.
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
//   - the board's own noise FLOOR, learnt once — and REFUSED when it is too
//     high to be a board at rest (kMaxFloorCounts)
//   - the trip point derived from it, its RELEASE point, and the on/off bit
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

// THE TRIP NUMBERS ARE SETTLED AS OF 2026-09-17, and this comment says so
// explicitly because it spent a week saying the opposite for three different
// reasons, each of which turned out to be wrong. Keeping the history compressed
// here is cheaper than having it re-derived a fourth time.
//
//   "the divider was rebuilt and nothing re-measured" — done 2026-09-16. Scale
//       held (+1.50% vs the Tasmota). RFC §5.5 is closed.
//   "the floor swings 10x with where a board boots" — it does not. The ~2.2 A
//       reading that suggested it came off the BREADBOARD and was a platform
//       fault, not a location: dcCounts drifted 1671 -> 1744 -> 1778 and stayed,
//       which ambient field cannot do, since magnetic pickup is AC with a zero
//       mean. A spring contact on a high-impedance analog node. WIRING.md had
//       already written the tell down in the same session.
//   "a 0.78 A trip is too sensitive" — nothing else is on the conductor. A clamp
//       goes INSIDE the tool's own switch or motor enclosure, around that tool's
//       wire. There is no shop vac or charger to pick up. (A whole-circuit clamp
//       would be a different question; we do not install one.)
//   "lead length is untested" — the SCT-013's lead is fixed at ~1 m and every
//       measurement has been taken at full length. The worst case IS the
//       measured case, and there is no shorter-lead variant to be surprised by.
//
// THE FLOOR IS ~0.195 A (6.68 counts), measured at the collector with the board
// a couple of feet away on the full lead, and it barely moves: 0.179 screen off,
// 0.173 on a no-WiFi build, and UNCHANGED WITH THE CT SHORTED OUT. That last
// control is what settles it — it is the C5's own SAR ADC noise, which 9-10 bit
// ENOB predicts, not anything in the shop.
//
// So the trip lands at max(4 x 6.68, 8) = ~26.7 counts, ~0.78 A, and every real
// load clears it: SawStop running 6.4x, 2 HP planer no-load 9x, collector 13.6x.
// SawStop STANDBY stays invisible at 0.043 A, which is the right answer and not
// a near miss — the hazard here is a false POSITIVE, a gate opening for a tool
// nobody is using. kMinTripCounts never binds at this floor; it is insurance
// against a board that learns a floor near zero, not a working limit.
//
// What each one is:
//
//   kTripRatio     4x the learned floor.
//   kMinTripCounts an absolute guard, because a ratio against a floor that
//                  learns near zero trips on nothing.
//   kClearRatio    where it turns back OFF, as a fraction of the trip point.
//                  ⚠️ THE ONE NUMBER HERE STILL WITHOUT BENCH EVIDENCE — it is
//                  new, it was chosen to be obviously safe, and no hardware has
//                  run it. See THE RELEASE POINT below.
//
// SINCE 2026-09-17 THESE ARE ONLY THE FALLBACK. The primary sends its own values
// in the CONFIG frame's SensorSpec and a node uses those, so retuning a shop is a
// PRIMARY reflash and nobody climbs to a node. These remain what a board uses
// when a CONFIG is silent about them, which is any frame from a primary older
// than that date — so they must stay VALID, not merely present.
//
static constexpr float kTripRatio     = 4.0f;
static constexpr float kMinTripCounts = 8.0f;

// THE RELEASE POINT, and why a single threshold was a bug rather than a
// simplification.
//
// Until now the answer was a bare `rmsCounts > trip`, which means a tool sitting
// anywhere near its trip point does not report a state — it reports a STREAM of
// them. Every flip is a SENSE frame (they are sent on change), and every frame
// the primary believes is a tool starting or stopping, which is a gate move and
// a collector decision. The quiet-to-running gap is ~80x, so this does not
// happen to a table saw; it happens exactly where it is least welcome, on the
// marginal load nobody is sure about — the case Jeff hit on 2026-09-16 with a
// back massager that would not trip at all.
//
// 0.75 gives a 25% band. Wide enough that ADC noise cannot cross it, narrow
// enough that a tool genuinely switching off still clears it at once, since off
// is a fall to the floor rather than a fall to 74%.
//
// Asymmetry is deliberate and it is NOT a substitute for the collector's
// coast-down: that lives downstream in kDefaultCollectorOffDelayMs and answers a
// different question (how long the blower keeps running after the last tool
// stops). This one answers whether the tool stopped at all.
static constexpr float kClearRatio    = 0.75f;

// THE FLOOR SANITY LIMIT — the guard `kMinTripCounts` is not.
//
// kMinTripCounts stops a floor that learns too LOW from tripping on nothing.
// Nothing stopped a floor that learns too HIGH, and that is the failure that
// actually happened: the breadboard rig read ~76 counts (2.2 A), which computes
// a trip of 8.8 A. On that board the SawStop at 5 A is invisible, the planer at
// 7 A is invisible, and only the dust collector is ever noticed — silently, with
// an entirely plausible-looking number in the log. A board that quietly notices
// one machine out of four is worse than a board that says it is broken.
//
// 34 counts is ~1 A on this clamp, and the number is not round by accident —
// though the first version of this note justified it badly, as "a decade over the
// real floor and a fifth of the quietest tool", which is true and arbitrary.
//
// The reason it belongs at 34: a floor of 34 puts the trip at 4 x 34 = 136
// counts, and the SawStop running is 171. So the guard fires at almost exactly
// the floor where the QUIETEST TOOL WORTH SENSING stops being reliably separable
// from it. Below the limit the system works; above it, it has already stopped
// working for the marginal case whether or not anything says so. That is what a
// limit should mean.
//
// The healthy floor is 6.2-7.0 counts, measured across four sessions on two
// supply topologies (RFC §5.5b and WIRING.md §2), so a real board sits about 5x
// under the limit. The breadboard fault that motivated this was 76 — 11x the
// healthy floor, and comfortably caught.
//
// A CEILING ON THE TRIP POINT WOULD HAVE BEEN THE WRONG FIX, and it is worth
// saying why since it is the obvious one: capping trip at ~2 A leaves a board
// whose floor is 2.2 A reading permanently ON — gate open, blower running,
// forever. That trades a silent deaf board for a loud stuck-on one. Refusing the
// FLOOR instead keeps the board honest in both directions.
//
// IT RETRIES RATHER THAN LATCHING, which fixes a second bug in passing. Today a
// tool that happens to be running at boot poisons the floor for the whole
// session — the wiring is supposed to make that impossible (supply tapped
// upstream of the tool's own switch), but "supposed to" is doing a lot of work
// in someone else's shop. Leaving _floorLearnt false means every later sample is
// another chance, so the floor lands correctly the moment the tool stops.
static constexpr float kMaxFloorCounts = 34.0f;

// What a board was TOLD to use, as opposed to what it was built with.
//
// Carried in CONFIG.sensors[] (SensorSpec in control/NodeLink.h, nodelink.js).
// Defaulted to the constants above so every caller that does not care — the
// bench console, a test — reads exactly as it did before.
//
// These are HARDWARE facts, not document facts, which is what makes them
// legal on a wire that deliberately carries no topology: a multiple of a
// board's own learned noise floor and a guard in that board's own ADC counts
// mean nothing outside the board they describe. The threshold nodelink.js
// refuses to carry is the WATTAGE one, which names a machine.
struct TripParams {
    float tripRatio  = kTripRatio;
    float minCounts  = kMinTripCounts;
    float clearRatio = kClearRatio;
};

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
        // The clamp is reading far too much for a board at rest, so no floor was
        // learnt and there is no trip point. `on` is false — refusing to guess is
        // the safe answer — and the caller is expected to say so out loud.
        bool  floorFault = false;
        // AMPS PER ADC COUNT for this read, so a consumer can render any of the
        // counts above in amps without owning a hardware constant. Derived from
        // the reading itself (amps / rmsCounts) rather than fixed, because the
        // mV-per-count scale is averaged per window — see CtSensor::read(). The
        // UI multiplies; nothing here or upstream branches on it.
        //
        // Negative when this read produced no usable signal to divide by.
        float aPerCount = -1.0f;
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
                const TripParams& p = TripParams(),
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
            if (r.rmsCounts > kMaxFloorCounts) {
                // REFUSED, NOT CLAMPED, and not latched either — see
                // kMaxFloorCounts. Reported so the caller can raise it; a board
                // that cannot measure its own quiet is a board with a wiring
                // fault, not a board with a high threshold.
                t.sampled    = true;
                t.rmsCounts  = r.rmsCounts;
                t.floorFault = true;
                t.aPerCount  = r.rmsCounts > 0 ? r.amps / r.rmsCounts : -1.0f;
                if (!_floorFaulted) {
                    _floorFaulted = true;
                    Serial.print(F("[CT] FLOOR REFUSED: ")); Serial.print(r.rmsCounts, 1);
                    Serial.print(F(" counts (limit ")); Serial.print(kMaxFloorCounts, 0);
                    Serial.println(F("). A board at rest cannot read this much."));
                    Serial.println(F("[CT]   Check the clamp, its plug and its lead — or is a tool already running?"));
                    Serial.println(F("[CT]   Nothing is sensed until this clears. Retrying every sample."));
                }
                return t;
            }
            _floorCounts = r.rmsCounts;
            _floorLearnt = true;
            if (_floorFaulted) {
                _floorFaulted = false;
                Serial.println(F("[CT] floor recovered."));
            }
            Serial.print(F("[CT] floor learnt: ")); Serial.print(_floorCounts, 1);
            Serial.println(F(" counts — everything above this is a running tool."));
        }

        t.trip = (_floorCounts * p.tripRatio) > p.minCounts
               ? (_floorCounts * p.tripRatio) : p.minCounts;
        t.sampled   = true;
        t.rmsCounts = r.rmsCounts;
        t.aPerCount = r.rmsCounts > 0 ? r.amps / r.rmsCounts : -1.0f;

        // TWO POINTS, NOT ONE. Rising crosses `trip`; falling has to get all the
        // way back down to `trip * clearRatio` before this says the tool stopped.
        // Without the second point a marginal load chatters, and a chattering
        // bit is not a noisy log — it is a gate moving back and forth.
        t.on = _on ? (r.rmsCounts > t.trip * p.clearRatio)
                   : (r.rmsCounts > t.trip);
        _on  = t.on;

        t.level     = t.trip > 0.0f ? r.rmsCounts / t.trip : -1.0f;
        return t;
    }

    bool  floorLearnt() const { return _floorLearnt; }
    // True while this board has never managed to measure its own quiet. Distinct
    // from "not settled yet": that resolves in seconds, this needs a human.
    bool  floorFaulted() const { return _floorFaulted; }
    float floorCounts() const { return _floorCounts; }

private:
    uint32_t _lastSampleMs = 0;
    bool     _on           = false;   // the hysteresis state — see THE RELEASE POINT
    bool     _floorFaulted = false;   // so the refusal logs once, not 4x a second
    float    _floorCounts  = 0.0f;
    bool     _floorLearnt  = false;
};

} // namespace sensing

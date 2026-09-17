// =============================================================================
// test_cttrip.cpp — host tests for sensing/CtTrip.h.
//
// NOT half of an anti-drift pair, and CLAUDE.md's rule says to state that
// rather than leave it ambiguous: no JS models a clamp, so the floor, the trip
// ratio and the release point exist once and have nothing to drift against. The
// same shape as kBinDebounceMs. What IS paired is the range-checking on the
// three tuning fields where they cross the wire, and that lives in
// test_nodebus.cpp against nodelink.test.js.
//
// WHY IT NEEDED A SHIM. Every other header of this kind is pure so the host can
// drive it. This one reads an ADC through CtSensor, which busy-waits on
// millis(), so it gets test/shim/Arduino.h — see that file for the reasoning and
// for why it must not grow.
//
// WHAT THIS COVERS, and it was all untested until 2026-09-17: the floor learn,
// the floor REFUSAL and its recovery, the trip arithmetic, hysteresis, and the
// primary-supplied TripParams overriding the built-in defaults. This is the
// logic that decides whether a gate opens.
//
// Build + run via tools/ script `firmware:cttrip:test`.
// =============================================================================

#include "shim/Arduino.h"
#include "../sensing/CtTrip.h"
#include <cstdio>
#include <string>

static int passed = 0, failed = 0;
static void ok(const char* what, bool cond, const std::string& got = "") {
    if (cond) { printf("  ok   %s\n", what); passed++; }
    else      { printf("  FAIL %s%s%s\n", what,
                       got.empty() ? "" : "  got: ", got.c_str()); failed++; }
}

// Drive one sample at a given amplitude, jumping the clock far enough that the
// cadence gate always opens. Returns the Tick so a test can read the verdict.
static sensing::CtTrip::Tick step(sensing::CtTrip& trip, CtSensor& ct,
                                  int amplitudeCounts,
                                  const sensing::TripParams& p = sensing::TripParams()) {
    ardushim::amplitude() = amplitudeCounts;
    ardushim::clockMs() += sensing::kSampleIntervalMs + 1;
    return trip.update(ct, ardushim::clockMs(), false, p);
}

int main() {
    printf("\nCtTrip — the floor, the trip point, and the release point\n");

    // ── the floor is learnt once, from the first valid sample ───────────────
    {
        ardushim::reset();
        CtSensor ct(0);
        sensing::CtTrip trip;
        const sensing::CtTrip::Tick t = step(trip, ct, 7);
        ok("the first sample learns the floor", trip.floorLearnt());
        ok("and the floor is what it measured",
           trip.floorCounts() > 6.5f && trip.floorCounts() < 7.5f);
        // 4 x 7 = 28, which is over kMinTripCounts (8), so the ratio wins. This
        // is the real bench case: the guard does not bind at a healthy floor.
        ok("the trip is 4x the floor, not the absolute guard",
           t.trip > 27.0f && t.trip < 29.0f);
        ok("and a board at rest reads OFF", t.sampled && !t.on);
    }

    // ── kMinTripCounts binds only when the floor learns near zero ───────────
    {
        ardushim::reset();
        CtSensor ct(0);
        sensing::CtTrip trip;
        const sensing::CtTrip::Tick t = step(trip, ct, 1);
        ok("a near-zero floor falls back to the absolute guard",
           t.trip > 7.9f && t.trip < 8.1f);
        // Without the guard the trip would be 4 counts and ordinary quantisation
        // noise would report a running tool on a board with nothing plugged in.
        ok("...which is above what the floor alone would give", t.trip > 4.0f * 1.0f);
    }

    // ── THE FLOOR REFUSAL (2026-09-17) ──────────────────────────────────────
    //
    // The failure this exists for is real and already happened: the breadboard
    // rig read ~76 counts, which computes a trip of ~300 and leaves a board that
    // notices the dust collector and nothing else.
    {
        ardushim::reset();
        CtSensor ct(0);
        sensing::CtTrip trip;
        const sensing::CtTrip::Tick t = step(trip, ct, 76);
        ok("a breadboard-grade floor is REFUSED", t.floorFault);
        ok("and no floor is learnt", !trip.floorLearnt());
        ok("and it says so", trip.floorFaulted());
        ok("the sample is still reported, not swallowed", t.sampled);
        // Refusing to guess is the safe answer: a board that cannot measure its
        // own quiet must not decide that a tool is running.
        ok("and the verdict is OFF, not a guess", !t.on);
        ok("with no trip point at all", t.trip == 0.0f);
    }

    // IT RETRIES RATHER THAN LATCHING, which is what makes a tool that happens
    // to be running at boot a transient rather than a session-long fault.
    {
        ardushim::reset();
        CtSensor ct(0);
        sensing::CtTrip trip;
        ok("boots faulted", step(trip, ct, 90).floorFault);
        const sensing::CtTrip::Tick good = step(trip, ct, 7);
        ok("and recovers the moment the clamp goes quiet", !good.floorFault);
        ok("learning the real floor", trip.floorLearnt() && trip.floorCounts() < 7.5f);
        ok("and clearing the fault flag", !trip.floorFaulted());
    }

    // The limit is a boundary, so test the boundary rather than the middle.
    {
        ardushim::reset();
        CtSensor ct(0);
        sensing::CtTrip trip;
        ok("just under the limit is accepted",
           !step(trip, ct, (int)sensing::kMaxFloorCounts - 1).floorFault);
    }
    {
        ardushim::reset();
        CtSensor ct(0);
        sensing::CtTrip trip;
        ok("just over the limit is refused",
           step(trip, ct, (int)sensing::kMaxFloorCounts + 1).floorFault);
    }

    // ── HYSTERESIS: two points, not one ─────────────────────────────────────
    //
    // Before 2026-09-17 this was a bare `>`, so a tool sitting near its trip
    // point reported a STREAM of states — and every flip is a SENSE frame the
    // primary reads as a tool starting or stopping, which is a gate move.
    {
        ardushim::reset();
        CtSensor ct(0);
        sensing::CtTrip trip;
        step(trip, ct, 7);                       // floor = 7, trip = 28
        const float release = 28.0f * sensing::kClearRatio;   // 21

        ok("below the trip point, off",        !step(trip, ct, 20).on);
        ok("above it, on",                      step(trip, ct, 40).on);
        // THE WHOLE POINT: 24 is below the trip point but above the release, so
        // a single-threshold implementation would have dropped out here.
        ok("back between the two points, STILL ON", step(trip, ct, 24).on);
        ok("...and that is genuinely between them",
           24.0f < 28.0f && 24.0f > release);
        ok("below the release point, off",     !step(trip, ct, 18).on);
        // Asymmetry is only useful if it does not also make a real switch-off
        // slow. A tool actually stopping falls to the floor, not to 79%.
        ok("a real switch-off clears it at once", !step(trip, ct, 7).on);
    }

    // ── TripParams: the primary's numbers beat the board's ──────────────────
    //
    // This is what makes a node's firmware finished — retuning a shop is a
    // primary reflash and nobody climbs to a node.
    {
        ardushim::reset();
        CtSensor ct(0);
        sensing::CtTrip trip;
        sensing::TripParams p;
        p.tripRatio = 10.0f;                     // where the default is 4
        step(trip, ct, 7, p);                    // floor 7 -> trip 70, not 28
        ok("a supplied ratio replaces the default",
           step(trip, ct, 40, p).trip > 69.0f);
        ok("and changes the verdict with it", !step(trip, ct, 40, p).on);
        ok("the same reading trips on the DEFAULT params",  step(trip, ct, 40).on);
    }
    {
        ardushim::reset();
        CtSensor ct(0);
        sensing::CtTrip trip;
        sensing::TripParams p;
        p.minCounts = 200.0f;                    // absurd, but legal at the wire
        step(trip, ct, 7, p);
        ok("a supplied guard can outrank the ratio",
           step(trip, ct, 100, p).trip > 199.0f);
    }
    {
        ardushim::reset();
        CtSensor ct(0);
        sensing::CtTrip trip;
        sensing::TripParams p;
        p.clearRatio = 0.5f;                     // a wider band than the default
        step(trip, ct, 7, p);                    // trip 28, release 14
        ok("on", step(trip, ct, 40, p).on);
        ok("a wider release band holds through a deeper dip",
           step(trip, ct, 16, p).on);
        ok("and still lets go below it", !step(trip, ct, 13, p).on);
    }

    // ── the guards that stop a reading being taken at all ───────────────────
    {
        ardushim::reset();
        CtSensor ct(0);
        sensing::CtTrip trip;
        // A gate moving on this board: sampling busy-waits, which makes a sweep
        // jerk, and the answer is worthless anyway — if this board is moving a
        // gate the collector is already running.
        ardushim::amplitude() = 40;
        ardushim::clockMs() += sensing::kSampleIntervalMs + 1;
        ok("nothing is sampled while an actuator is moving",
           !trip.update(ct, ardushim::clockMs(), true).sampled);
        ok("and no floor was learnt from it", !trip.floorLearnt());
    }
    {
        ardushim::reset();
        CtSensor ct(0);
        sensing::CtTrip trip;
        step(trip, ct, 7);
        const uint32_t now = ardushim::clockMs();
        // The cadence gate: read() busy-waits for its whole window, so sampling
        // every loop() pass would hand a quarter of the board to the ADC.
        ok("a second look inside the interval does no work",
           !trip.update(ct, now, false).sampled);
    }

    printf("\n%d passed, %d failed\n\n", passed, failed);
    return failed ? 1 : 0;
}

// =============================================================================
// test_collector_plug.cpp — host tests for control/CollectorPlugState.h.
//
// HALF OF AN ANTI-DRIFT PAIR, and the newest one. The partner is
// shared/device-model/collector-plug.test.js, and this asserts THE SAME CASES
// IN THE SAME ORDER against the C++ engine — the nodelink.test.js ↔
// test_nodebus.cpp arrangement, not the looser collector-plug/bin-sensor one.
//
// It could not be a pair until 2026-09-10, because the judgement existed only
// in JS: the firmware reported the plug's facts and a browser decided what they
// meant. That stopped being sufficient when the way we COMMAND a blower became
// stateless — a servo pressing a fob, or an RF frame, sends an edge and learns
// nothing (docs/tool-sensing-rfc.md §4.2a/§4.2b) — because a browser nobody has
// open cannot be the only thing that notices a blower failed to start.
//
// So kCollectorRunningW and kCollectorSpinupGraceMs must equal
// COLLECTOR_RUNNING_W and COLLECTOR_SPINUP_GRACE_MS in topology-device.js.
// CHANGE ONE, CHANGE BOTH — the numbers are asserted literally below so a
// one-sided edit fails here rather than on a bench.
//
// Build + run via tools/ script `firmware:collectorplug:test`.
// =============================================================================

#include "../control/CollectorPlugState.h"
#include <cstdio>
#include <string>

static int passed = 0, failed = 0;
static void ok(const char* what, bool cond, const std::string& got = "") {
    if (cond) { printf("  ok   %s\n", what); passed++; }
    else      { printf("  FAIL %s%s%s\n", what,
                       got.empty() ? "" : "  got: ", got.c_str()); failed++; }
}

using namespace topo;

// Mirrors `st` in collector-plug.test.js. `known` false is that file's
// `undefined` plug; haveOnFor mirrors JS's "is onForMs a number".
static std::string st(bool known, bool reachable, float watts,
                      uint32_t onForMs, bool haveOnFor, bool commandedOn) {
    return plugStateName(collectorPlugState(known, reachable, watts,
                                            onForMs, haveOnFor, commandedOn));
}

int main() {
    const uint32_t GRACE = kCollectorSpinupGraceMs;
    const float    RUN   = kCollectorRunningW;

    printf("\nP0 the constants themselves — the pair, asserted literally\n");
    {
        // Written out rather than derived, so a change on one side of the pair
        // fails HERE. Deriving them from the header would make this test agree
        // with any value at all, which is the opposite of the point.
        ok("kCollectorRunningW is 50", RUN == 50.0f,
           std::to_string(RUN));
        ok("kCollectorSpinupGraceMs is 4000", GRACE == 4000u,
           std::to_string(GRACE));
    }

    printf("\nP1 nothing to judge\n");
    {
        ok("no plug and not asking -> noplug",
           st(false, false, 0, 0, false, false) == "noplug");
        ok("no plug but asking -> unknown",
           st(false, false, 0, 0, false, true) == "unknown");
        ok("plug not answering -> unknown",
           st(true, false, 0, 99999, true, true) == "unknown");
        ok("not asking -> off",
           st(true, true, 0, 0, true, false) == "off");
    }

    printf("\nP2 drawing current settles it, grace or no grace\n");
    {
        ok("drawing running current -> running",
           st(true, true, RUN, 0, true, true) == "running");
        ok("drawing far more is still running",
           st(true, true, RUN * 10, GRACE * 3, true, true) == "running");
        // The grace gates the ACCUSATION, never the good news: a blower that is
        // demonstrably drawing is running, whether or not it did so promptly.
        ok("good news is not held back by the grace",
           st(true, true, RUN, 1, true, true) == "running");
    }

    printf("\nP3 the spin-up grace\n");
    {
        ok("just switched on, nothing yet -> starting",
           st(true, true, 0, 0, true, true) == "starting");
        ok("one tick before the grace expires -> still starting",
           st(true, true, 0, GRACE - 1, true, true) == "starting");
        ok("the moment the grace expires -> notStarting",
           st(true, true, 0, GRACE, true, true) == "notStarting");
        ok("long past it, still nothing -> notStarting",
           st(true, true, 0, GRACE * 5, true, true) == "notStarting");
    }

    printf("\nP4 the edges that would produce a false alarm\n");
    {
        // Below the threshold is not running — a blower drawing a trickle is
        // exactly the stalled/failed case this exists to catch.
        ok("a trickle below threshold is not running",
           st(true, true, RUN - 1, GRACE, true, true) == "notStarting");

        // NO AGE MUST NEVER BECOME AN ACCUSATION. A device that reports no
        // onForMs cannot say how long it has been on, and guessing "long
        // enough" turns every just-started blower into an alarm. JS spells this
        // as an absent number; C++ needs the explicit flag, because a bare
        // uint32_t cannot tell absent from zero.
        ok("no onForMs -> starting, never an alarm",
           st(true, true, 0, 0, false, true) == "starting");
        ok("...even with a large age sitting in the field",
           st(true, true, 0, GRACE * 99, false, true) == "starting");

        // An unreachable plug outranks everything below it: we do not know, and
        // not knowing is not an accusation either.
        ok("an unreachable plug never says notStarting",
           st(true, false, 0, GRACE * 5, true, true) == "unknown");
    }

    printf("\n%d passed, %d failed\n\n", passed, failed);
    return failed ? 1 : 0;
}

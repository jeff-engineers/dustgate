// =============================================================================
// test_faults.cpp — host test for FaultPolicy.h.
//
// The whole point of lifting this out of firmware.ino is that the sketch cannot
// be tested and this table can. Every case below is a board someone has actually
// had on a desk: stepper power off, endstops unplugged, AP not up yet, a
// servo-only node with no rack at all.
//
// The case that motivated the split is `outlets alone`: one latched boolean
// meant a WiFi failure refused to home a rack it shares no wire with.
//
// Build + run:
//   c++ -std=c++17 firmware/test/test_faults.cpp -o /tmp/faulttest && /tmp/faulttest
// (or the tools/ script `firmware:faults:test`)
// =============================================================================
#include <cstdio>
#include "../control/FaultPolicy.h"

static int passed = 0, failed = 0;

static void ok(const char* what, bool cond) {
    if (cond) { printf("  ✓ %s\n", what); passed++; }
    else      { printf("  ✗ %s\n", what); failed++; }
}

// Reads as the sentence the policy is meant to enforce.
static faults::Policy on(bool motor, bool endstops, bool outlets, bool rackFitted = true) {
    faults::Stages s;
    s.motor = motor; s.endstops = endstops; s.outlets = outlets;
    return faults::decide(s, rackFitted);
}

int main() {
    printf("\n== FaultPolicy ==\n");

    {
        auto p = on(false, false, false);
        ok("a healthy board refuses nothing", !p.linearMotion);
        ok("...and is not in an error state",  !p.errorState);
    }

    // ── the case this exists for ──────────────────────────────────────────
    {
        auto p = on(/*motor*/false, /*endstops*/false, /*outlets*/true);
        ok("OUTLETS alone: linear motion still allowed", !p.linearMotion);
        ok("OUTLETS alone: not an error state",          !p.errorState);
    }

    // ── the two that genuinely stop the rack ──────────────────────────────
    {
        auto p = on(true, false, false);
        ok("MOTOR fault refuses linear motion", p.linearMotion);
        ok("MOTOR fault is an error state",     p.errorState);
    }
    {
        // There IS a motor. Homing it without a reference is how a carriage gets
        // driven into its own end, so this refusal is about safety, not absence.
        auto p = on(false, true, false);
        ok("ENDSTOP fault refuses linear motion", p.linearMotion);
        ok("ENDSTOP fault is an error state",     p.errorState);
    }

    // ── a build with no rack ──────────────────────────────────────────────
    {
        auto p = on(false, false, false, /*rackFitted*/false);
        ok("no rack: linear motion refused (by build)", p.linearMotion);
        ok("no rack: NOT an error state — nothing is broken", !p.errorState);
    }
    {
        // A servo-only node reports a missing TMC2209 every boot. That is the
        // build working as designed and must not paint the board red.
        auto p = on(/*motor*/true, false, false, /*rackFitted*/false);
        ok("no rack: a missing driver is not news", !p.errorState);
        ok("no rack: motion stays refused anyway",   p.linearMotion);
    }
    {
        // ...but the endstops on a rackless board are still real hardware, and a
        // failure there is still a failure.
        auto p = on(false, /*endstops*/true, false, /*rackFitted*/false);
        ok("no rack: an ENDSTOP failure is still an error", p.errorState);
    }

    // ── servo gates are refused by nothing at all ─────────────────────────
    // Stated as a test so it stays a decision. A shop whose stepper died still
    // opens every ball valve it has, and on a bench board stepper power comes
    // and goes far more often than the shop is actually broken.
    ok("servo gates: never gated", faults::Policy::servoGates == false);
    {
        auto p = on(true, true, true);
        ok("everything failed: linear motion refused", p.linearMotion);
        ok("everything failed: error state",           p.errorState);
        ok("everything failed: servo gates STILL run", faults::Policy::servoGates == false);
        (void)p;
    }

    printf("\n%d/%d passed\n", passed, passed + failed);
    return failed == 0 ? 0 : 1;
}

// =============================================================================
// test_collector_press.cpp — host tests for control/CollectorPress.h.
//
// NOT half of a pair, and saying so is part of the job (CLAUDE.md). Nothing in
// shared/device-model/ presses a button: the mock and the demo STAGE a
// collector's state directly, the way they stage a plug fault, so this policy
// exists once and has nothing to drift against. The same shape as kBinDebounceMs.
//
// What it guards is the difference between a closed loop and a machine that
// switches a motor on and off every few seconds. A press is an EDGE against a
// TOGGLE, so every mistake here is destructive rather than merely wrong:
//
//   • pressing inside the spin-up grace turns a STARTING blower off
//   • pressing because the sensor went unreachable turns a RUNNING blower off
//   • never giving up hides a tripped breaker and cycles a contactor all day
//
// Each of those has a test below, and they are the reason this file exists.
//
// Build + run via tools/ script `firmware:collectorpress:test`.
// =============================================================================

#include "../control/CollectorPress.h"
#include <cstdio>
#include <string>

static int passed = 0, failed = 0;
static void ok(const char* what, bool cond, const std::string& got = "") {
    if (cond) { printf("  ok   %s\n", what); passed++; }
    else      { printf("  FAIL %s%s%s\n", what,
                       got.empty() ? "" : "  got: ", got.c_str()); failed++; }
}

using namespace topo;

static const char* actName(PressAction a) {
    switch (a) {
        case PressAction::Nothing: return "nothing";
        case PressAction::Press:   return "press";
        case PressAction::GiveUp:  return "giveup";
    }
    return "?";
}

int main() {
    const uint32_t COOL  = kPressCooldownMs;
    const uint32_t GRACE = kCollectorSpinupGraceMs;

    printf("\nR0 the constants, and the one that is a safety property\n");
    {
        // A press inside the grace turns a starting blower OFF. The
        // static_assert in the header catches this at compile time; this catches
        // anyone who deletes the static_assert.
        ok("the cooldown outlasts the spin-up grace", COOL > GRACE,
           std::to_string(COOL) + " vs " + std::to_string(GRACE));
        ok("three presses in total, so two retries",
           kMaxPressAttempts == 3 && kMaxPressRetries == 2);
    }

    printf("\nR1 a first command presses once\n");
    {
        PressState st;
        ok("wanting it on, nothing pressed yet -> press",
           nextPressAction(st, true, PlugState::Off, 1000) == PressAction::Press);
        notePress(st, true, 1000);
        ok("...and not again inside the cooldown",
           nextPressAction(st, true, PlugState::Off, 1000 + COOL - 1) == PressAction::Nothing);
    }

    printf("\nR2 agreement ends it\n");
    {
        PressState st;
        notePress(st, true, 1000);
        ok("it came on -> nothing more",
           nextPressAction(st, true, PlugState::Running, 1000 + COOL) == PressAction::Nothing);

        PressState off;
        notePress(off, false, 1000);
        ok("asked off and it is off -> nothing more",
           nextPressAction(off, false, PlugState::Off, 1000 + COOL) == PressAction::Nothing);
    }

    printf("\nR3 the grace is not disagreement\n");
    {
        // The single most destructive mistake available here: a slow blower is
        // STARTING, not failing, and pressing again would switch it off.
        PressState st;
        notePress(st, true, 1000);
        ok("still starting, past the cooldown -> WAIT, do not press",
           nextPressAction(st, true, PlugState::Starting, 1000 + COOL) == PressAction::Nothing,
           actName(nextPressAction(st, true, PlugState::Starting, 1000 + COOL)));
        ok("...still waiting much later, while it is still starting",
           nextPressAction(st, true, PlugState::Starting, 1000 + COOL * 9) == PressAction::Nothing);
    }

    printf("\nR4 going blind is not disagreement either\n");
    {
        // A WiFi dropout must never switch a running collector off.
        PressState st;
        notePress(st, true, 1000);
        ok("sensor unreachable -> do nothing",
           nextPressAction(st, true, PlugState::Unknown, 1000 + COOL) == PressAction::Nothing,
           actName(nextPressAction(st, true, PlugState::Unknown, 1000 + COOL)));
        ok("...however long it stays unreachable",
           nextPressAction(st, true, PlugState::Unknown, 1000 + COOL * 50) == PressAction::Nothing);
    }

    printf("\nR5 a real failure is retried, then given up on\n");
    {
        PressState st;
        uint32_t t = 1000;
        ok("press 1", nextPressAction(st, true, PlugState::Off, t) == PressAction::Press);
        notePress(st, true, t);

        for (int retry = 1; retry <= kMaxPressRetries; retry++) {
            t += COOL;
            PressAction a = nextPressAction(st, true, PlugState::NotStarting, t);
            ok((std::string("retry ") + std::to_string(retry)).c_str(),
               a == PressAction::Press, actName(a));
            notePress(st, true, t);
        }

        t += COOL;
        PressAction a = nextPressAction(st, true, PlugState::NotStarting, t);
        ok("out of retries -> give up", a == PressAction::GiveUp, actName(a));
        noteGaveUp(st);
        ok("...and it stays given up rather than resuming",
           nextPressAction(st, true, PlugState::NotStarting, t + COOL * 10) == PressAction::GiveUp);
    }

    printf("\nR6 an inverted belief corrects itself in two presses\n");
    {
        // Our model says off, the blower is really on — a missed press, or a
        // person used the fob. Commanding ON presses once and turns it OFF; the
        // next verdict is notStarting, we press again, and it comes on. No
        // special handling anywhere: the DRAW is what settles it.
        PressState st;
        uint32_t t = 1000;
        notePress(st, true, t);                 // press 1: actually turned it off
        t += COOL;
        ok("the reading disagrees -> press again",
           nextPressAction(st, true, PlugState::NotStarting, t) == PressAction::Press);
        notePress(st, true, t);                 // press 2: on
        t += COOL;
        ok("now it agrees -> stop",
           nextPressAction(st, true, PlugState::Running, t) == PressAction::Nothing);
    }

    printf("\nR7 a change of intent gets a fresh start\n");
    {
        // Someone switching the blower off after we failed to start it must not
        // inherit that failure — the new intent has not been tried.
        PressState st;
        uint32_t t = 1000;
        notePress(st, true, t);
        for (int i = 0; i < kMaxPressRetries; i++) { t += COOL; notePress(st, true, t); }
        t += COOL;
        ok("we have given up on ON", nextPressAction(st, true, PlugState::NotStarting, t)
                                     == PressAction::GiveUp);
        noteGaveUp(st);
        ok("but asking for OFF presses immediately",
           nextPressAction(st, false, PlugState::NotStarting, t) == PressAction::Press,
           actName(nextPressAction(st, false, PlugState::NotStarting, t)));
        notePress(st, false, t);
        ok("...on a fresh budget", st.attempts == 1 && !st.gaveUp);
        // ...and it does not honour the old cooldown either, because the press
        // that mattered was for a different intent.
    }

    printf("\nR8 no feedback at all: one press, never a retry\n");
    {
        // An open-loop collector has nothing to reconcile against, so retrying
        // would be guessing — and a guess against a toggle is a coin flip that
        // may switch off a blower that is running perfectly well.
        PressState st;
        ok("first command still presses",
           nextPressAction(st, true, PlugState::NoPlug, 1000) == PressAction::Press);
        notePress(st, true, 1000);
        ok("but never a second time on its own",
           nextPressAction(st, true, PlugState::NoPlug, 1000 + COOL * 20) == PressAction::Nothing,
           actName(nextPressAction(st, true, PlugState::NoPlug, 1000 + COOL * 20)));
        ok("a change of intent still presses",
           nextPressAction(st, false, PlugState::NoPlug, 1000 + COOL) == PressAction::Press);
    }

    printf("\nR9 settling clears the budget\n");
    {
        PressState st;
        uint32_t t = 1000;
        notePress(st, true, t);
        t += COOL; notePress(st, true, t);
        ok("two presses spent", st.attempts == 2);
        noteSettled(st);
        ok("agreement resets the budget", st.attempts == 0 && !st.gaveUp);
    }

    printf("\n%d passed, %d failed\n\n", passed, failed);
    return failed ? 1 : 0;
}

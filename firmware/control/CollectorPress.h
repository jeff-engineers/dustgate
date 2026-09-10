// =============================================================================
// CollectorPress.h — pressing a stateless button until the blower agrees.
//
// TWO THINGS: a seam for whatever does the pressing, and the pure policy that
// decides WHEN to press. The policy is the dangerous half and the only half
// worth testing, so it has no hardware in it at all.
//
// WHY A "PRESS" AND NOT A "SWITCH". A Shelly is commanded: setSwitch(true)
// means on, and asking twice is harmless. The collector is not, any more. It is
// operated by pressing the button on its own remote — with a servo, or by
// injecting an RF frame — and that button on the Rockler fob is a TOGGLE
// (docs/tool-sensing-rfc.md §4.2b). A press is an EDGE. It says "change", never
// "be on", so the firmware cannot know the blower's state from what it sent.
// Only the draw says, which is why CollectorPlugState.h had to become a pair.
//
// THE LOOP: command a state, press once, wait out the spin-up grace, read the
// draw, and press again if it disagrees. That is the whole closed loop, and
// every subtlety below is about not making it worse than the open-loop version.
//
// ── THE COOLDOWN IS A SAFETY PROPERTY, NOT A TUNING KNOB ────────────────────
//
// kPressCooldownMs MUST exceed kCollectorSpinupGraceMs, and there is a
// static_assert below because getting this wrong is silent and destructive: a
// second press that lands while the blower is still spinning up TURNS IT OFF.
// The system would then see it off, press again, and oscillate — driving a
// motor's contactor at a few seconds per cycle, which is how a starter gets
// destroyed. A slow blower must be allowed to finish starting before anyone is
// allowed to conclude it did not.
//
// ── AN INVERTED BELIEF SELF-CORRECTS, IN TWO PRESSES ────────────────────────
//
// If our model says OFF and the blower is really ON — a missed press, or a
// person pressed the fob themselves — then commanding ON presses once and
// turns it OFF. The next verdict says notStarting, we press again, and it comes
// on. Two presses and the truth wins, with no special handling. That is the
// property that makes this safe to run unattended, and it only holds because
// the verdict comes from the DRAW rather than from our own bookkeeping.
//
// ── GIVING UP IS PART OF THE DESIGN ─────────────────────────────────────────
//
// kMaxPressAttempts bounds it at three presses total. A blower that will not start is a tripped
// breaker, an unplugged cord, a dead fob battery, a servo out of alignment or a
// seized motor, and none of those are fixed by pressing again. Pressing forever
// hides the fault, wastes the fob's battery and — with a toggle — risks landing
// a press at exactly the wrong moment for the rest of the day. Stop, and say so.
// =============================================================================

#pragma once
#include <cstdint>
#include "CollectorPlugState.h"

namespace topo {

// How long after a press before its result may be judged, and another sent.
// MUST exceed the spin-up grace — see the header. The extra second is margin
// for the poll interval, so the verdict is read from a plug reading taken after
// the grace rather than one straddling it.
static const uint32_t kPressCooldownMs = kCollectorSpinupGraceMs + 1000;

static_assert(kPressCooldownMs > kCollectorSpinupGraceMs,
              "A press inside the spin-up grace turns a starting blower OFF, and "
              "the system then oscillates. See CollectorPress.h.");

// How many PRESSES IN TOTAL before we stop and call it a fault — the first plus
// its retries, not retries alone. Three (Jeff, 2026-09-10).
//
// Counted in total presses rather than retries because the two differ by one,
// and the difference is a whole extra press against a motor's contactor. Naming
// it for the thing that physically happens leaves nothing to interpret.
//
// Three is enough to ride out an inverted belief, which costs two presses on its
// own (see above) and still leaves one in hand; and few enough that a genuinely
// dead blower is reported in about fifteen seconds rather than left cycling.
static const uint8_t kMaxPressAttempts = 3;
static const uint8_t kMaxPressRetries  = kMaxPressAttempts - 1;

/** What the presser should do next. */
enum class PressAction : uint8_t {
    Nothing,   // agreed, or still waiting to find out
    Press,     // send one edge
    GiveUp,    // out of attempts — this is a fault, report it
};

/** Bookkeeping for one collector's button. Owned by the caller; pure. */
struct PressState {
    uint32_t lastPressMs = 0;
    uint8_t  attempts    = 0;
    bool     everPressed = false;
    bool     wanted      = false;   // the state the attempts are working toward
    bool     gaveUp      = false;
};

/**
 * Decide the next action. Pure — `nowMs` is passed in, never read.
 *
 * @param st        bookkeeping, mutated only by notePress()/noteSettled()
 * @param want      the state routing has decided this blower should be in
 * @param observed  what the plug says (CollectorPlugState.h)
 * @param nowMs     caller's clock
 */
inline PressAction nextPressAction(const PressState& st, bool want,
                                   PlugState observed, uint32_t nowMs) {
    // A WANT THAT CHANGED RESTARTS EVERYTHING, including a given-up attempt.
    // Someone switching the blower off after we failed to start it must not
    // inherit that failure — the new intent has not been tried yet.
    if (st.everPressed && st.wanted != want) return PressAction::Press;

    if (st.gaveUp) return PressAction::GiveUp;

    // UNKNOWN IS NOT DISAGREEMENT. An unreachable sensor means we cannot see the
    // blower, and pressing a button because we went blind is how a running
    // collector gets switched off by a WiFi dropout. Wait.
    if (observed == PlugState::Unknown) return PressAction::Nothing;

    // NoPlug: no feedback configured at all, so there is nothing to reconcile
    // against and retrying would be guessing. One press on a change of want
    // (handled above) is all an open-loop collector ever gets.
    if (observed == PlugState::NoPlug) {
        return st.everPressed ? PressAction::Nothing : PressAction::Press;
    }

    // Inside the cooldown, the previous press has not had time to show its
    // result. Judging now is exactly the oscillation the cooldown prevents.
    if (st.everPressed && (nowMs - st.lastPressMs) < kPressCooldownMs)
        return PressAction::Nothing;

    const bool agrees = want ? (observed == PlugState::Running)
                             : (observed == PlugState::Off);
    if (agrees) return PressAction::Nothing;

    // Still starting is not yet a disagreement — the grace exists precisely so
    // a slow blower is not accused. (Reachable here, so this is a real reading.)
    if (want && observed == PlugState::Starting) return PressAction::Nothing;

    if (st.attempts >= kMaxPressAttempts) return PressAction::GiveUp;
    return PressAction::Press;
}

/** Record that a press was sent. */
inline void notePress(PressState& st, bool want, uint32_t nowMs) {
    if (st.everPressed && st.wanted != want) {
        // New intent: a fresh budget, and any previous surrender is void.
        st.attempts = 0;
        st.gaveUp   = false;
    }
    st.wanted      = want;
    st.lastPressMs = nowMs;
    st.everPressed = true;
    if (st.attempts < 255) st.attempts++;
}

/** Record that the blower now agrees, so the next disagreement starts fresh. */
inline void noteSettled(PressState& st) {
    st.attempts = 0;
    st.gaveUp   = false;
}

/** Latch the surrender, so GiveUp is reported once and stays reported. */
inline void noteGaveUp(PressState& st) { st.gaveUp = true; }

// -----------------------------------------------------------------------------
// The seam: whatever physically sends the edge.
//
// Deliberately one method. A presser cannot be asked for a state, because it has
// no way to produce one — that asymmetry with SmartOutlet::setSwitch() is the
// honest shape of the thing, and hiding it behind a switch-like interface would
// invite exactly the open-loop assumption this file exists to prevent.
//
// Implementations, none of which exist yet: a servo on the collector node
// pressing the fob (§4.2a, the shipping answer), and an RF frame injected via
// HT12E (§4.2, proven on the bench and kept as a live option).
// -----------------------------------------------------------------------------
class CollectorPresser {
public:
    virtual ~CollectorPresser() {}

    // Send exactly one edge. Returns false if it could not be sent at all — a
    // dead node, a servo that never reported back — which is different from a
    // press that was sent and did nothing, and only the plug reading can tell
    // us about the second kind.
    virtual bool press() = 0;

    // For the UI and the logs: "servo", "rf".
    virtual const char* kind() const = 0;
};

}  // namespace topo

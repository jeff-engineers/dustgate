// =============================================================================
// control/CollectorDriver.h — one pass of the loop that holds a PRESSED collector to what routing wants.
//
// It lived inline in firmware.ino's loop(), wrapped in DEBUG_PRINTs and watchdog pets. The decisions
// are CollectorPress.h's (when to press, when to give up); this is the glue around them — read what the
// plug says, ask the policy, press, record it, and raise or clear the problems a person has to see —
// and it is exactly the glue a second shell would otherwise rewrite and get subtly different. PURE:
// the shell passes a clock value, a presser and a hooks object, nothing here touches a pin.
//
// The sketch's OTHER way to command a blower (a switchable plug) is a plain on/off and stays in the
// sketch; a layout has exactly one of the two (validateTopology refuses both).
// =============================================================================
#pragma once
#include <string>
#include "CollectorPress.h"
#include "TopologyRuntime.h"

namespace topo {

// What the shell lends the driver: a console and a watchdog. Both default to nothing.
class DriverHooks {
public:
    virtual ~DriverHooks() {}
    virtual void say(const std::string& /*line*/) {}
    virtual void pet() {}     // a press is ~500 ms of RMT on the ESP32, against a 10 s watchdog
};

// Run one pass for one system's collector. `want` is routing's decision (collectorOn), `now` the shell's
// clock. Returns true when a press was attempted this pass.
inline bool driveCollectorPress(TopologyRuntime& rt, const std::string& sys, CollectorPresser& presser,
                                PressState& ps, bool want, uint32_t now, DriverHooks& h) {
    const PlugState seen = rt.pressObservation(sys);
    bool pressed = false;
    switch (nextPressAction(ps, want, seen, now)) {
        case PressAction::Press: {
            h.pet();
            const bool sent = presser.press();
            notePress(ps, want, now);
            pressed = true;
            h.say(std::string("[RF] press #") + std::to_string((int)ps.attempts) + " wanting " + (want ? "ON" : "OFF") +
                  " (saw " + plugStateName(seen) + (sent ? ") -> sent" : ") -> TRANSMIT FAILED"));
            if (sent) rt.clearProblem("rf-send:" + sys);
            else rt.raiseProblem("rf-send:" + sys, "rf-send-failed", "bad", "system", sys,
                                 "The remote's transmitter could not send the press.", now);
            rt.clearProblem("rf-gave-up:" + sys);
            h.pet();
            break;
        }
        case PressAction::GiveUp:
            rt.raiseProblem("rf-gave-up:" + sys, "rf-gave-up", "bad", "system", sys,
                "Pressed the remote 3 times and the blower never agreed \xE2\x80\x94 check the breaker, the cord and the fob's battery.", now);
            if (!ps.gaveUp) {
                noteGaveUp(ps);
                // Said ONCE: the state latches, so this is not a line per loop for the rest of the day.
                h.say(std::string("[RF] Collector ") + sys + " did not respond after " + std::to_string((int)kMaxPressAttempts) +
                      " presses \xE2\x80\x94 check the breaker, the cord, and the fob's battery.");
            }
            break;
        case PressAction::Nothing:
            // OFF that did not take. The policy cannot see it (the plug state reads "off" whenever we are
            // not asking, whatever the wire says), so it never presses again and the blower runs on after
            // the last tool. We do not press blind here, but we do SAY so — and only for a blower WE started: one
            // a person started by hand is theirs, and "won't stop" accused the fan on the bench of a fault it
            // did not have (2026-10-06), then held the collector's switch against a manual start.
            if (!want && ps.weStarted && (now - ps.lastPressMs) > kPressCooldownOffMs + kPressCooldownMs && rt.collectorDrawing(sys)) {
                rt.raiseProblem("rf-wont-stop:" + sys, "collector-wont-stop", "bad", "system", sys,
                    "Told it to stop, but it is still drawing power \xE2\x80\x94 the remote may have missed the press, or someone started it by hand.", now);
            } else {
                rt.clearProblem("rf-wont-stop:" + sys);
            }
            // Settled: clear the budget so the next disagreement gets a full one, not the tail of this one.
            if (seen == (want ? PlugState::Running : PlugState::Off)) {
                noteSettled(ps);
                rt.clearProblem("rf-gave-up:" + sys);
            }
            break;
    }
    return pressed;
}

}  // namespace topo

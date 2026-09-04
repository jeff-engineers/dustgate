#pragma once
// =============================================================================
// WakeButton.h — one momentary button, whose entire job is the screen's power.
//
// VERIFIED on a XIAO C5 (D1/GPIO0, internal pull-up, momentary to GND) on
// 2026-08-22: a press lights the panel. The DevKitC's GPIO34 arrangement — input-
// only, no internal pull-up, external 10k to 3V3 — has NOT been pressed, and it
// is the one with a way to go wrong; so has the QT Py's MISO.
//
// The TOGGLE is newer than that test. Press-to-light is what ran on a board;
// press-again-to-blank was written afterwards and has never been pressed.
//
// The screen blanks itself after two minutes (statusscreen::awake() in
// StatusScreenModel.h) so a panel screwed to a wall doesn't burn a static layout
// into itself over a year of idling. That is right until you actually want to
// read it: the board is fine, nothing is changing, and therefore nothing wakes
// the glass. Walking to a phone to find out what a screen two feet away already
// knows is the failure this part exists to prevent.
//
// REQUIRED, not optional, on a board that carries a panel — and the #error below
// enforces it. This was a convenience while faults held the screen lit by
// themselves; since 2026-08-22 the sleep timer has no exceptions (a fault left
// lit all weekend is the burn-in the blanking exists to prevent), so a screen
// build with no button has states it can never show you again.
//
// TWO gestures as of 2026-08-22, and that is a reversal — see the decision log in
// docs/mockups/oled-status.html. A short press works the screen; a ONE-SECOND
// HOLD runs the servo self-test (motor/ServoSelfTest.h). The rejected version of
// this was "paging", a second gesture that changed what the SCREEN said; this is
// a second gesture that moves GATES, which is further over the line the original
// decision drew, not less. It is here anyway because the bug it exists to catch
// is invisible without it (a servo that answers exactly once per boot), and it
// carries the one confirmation a button can carry: it refuses outright while the
// collector is running, because sweeping the gates shut against a pulling
// collector is the dead-head everything else in this system is built to avoid.
//
// The short press therefore fires on RELEASE, not on the press edge. It has to:
// acting on the press edge would blank the screen at the start of every hold,
// just as you want to watch what the hold is doing.
//
// The screen TOGGLES on that short press: press to light it, press again to put it out
// rather than standing in front of a panel you have finished reading for the two
// minutes the timer takes. The timer is unchanged and still has the last word —
// walk away mid-read and it blanks itself as before — and a manual blank does not
// suppress the next event, so an alarm still lights the glass. The off half is
// statusscreen::toggle(); see blankedAt() in StatusScreenModel.h for why "off" is
// a backdated clock rather than a flag.
//
// Beyond that it is deliberately NOT a general input layer. There is no
// long-press, no double-tap, no menu — one edge, one call, and the sleep timer
// does the rest. A button that could change what the shop DOES would need every
// one of the confirmations the web UI has, and that is a different part.
//
// COMPILES OUT COMPLETELY without PIN_WAKE_BTN, the same seam PIN_PIXEL,
// HAS_LINEAR and PIN_OLED_* already use — which means only a board with nowhere
// to put a PANEL either, since the two are declared together in the board
// header. A blind board pays nothing.
//
// A board that HAS the pins but no hardware on them is fine and ordinary since
// 2026-08-22, when the screen stopped being a build flag: statusscreen::toggle()
// returns immediately when no panel answered at boot, so a floating button pin
// with nothing wired to it can do nothing but waste a few instructions.
//
// PULL-UP, and the one place it bites: the default is INPUT_PULLUP, which is
// what a plain momentary-to-GND wants. The DevKitC's only free pins are the
// input-only GPIO34/35/36/39, which have NO internal pull-up at all — Arduino
// accepts INPUT_PULLUP on them and silently gives you a floating pin that reads
// as random presses. That board defines WAKE_BTN_INPUT_MODE as plain INPUT and
// carries an external 10kΩ to 3V3 instead; see firmware/attic/linear/devkitc-wiring.md §5.
// =============================================================================

#include <Arduino.h>
#include "../config.h"
#include "StatusScreen.h"
#include "../motor/ServoSelfTest.h"

#if defined(PIN_OLED_SDA) && defined(PIN_OLED_SCL) && !defined(PIN_WAKE_BTN)
#error "A board with an OLED must define PIN_WAKE_BTN: the screen blanks after \
two minutes with no exceptions, so without a button there is no way to light it \
again. See WakeButton.h and firmware/wiring/<board>.md."
#endif

#if defined(PIN_WAKE_BTN)

namespace wakebutton {

#ifndef WAKE_BTN_INPUT_MODE
// Momentary to GND: idle reads HIGH, pressed reads LOW.
#define WAKE_BTN_INPUT_MODE INPUT_PULLUP
#endif

// Long enough to swallow the contact bounce of a cheap panel-mount switch,
// short enough that the screen lights while your finger is still on it.
// Calculated from the usual 1-5ms bounce figure, not measured on a part.
static const uint32_t kDebounceMs = 25;

// Hold this long for the servo self-test. A second is long enough that nobody
// reaches it by pressing the button normally, and short enough to not feel
// broken while you wait. It fires WHILE STILL HELD rather than on release, so
// the panel confirms the moment it takes — a gesture with no feedback until you
// let go feels like it didn't work.
static const uint32_t kLongPressMs = 1000;

// Asked at the moment of the hold, not cached: whether the collector is running
// right now is the whole basis of the refusal below. Null on a board that has no
// idea (a node) — which reads as "not running", correctly: a node owns no
// collector and cannot start one.
inline bool (*&_collectorRunningFn())() { static bool (*f)() = nullptr; return f; }

inline uint8_t&  _stable()     { static uint8_t v = HIGH; return v; }
inline uint8_t&  _lastRaw()    { static uint8_t v = HIGH; return v; }
inline uint32_t& _lastChange() { static uint32_t t = 0;  return t; }
inline uint32_t& _pressedAt()  { static uint32_t t = 0;  return t; }
inline bool&     _longFired()  { static bool b = false;  return b; }

/**
 * Tell the button how to find out whether the collector is running. Call from
 * setup() on a primary; leave unset on a node.
 */
inline void setCollectorQuery(bool (*fn)()) { _collectorRunningFn() = fn; }

// What the hold DOES, for a board where sweeping every servo is not the useful
// gesture. A slider node has no servos to sweep and no serial console to type
// into, so the hold is its only way to say "home yourself now" at the board —
// which is the manual half of the on-demand homing rule (node/dustgate_node.cpp).
//
// A hook rather than an #if because the button has no business knowing what kind
// of actuator is fitted; it knows the gesture, the board says what it means.
// Unset keeps the servo self-test, which is what every PWM board wants.
inline void (*&_holdActionFn())() { static void (*f)() = nullptr; return f; }
inline void setHoldAction(void (*fn)()) { _holdActionFn() = fn; }

inline void begin() {
    pinMode(PIN_WAKE_BTN, WAKE_BTN_INPUT_MODE);
    // Seed from the pin rather than assuming HIGH: a button held down through
    // reset would otherwise register as a press the moment loop() starts, and
    // "the screen lit by itself at boot" is a confusing thing to debug.
    _stable() = _lastRaw() = digitalRead(PIN_WAKE_BTN);
    _lastChange() = millis();
}

/**
 * Call every loop(). Polled, not an interrupt: nothing here is time-critical,
 * and an ISR on the DevKitC would be an ISR sharing a core with the stepper's
 * step timing for the sake of lighting a display.
 *
 * Acts on the PRESS edge (HIGH→LOW), so the screen has already changed state by
 * the time the button comes back up. Release does nothing, which is what keeps a
 * held button from flickering the panel.
 */
inline void _checkHold();

inline void update() {
    const uint8_t raw = digitalRead(PIN_WAKE_BTN);
    const uint32_t now = millis();

    _checkHold();   // before the edge logic: the hold has no edge to wait for

    if (raw != _lastRaw()) { _lastRaw() = raw; _lastChange() = now; return; }
    // Unsigned subtraction, so this is correct across the millis() rollover.
    if ((now - _lastChange()) < kDebounceMs) return;
    if (raw == _stable()) return;

    _stable() = raw;

    if (raw == LOW) {                 // pressed
        _pressedAt() = now;
        _longFired() = false;
        return;                       // the short action waits for the release
    }

    // released
    if (!_longFired()) statusscreen::toggle();
}

/**
 * The hold, checked separately from the edge detector above because it has to
 * fire while the button is still down — there is no edge at one second.
 */
inline void _checkHold() {
    if (_stable() != LOW || _longFired()) return;
    if ((millis() - _pressedAt()) < kLongPressMs) return;
    _longFired() = true;

    bool (*q)() = _collectorRunningFn();
    const bool collectorOn = q ? q() : false;

    // Light the panel either way: the whole point of doing this at the board is
    // that the board answers. A silent refusal is the failure mode this gesture
    // is meant to be the cure for.
    statusscreen::note();

    void (*action)() = _holdActionFn();
    if (action) { action(); return; }

    if (servoselftest::start(collectorOn)) {
        Serial.println(F("[SELFTEST] Sweeping every servo — button held."));
    } else {
        Serial.print(F("[SELFTEST] Refused: "));
        Serial.println(servoselftest::refusal() ? servoselftest::refusal() : "unavailable");
    }
}

} // namespace wakebutton

#else   // ---- no button fitted: both entry points compile to nothing ----

namespace wakebutton {
inline void begin()  {}
inline void update() {}
inline void setCollectorQuery(bool (*)()) {}
inline void setHoldAction(void (*)()) {}
} // namespace wakebutton

#endif  // PIN_WAKE_BTN

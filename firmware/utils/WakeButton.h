#pragma once
// =============================================================================
// WakeButton.h — one momentary button, whose entire job is to light the screen.
//
// UNVERIFIED: no button has been wired to any board. The screen it wakes has run
// on exactly one DevKitC (2026-08-21); this half of the pair has not.
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
// So this is deliberately NOT a general input layer. There is no long-press, no
// double-tap, no menu — one edge, one call to statusscreen::note(), and the
// existing sleep timer does the rest. A button that could change what the shop
// DOES would need every one of the confirmations the web UI has, and that is a
// different part.
//
// COMPILES OUT COMPLETELY without PIN_WAKE_BTN, the same seam PIN_PIXEL,
// HAS_LINEAR and PIN_OLED_* already use — which now means only a board with no
// PANEL either, since the two are required together. A blind board pays nothing.
//
// PULL-UP, and the one place it bites: the default is INPUT_PULLUP, which is
// what a plain momentary-to-GND wants. The DevKitC's only free pins are the
// input-only GPIO34/35/36/39, which have NO internal pull-up at all — Arduino
// accepts INPUT_PULLUP on them and silently gives you a floating pin that reads
// as random presses. That board defines WAKE_BTN_INPUT_MODE as plain INPUT and
// carries an external 10kΩ to 3V3 instead; see firmware/wiring/devkitc.md §5.
// =============================================================================

#include <Arduino.h>
#include "../config.h"
#include "StatusScreen.h"

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

inline uint8_t&  _stable()     { static uint8_t v = HIGH; return v; }
inline uint8_t&  _lastRaw()    { static uint8_t v = HIGH; return v; }
inline uint32_t& _lastChange() { static uint32_t t = 0;  return t; }

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
 * Wakes on the PRESS edge (HIGH→LOW), so the screen is already lit by the time
 * the button comes back up.
 */
inline void update() {
    const uint8_t raw = digitalRead(PIN_WAKE_BTN);
    const uint32_t now = millis();

    if (raw != _lastRaw()) { _lastRaw() = raw; _lastChange() = now; return; }
    // Unsigned subtraction, so this is correct across the millis() rollover.
    if ((now - _lastChange()) < kDebounceMs) return;
    if (raw == _stable()) return;

    _stable() = raw;
    if (raw == LOW) statusscreen::note();
}

} // namespace wakebutton

#else   // ---- no button fitted: both entry points compile to nothing ----

namespace wakebutton {
inline void begin()  {}
inline void update() {}
} // namespace wakebutton

#endif  // PIN_WAKE_BTN

// =============================================================================
// NodeStatusLed.h — the node's only user interface.
//
// A secondary is headless: no display, no web UI, no buttons, and (once it's
// screwed to a joist above a manifold) no USB cable either. The single NeoPixel
// is therefore the ONLY way to answer "is this thing working?" from across the
// shop, which makes it worth more than an afterthought.
//
// The colour vocabulary, chosen so the common failure is unmistakable:
//
//   RED     — fault. Something is wrong that the node can't fix itself.
//   ORANGE  — booted, but NOT on WiFi. Either still joining, or fallen off.
//   WHITE   — captive portal is up, waiting to be told which WiFi to join.
//             Blinking, and the only white in the set, because it is the one
//             state that cannot resolve without a human walking over.
//   BLUE    — on WiFi, but no primary has linked. Healthy but idle.
//   GREEN   — primary linked. This is the "everything works" colour.
//   PURPLE  — brief flash on each SET, so you can see commands landing while
//             standing at the gate rather than at the laptop.
//
// Green is reserved for fully-working ON PURPOSE. An earlier cut used dim green
// for "on WiFi" and bright green for "linked", which asks you to judge an
// absolute brightness with nothing to compare against — from across a shop, dim
// green and green are the same colour. Blue→green is a change anyone can read at
// a glance, and it keeps "green = good to go" honest.
//
// Why colour rather than blink codes: counting flashes is miserable at a
// distance and worse through sawdust. A woodworker glancing up should get the
// answer in one look — green good, orange thinking, red bad — which is the same
// language the UI already uses for gate state (see the UI notes on
// green = open).
//
// BRIGHTNESS is deliberately low (kDim/kBright below). A NeoPixel at full scale
// is genuinely painful indoors, wastes current on a 5V rail shared with servos,
// and washes out colour discrimination — which is the whole point here.
//
// Everything is a no-op on boards without a pixel (NODE_PIXEL_PIN undefined),
// so this header is safe to call unconditionally from the node firmware.
// =============================================================================
#pragma once

#include <Arduino.h>
#include "../config.h"

namespace nodeled {

// One state per thing you'd actually want to distinguish while standing in a
// workshop. Ordered roughly worst → best, which is also the order the boot
// sequence walks through.
enum Status {
    FAULT,        // red — pulsing
    BOOTING,      // orange — pre-WiFi
    PORTAL,       // white — blinking; captive portal waiting for credentials
    NO_WIFI,      // orange — was connected, isn't now (or never joined)
    ONLINE,       // blue — on WiFi, primary not connected
    LINKED        // green — primary connected over /nodelink
};

#ifdef NODE_PIXEL_PIN

// Low on purpose — see the brightness note in the header comment. These are
// the raw 0-255 channel values, not a percentage.
static const uint8_t kDim    = 12;
static const uint8_t kBright = 48;

inline void _raw(uint8_t r, uint8_t g, uint8_t b) {
    neopixelWrite(NODE_PIXEL_PIN, r, g, b);
}

// Current state, so blinking phases can be computed in update() without the
// caller having to re-assert the state every loop.
inline Status& _state() { static Status s = BOOTING; return s; }
inline unsigned long& _flashUntil() { static unsigned long t = 0; return t; }

inline void begin() {
#ifdef NODE_PIXEL_POWER_PIN
    // The S3's pixel is behind a power switch; the C3's is hardwired, which is
    // why this is guarded rather than assumed.
    pinMode(NODE_PIXEL_POWER_PIN, OUTPUT);
    digitalWrite(NODE_PIXEL_POWER_PIN, HIGH);
    delay(1);   // let the rail settle before clocking data in
#endif
    _raw(0, 0, 0);
}

inline void set(Status s) { _state() = s; }

// Flash on activity. Non-blocking: update() restores the underlying status colour
// once the window expires, so a burst of SETs can never leave the pixel stuck on
// the activity colour.
//
// 400ms, not the 120ms this started at. The point of this flash is to answer
// "did a command reach the board?" from across a shop, and 120ms against a
// bright green background was easy to miss entirely — which is the opposite of
// useful when the thing you're debugging is whether commands arrive at all. A
// held-open window means a burst of SETs reads as one continuous flash rather
// than a stutter, which is also the honest picture.
inline void flashActivity() { _flashUntil() = millis() + 400; }

// Held for as long as a move is actually in flight, so the pixel shows the SWEEP
// rather than just its trigger. Cleared when the servo settles.
inline bool& _moving() { static bool m = false; return m; }
inline void setMoving(bool m) { _moving() = m; }

// Call every loop(). Owns all the timing so nothing else has to.
inline void update() {
    const unsigned long now = millis();

    // ORANGE while a command is landing or a valve is sweeping — the loudest
    // signal on the board, and deliberately so: it is the answer to "is anything
    // reaching this node?", which is the first question every time a gate doesn't
    // move. It outranks the link colours because during a move it IS the news.
    if (_moving() || now < _flashUntil()) { _raw(kBright, kBright / 3, 0); return; }

    switch (_state()) {
        case FAULT:
            // Slow pulse rather than solid: a steady red can read as "power LED"
            // at a glance, and this needs to look wrong.
            _raw((now / 500) % 2 ? kBright : kDim, 0, 0);
            break;
        case BOOTING:  _raw(kDim,    kDim / 3, 0); break;              // dim orange
        case NO_WIFI:  _raw(kBright, kBright / 3, 0); break;           // orange
        case PORTAL: {
            // Blinking, because this state needs a human to act — an unattended
            // node should never sit here quietly.
            const uint8_t w = (now / 400) % 2 ? kBright : 0;
            _raw(w, w, w);
            break;
        }
        case ONLINE:   _raw(0, 0, kBright); break;                     // blue
        case LINKED:   _raw(0, kBright, 0); break;                     // green
    }
}

#else   // ---- board has no pixel: every entry point compiles to nothing ----

inline void begin() {}
inline void set(Status) {}
inline void flashActivity() {}
inline void update() {}

#endif  // NODE_PIXEL_PIN

} // namespace nodeled

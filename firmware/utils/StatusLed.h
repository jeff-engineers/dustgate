// =============================================================================
// StatusLed.h — the board's at-a-glance status indicator.
//
// Shared by BOTH programs. On a secondary it is the node's ONLY user interface:
// headless, no display, no web UI, no buttons, and (once it's screwed to a joist
// above a manifold) no USB cable either. On a primary it answers the same
// question from across the shop without walking to a phone — is it moving, is it
// stuck, is it waiting for WiFi.
//
// The colour vocabulary, chosen so the common failure is unmistakable:
//
//   RED     — fault. Something is wrong that the board can't fix itself.
//   ORANGE  — motion or activity. The loudest signal here, deliberately: it is
//             the answer to "is anything actually happening?", which is the
//             first question every time a gate doesn't move.
//   WHITE   — captive portal is up, waiting to be told which WiFi to join.
//             Blinking, and the only white in the set, because it is the one
//             state that cannot resolve without a human walking over.
//   BLUE    — on WiFi, but not ready to work. On a node: no primary has linked.
//             On a primary: no topology stored yet, so there is nothing to route.
//   GREEN   — ready. This is the "everything works" colour.
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
// Blink RATE is still used, but only ever as a second-order detail INSIDE a
// colour, never as the primary signal. Orange means "moving" whether it's
// steady, slow or fast; the rate only tells you which kind of moving. That's the
// rule the old primary blink codes broke — a bare white LED blinking at 250 vs
// 120 vs 100 ms asked you to time it with a stopwatch to tell homing from
// calibrating from a hard fault.
//
// BRIGHTNESS is deliberately low (kDim/kBright below). A NeoPixel at full scale
// is genuinely painful indoors, wastes current on a 5V rail shared with servos,
// and washes out colour discrimination — which is the whole point here.
//
// Boards without a pixel fall back to PIN_LED, where each colour degrades to a
// blink pattern (see the #else branch). Boards with neither compile every entry
// point to nothing, so this header is always safe to call unconditionally.
// =============================================================================
#pragma once

#include <Arduino.h>
#include "../config.h"

namespace statusled {

// One state per thing you'd actually want to distinguish while standing in a
// workshop. Ordered roughly worst → best, which is also the order the boot
// sequence walks through.
enum Status {
    FAULT,        // red — pulsing
    BOOTING,      // orange — pre-WiFi
    PORTAL,       // white — blinking; captive portal waiting for credentials
    NO_WIFI,      // orange — was connected, isn't now (or never joined)
    ONLINE,       // blue — on WiFi, not ready to work (node: unlinked;
                  //        primary: no topology stored)
    READY         // green — node: primary linked. primary: routing live.
};

// Motion overlays. These OUTRANK Status while active, because during a move the
// motion IS the news — a gate that isn't moving when you asked it to is the
// thing you're standing there trying to diagnose.
//
// All three are orange; the rate distinguishes them for anyone who cares to
// look closer, and nobody who doesn't is misled.
enum Motion {
    STILL,        // nothing in flight — Status shows through
    MOVING,       // solid orange   — a normal move or a servo sweep
    HOMING,       // slow blink     — seeking the home datum
    CALIBRATING   // fast blink     — reference sweep between the endstops
};

// ---------------------------------------------------------------------------
// Shared state. Kept in function-local statics so this stays header-only with
// no .cpp and no ODR trouble when both programs include it.
// ---------------------------------------------------------------------------
inline Status& _state()      { static Status s = BOOTING; return s; }
inline Motion& _motion()     { static Motion m = STILL;   return m; }
inline unsigned long& _flashUntil() { static unsigned long t = 0; return t; }

inline void set(Status s)      { _state()  = s; }
inline void setMotion(Motion m) { _motion() = m; }

// Back-compat with the node's original call: "a servo is sweeping" is just
// MOVING. Kept because it reads better at the call site than setMotion(MOVING).
inline void setMoving(bool m) { _motion() = m ? MOVING : STILL; }

// Flash on activity. Non-blocking: update() restores the underlying status colour
// once the window expires, so a burst of SETs can never leave the indicator stuck
// on the activity colour.
//
// 400ms, not the 120ms this started at. The point of this flash is to answer
// "did a command reach the board?" from across a shop, and 120ms against a
// bright green background was easy to miss entirely — which is the opposite of
// useful when the thing you're debugging is whether commands arrive at all. A
// held-open window means a burst of SETs reads as one continuous flash rather
// than a stutter, which is also the honest picture.
inline void flashActivity() { _flashUntil() = millis() + 400; }

#ifdef PIN_PIXEL

// Low on purpose — see the brightness note in the header comment. These are
// the raw 0-255 channel values, not a percentage.
static const uint8_t kDim    = 12;
static const uint8_t kBright = 48;

// The core's own RMT-driven pixel write, so a single status pixel costs no
// library dependency. Adafruit_NeoPixel would pull in a whole strip driver to
// set one LED.
//
// Core 3.x renamed neopixelWrite() to rgbLedWrite() and deprecated the old name;
// the four supported targets are still on 2.0.x, so both spellings have to work
// until the platform question in platformio.ini's xiao_c5 env is settled.
inline void _raw(uint8_t r, uint8_t g, uint8_t b) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    rgbLedWrite(PIN_PIXEL, r, g, b);
#else
    neopixelWrite(PIN_PIXEL, r, g, b);
#endif
}

inline void begin() {
#ifdef PIN_PIXEL_POWER
    // Some boards gate the pixel's rail behind a GPIO (the S2 Feather and the
    // QT Py S3 do; the C3's is hardwired) — which is why this is guarded rather
    // than assumed.
    pinMode(PIN_PIXEL_POWER, OUTPUT);
    digitalWrite(PIN_PIXEL_POWER, HIGH);
    delay(1);   // let the rail settle before clocking data in
#endif
    _raw(0, 0, 0);
}

// Call every loop(). Owns all the timing so nothing else has to.
inline void update() {
    const unsigned long now = millis();

    // Motion and activity outrank everything — see the Motion enum note.
    if (now < _flashUntil()) { _raw(kBright, kBright / 3, 0); return; }
    switch (_motion()) {
        case MOVING:      _raw(kBright, kBright / 3, 0); return;
        case HOMING:      { const uint8_t v = (now / 250) % 2 ? kBright : kDim;
                            _raw(v, v / 3, 0); return; }
        case CALIBRATING: { const uint8_t v = (now / 120) % 2 ? kBright : 0;
                            _raw(v, v / 3, 0); return; }
        case STILL:       break;
    }

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
            // board should never sit here quietly.
            const uint8_t w = (now / 400) % 2 ? kBright : 0;
            _raw(w, w, w);
            break;
        }
        case ONLINE:   _raw(0, 0, kBright); break;                     // blue
        case READY:    _raw(0, kBright, 0); break;                     // green
    }
}

#elif defined(PIN_LED)

// ---- Fallback: a single-colour LED. Colours degrade to blink patterns. ----
//
// Strictly worse — this is the ambiguity the pixel exists to remove — so it's a
// compatibility path for boards with no pixel wired, not a design goal. Fast =
// bad, slow = working on it, solid = ready, off = idle, so at least the two ends
// of the scale still read correctly from a distance.
inline void begin() {
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
}

inline void update() {
    const unsigned long now = millis();
    bool on;

    if (now < _flashUntil())          on = (now / 60)  % 2;   // activity: flicker
    else switch (_motion()) {
        case MOVING:      on = true;                    break;
        case HOMING:      on = (now / 250) % 2;         break;
        case CALIBRATING: on = (now / 120) % 2;         break;
        default:
            switch (_state()) {
                case FAULT:   on = (now / 100) % 2;     break;  // rapid = bad
                case PORTAL:  on = (now / 400) % 2;     break;
                case NO_WIFI: on = (now / 700) % 2;     break;
                case BOOTING: on = false;               break;
                case ONLINE:  on = (now / 1500) % 2;    break;  // slow heartbeat
                case READY:   on = true;                break;
            }
    }
    digitalWrite(PIN_LED, on ? HIGH : LOW);
}

#else   // ---- no indicator at all: every entry point compiles to nothing ----

inline void begin()  {}
inline void update() {}

#endif  // PIN_PIXEL / PIN_LED

} // namespace statusled

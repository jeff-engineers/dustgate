// =============================================================================
// StatusLed.h — the board's at-a-glance status indicator.
//
// Shared by BOTH programs. On a secondary it is the node's ONLY user interface:
// headless, no display, no web UI, no buttons, and (once it's screwed to a joist
// above a manifold) no USB cable either. On a primary it answers the same
// question from across the shop without walking to a phone — is it moving, is it
// stuck, is it waiting for WiFi.
//
// The colour vocabulary (D-86, 2026-10-20): ONE CATEGORY PER COLOUR, so a glance across the shop answers "do I need to do
// anything, and about what" — first match wins:
//
//   RED       — needs a person. PULSING bright/dim: this board has a fault. BLINKING fully off and on: a dust bin on a
//               system this board serves is full — every such board blinks, so the whole shop asks.
//   ORANGE    — busy, and it will pass on its own: starting up, a gate moving (steady), homing (slow), calibrating (fast).
//   BLUE      — cut off from the shop. SOLID: on WiFi but no brain. BLINKING: no WiFi. Either way the network, not this
//               board, wants looking at — and after the fail-closed delay its gates close (D-85).
//   WHITE     — BLINKING: the setup portal is up, waiting to be told which WiFi to join (needs a person at the board).
//               DIM, steady: fine and idle.
//   GREEN     — "I've got the suction": one of this board's gates is open and its collector runs (the brain's word, in
//               ALERT). Not a promise the tool in use has it — a manifold or a slider shares the board.
//
// It used to be green for "linked to the brain", which earned its place while the network was being debugged and says
// little once it works. Brightness is still never the signal: dim white idle and green suction differ by COLOUR.
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
// That reasoning is about a pixel ON the board, a hand's width from your face at
// the bench. A board header can raise PIXEL_GAIN (default 1) for a pixel read
// from further off.
//
// There used to be a second pin here, PIN_PIXEL_EXT at 4x gain, for a remote
// pixel read from across the shop. No board ever defined it, and on the XIAO it
// would cost a pad there is none of to spare, so it came out (2026-10-17). The
// across-the-shop job it was for — a bin-level flag at the collector, replacing
// the red strobe — is planned as pixels CHAINED off this one's DOUT, which costs
// no pad but needs a multi-pixel write in place of _write() below. Not built yet.
//
// The gain multiplies and saturate at 255, so a colour's RATIO survives — orange
// stays (v, v/3, 0) and doesn't drift toward yellow as it gets brighter.
//
// Boards without a pixel fall back to PIN_LED, where each colour degrades to a
// blink pattern (see the #else branch). Boards with neither compile every entry
// point to nothing, so this header is always safe to call unconditionally.
// =============================================================================
#pragma once

#include <Arduino.h>
#include "../config.h"
#include "StatusVocab.h"

namespace statusled {

// Status and Motion live in StatusVocab.h — pure, no Arduino.h — so the
// optional OLED status screen can mirror this exact vocabulary and still be
// host-testable. Nothing else moved; the colour meanings above are still the
// design, this file is still where the pixel is driven.

// ---------------------------------------------------------------------------
// Shared state. Kept in function-local statics so this stays header-only with
// no .cpp and no ODR trouble when both programs include it.
// ---------------------------------------------------------------------------
inline Status& _state()      { static Status s = BOOTING; return s; }
inline Motion& _motion()     { static Motion m = STILL;   return m; }
inline unsigned long& _flashUntil() { static unsigned long t = 0; return t; }

// A dust bin on a system this board serves is full (the ALERT frame from the primary, or the primary's own bin; 2026-10-10).
// Not a Status: it is about the SHOP, and this board's own state still stands underneath it. Shown as red blinking fully
// OFF and on, against FAULT's bright/dim pulse — the RFC's split ("binNearFull"): both are red because both need a human,
// and the two are told apart without timing anything, because one goes dark and the other never does.
inline bool& _binAlert()     { static bool b = false; return b; }
inline void setBinAlert(bool full) { _binAlert() = full; }
inline bool binAlert()       { return _binAlert(); }

// "I'VE GOT THE SUCTION" (D-86): one of this board's gates is open and its collector runs — the brain's word, in the ALERT
// frame. It is what a READY board shows: green with it, a dim white without (fine, idle). Green used to mean only "I can
// talk to the brain", which earned its place while the network was being debugged and says little across a shop.
inline bool& _suction()      { static bool b = false; return b; }
inline void setSuction(bool s) { _suction() = s; }
inline bool suction()        { return _suction(); }

// THE SHOP LIGHT (D-86): a strip chained off this board's pixel (its DOUT), driven as one colour the brain resolved. Every
// board drives kShopPixels of it, always (jeff, 2026-10-20): on a board with no strip the data clocks into nothing, and a
// shorter strip passes the rest off its end. Off (0,0,0) until the brain says otherwise.
static const uint8_t kShopPixels = 16;
struct ShopLook { uint8_t r = 0, g = 0, b = 0; bool blink = false; };
inline ShopLook& _shop()     { static ShopLook l; return l; }
inline void setShopLight(uint8_t r, uint8_t g, uint8_t b, bool blink) {
    ShopLook& l = _shop();
    l.r = r; l.g = g; l.b = b; l.blink = blink;
}
inline const ShopLook& shopLight() { return _shop(); }

inline void set(Status s)      { _state()  = s; }
inline void setMotion(Motion m) { _motion() = m; }

// Read back what the indicator is currently saying. The optional status screen
// spells out this exact state (StatusScreenModel.h) rather than deciding one of
// its own — two indicators that could disagree would be worse than one.
inline Status state()  { return _state();  }
inline Motion motion() { return _motion(); }

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

#if defined(PIN_PIXEL)

// Low on purpose — see the brightness note in the header comment. These are
// the raw 0-255 channel values, not a percentage, and they are what a gain of 1
// produces.
static const uint8_t kDim    = 12;
static const uint8_t kBright = 48;

// Brightness multiplier; a board header overrides it. 1 encodes "read at arm's
// length".
#ifndef PIXEL_GAIN
#define PIXEL_GAIN      1
#endif

// Saturating, so a gain can't wrap a channel back around to dark — the failure
// there would be an indicator that goes black exactly when it means FAULT.
inline uint8_t _scale(uint8_t v, uint16_t gain) {
    const uint16_t s = (uint16_t)v * gain;
    return s > 255 ? 255 : (uint8_t)s;
}

// The core's own RMT-driven pixel write, so a single status pixel costs no
// library dependency. Adafruit_NeoPixel would pull in a whole strip driver to
// set one LED.
//
// Core 3.x renamed neopixelWrite() to rgbLedWrite() and deprecated the old name;
// the four supported targets are still on 2.0.x, so both spellings have to work
// until the platform question in platformio.ini's xiao_c5 env is settled.
inline void _write(uint8_t pin, uint8_t r, uint8_t g, uint8_t b) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    rgbLedWrite(pin, r, g, b);
#else
    neopixelWrite(pin, r, g, b);
#endif
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
// The board pixel AND the shop light, in one RMT transaction (D-86): a chain must be clocked as one stream, and
// rgbLedWrite() only ever sends one pixel. WS2812 timing at the 10 MHz RMT clock rgbLedWrite itself uses: a 1 is 0.8 us
// high then 0.4 us low, a 0 the reverse; colours go out green, red, blue. 17 pixels take about half a millisecond, so the
// chain is written only when a colour CHANGES (a blink is a change), plus once a second in case a write was lost — not on
// every pass of loop(). NOT YET RUN ON HARDWARE: every board now writes through it.
inline void _writeChain(uint8_t pin, uint8_t r, uint8_t g, uint8_t b, uint8_t sr, uint8_t sg, uint8_t sb) {
    static bool ready = false;
    static uint8_t last[6] = {1, 1, 1, 1, 1, 1};   // never a real first value, so the first call writes
    static unsigned long lastMs = 0;
    const uint8_t now6[6] = {r, g, b, sr, sg, sb};
    const unsigned long now = millis();
    if (memcmp(now6, last, 6) == 0 && now - lastMs < 1000) return;
    if (!ready) { ready = rmtInit(pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000); if (!ready) return; }
    static rmt_data_t buf[24 * (1 + kShopPixels)];
    size_t i = 0;
    auto put = [&](uint8_t v) {
        for (int bit = 7; bit >= 0; bit--) {
            const bool one = (v >> bit) & 1;
            buf[i].level0 = 1; buf[i].duration0 = one ? 8 : 4;
            buf[i].level1 = 0; buf[i].duration1 = one ? 4 : 8;
            i++;
        }
    };
    put(g); put(r); put(b);
    for (uint8_t k = 0; k < kShopPixels; k++) { put(sg); put(sr); put(sb); }
    rmtWrite(pin, buf, i, RMT_WAIT_FOR_EVER);
    memcpy(last, now6, 6); lastMs = now;
}
#endif

// Drives the pixel at its gain, and the shop light behind it. Writing to a pad with nothing soldered to it is harmless —
// it clocks a few bytes into open air.
inline void _raw(uint8_t r, uint8_t g, uint8_t b) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    const ShopLook& shop = _shop();
    const bool lit = !shop.blink || (millis() / 500) % 2;
    _writeChain(PIN_PIXEL, _scale(r, PIXEL_GAIN), _scale(g, PIXEL_GAIN), _scale(b, PIXEL_GAIN),
                lit ? shop.r : 0, lit ? shop.g : 0, lit ? shop.b : 0);
#else
    _write(PIN_PIXEL, _scale(r, PIXEL_GAIN), _scale(g, PIXEL_GAIN),
           _scale(b, PIXEL_GAIN));
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

    // THE ORDER (D-86): a fault, then a full bin, then motion — the two reds need a person, and a gate moving does not.
    // A fault: slow pulse rather than solid, since a steady red can read as "power LED" at a glance.
    if (_state() == FAULT) { _raw((now / 500) % 2 ? kBright : kDim, 0, 0); return; }
    // A full bin, except where the board is not yet on the shop's network (booting, the setup portal, no WiFi) — there it
    // cannot have heard the alert, so a stale one must not show.
    if (_binAlert() && _state() != BOOTING && _state() != PORTAL && _state() != NO_WIFI) {
        _raw((now / 250) % 2 ? kBright : 0, 0, 0);
        return;
    }

    // Motion and activity next — see the Motion enum note.
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
        case NO_WIFI: {
            // Blue, BLINKING (D-86, 2026-10-20; it was orange): cut off from the shop, like ONLINE's solid blue (no brain),
            // and both mean the network rather than this board needs looking at. Orange is now only "busy, will pass".
            const uint8_t b = (now / 400) % 2 ? kBright : 0;
            _raw(0, 0, b);
            break;
        }
        case PORTAL: {
            // Blinking, because this state needs a human to act — an unattended
            // board should never sit here quietly.
            const uint8_t w = (now / 400) % 2 ? kBright : 0;
            _raw(w, w, w);
            break;
        }
        case LAYOUT_BAD: {
            // Blue, blinking. Same family as ONLINE because the board is fine
            // and on the network; blinking because, unlike ONLINE, this will
            // never clear on its own — someone has to fix the layout.
            const uint8_t b = (now / 400) % 2 ? kBright : 0;
            _raw(0, 0, b);
            break;
        }
        case ONLINE:   _raw(0, 0, kBright); break;                     // blue
        case READY:                                                    // green with the suction, dim white without (D-86)
            if (_suction()) _raw(0, kBright, 0);
            else            _raw(kDim / 3, kDim / 3, kDim / 3);
            break;
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
            if (_binAlert()) { on = (now / 250) % 2; break; }   // a full bin: a steady blink, slower than a fault
            switch (_state()) {
                case FAULT:   on = (now / 100) % 2;     break;  // rapid = bad
                case PORTAL:  on = (now / 400) % 2;     break;
                case NO_WIFI: on = (now / 700) % 2;     break;
                case LAYOUT_BAD: on = (now / 400) % 2;  break;
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

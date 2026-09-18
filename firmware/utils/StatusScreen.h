#pragma once
// =============================================================================
// StatusScreen.h — the optional OLED status screen, driven.
//
// WORKING, on exactly one board. As of 2026-08-21 this drives a 0.96" SSD1306 on
// a DevKitC (GPIO16 SDA / GPIO4 SCL, 0x3C) — the first display any DustGate board
// has driven. The timing numbers below are still calculations rather than
// measurements, and the sleep behaviour has not been watched over hours.
//
// The division of labour, and the reason this file is small:
//
//   StatusScreenModel.h  decides WHAT the screen says — pure, host-tested,
//                        59 assertions against the 21×8 budget.
//   StatusScreen.h       puts those characters on glass. I²C, a font, a sleep
//                        timer, and nothing else. No policy lives here.
//
// COMPILES OUT COMPLETELY when the board has nowhere to put a screen: without
// PIN_OLED_SDA / PIN_OLED_SCL every entry point below is an empty inline, the
// same seam PIN_PIXEL and HAS_LINEAR already use, and the LDF never even links
// the driver. That is now a property of the BOARD HEADER alone.
//
// DECLARED BY THE BOARD, PROBED AT BOOT — and that is a reversal, so it is worth
// saying why. The original answer (docs/mockups/oled-status.html) was to declare
// a screen in the build, matching how every other fitted-or-not part on these
// boards is decided: an env set -DHAS_STATUS_SCREEN and there were two envs per
// board. That went on 2026-08-22, when the carrier design became one that always
// has the panel and its button, which left the flag costing a silent failure —
// flash the env without the suffix and the screen and button are simply absent —
// to save ~20 KB nothing was short of. Per-board reasoning is in the note at the
// top of platformio.ini.
//
// So a board with the pins but NO PANEL is now the ordinary case, not a mistake:
// begin() reports the missing ACK, sets _present false, and every later call
// returns immediately. A missing panel must not hang the shop's brain in Wire's
// timeout on every pass of loop().
// =============================================================================

#include <Arduino.h>
#include "../config.h"
#include "StatusScreenModel.h"

#if defined(PIN_OLED_SDA) && defined(PIN_OLED_SCL)

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// The one exception to "no library for one indicator" (see StatusLed.h, which
// drives its pixel with the core's own RMT call rather than take a dependency).
// A framebuffer and a font are not a job worth re-writing badly, and this pair
// is only compiled into a build that has a panel to draw on.

namespace statusscreen {

#ifndef OLED_I2C_ADDR
// 0x3C on essentially every 4-pin 0.96" module. 0x3D exists on some 128×64
// parts; a board that scans up as 0x3D overrides this.
#define OLED_I2C_ADDR 0x3C
#endif

#ifndef OLED_I2C_HZ
// 400kHz puts a full 1KB refresh at roughly 23ms. Calculated, not measured.
#define OLED_I2C_HZ 400000
#endif

static const int kPanelW = 128;
static const int kPanelH = 64;
static const int kCellW  = 6;    // Adafruit_GFX built-in font: 5×7 in a 6×8 cell
static const int kCellH  = 8;

// Redraw cadence. A full refresh is ~23ms of blocking I²C and nothing on this
// screen changes faster than a person can read it, so 250ms costs ~9% of one
// core's time in the worst case and buys a blink that looks like a blink.
static const uint32_t kRedrawMs = 250;

// ---------------------------------------------------------------------------
// State, in function-local statics — header-only, no .cpp, no ODR trouble when
// both programs include it. Same pattern as StatusLed.h.
// ---------------------------------------------------------------------------
inline Adafruit_SSD1306& _oled() {
    static Adafruit_SSD1306 d(kPanelW, kPanelH, &Wire, -1);   // -1: no reset pin
    return d;
}
inline bool&     _present()    { static bool b = false; return b; }
inline bool&     _lit()        { static bool b = false; return b; }
inline uint32_t& _lastEvent()  { static uint32_t t = 0; return t; }
inline uint32_t& _lastDraw()   { static uint32_t t = 0; return t; }
inline uint32_t& _lastHash()   { static uint32_t h = 0; return h; }
// The hash BEFORE last. Two are kept so an A→B→A oscillation can be told from a
// sequence of genuine changes — see the flap check in update().
inline uint32_t& _prevHash()   { static uint32_t h = 0; return h; }

/**
 * Wake the screen. Call it for anything worth looking up at that the rendered
 * text wouldn't show on its own; update() already wakes on its own for
 * everything that changes what the screen SAYS.
 */
inline void note() { _lastEvent() = millis(); }

/**
 * The wake button's edge: light the panel if it is dark, put it out if it is
 * lit. Nothing else calls this — an EVENT never blanks a screen, so note() and
 * toggle() are deliberately not the same entry point.
 *
 * Reads _lit() rather than recomputing awake(), so the toggle turns off exactly
 * what the user can see. The two agree anyway (update() runs every loop and
 * settles _lit() from the clock); using the visible one means a press can never
 * appear to do nothing because the timer expired between draw and press.
 *
 * The off state is a backdated timestamp, not a flag — see blankedAt() in
 * StatusScreenModel.h for why, and for what it deliberately does NOT suppress.
 */
inline void toggle() {
    if (!_present()) return;
    if (_lit()) _lastEvent() = blankedAt(millis());
    else        note();
}

/** Whether a panel actually answered at begin(). False on every other board. */
inline bool present() { return _present(); }

/** Whether the glass is currently ON. For the button's log line, which has to
 *  tell "the press did nothing" apart from "the press blanked a lit panel". */
inline bool lit() { return _lit(); }

/**
 * Does anything answer at the screen's address? One zero-length write; an ACK
 * is the whole test.
 *
 * We have to ask this OURSELVES, because Adafruit_SSD1306::begin() does not.
 * Read its source before assuming otherwise: its only `return false` is a failed
 * malloc of the 1KB framebuffer. It sends the init sequence into open air and
 * reports success either way — so "begin() returned true" means "the heap had a
 * kilobyte", not "there is a display".
 *
 * That cost a bench cycle on 2026-08-21: `[SCREEN] SSD1306 up` on a board where
 * a scan of the same pins found nothing at all, which is not a contradiction
 * once you know what begin() actually promises.
 */
inline bool _probe() {
    Wire.beginTransmission((uint8_t)OLED_I2C_ADDR);
    return Wire.endTransmission() == 0;
}

inline bool begin() {
    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
    Wire.setClock(OLED_I2C_HZ);

    // Probe before init, so a board with no panel wired stops here instead of
    // spending the rest of its life clocking frames into open air.
    if (!_probe()) { _present() = false; return false; }

    // SWITCHCAPVCC: the module's own charge pump, which is how every 4-pin
    // board is built — it has no external panel supply to point at.
    //
    // periphBegin = FALSE, and this one is not optional on this hardware. Left
    // at its default the library calls wire->begin() with NO ARGUMENTS, which on
    // an ESP32 re-initialises I2C on the CORE DEFAULT pins — GPIO21/22. On the
    // DevKitC those are the TMC2209's EN and DIR. The board's whole pin choice
    // exists to keep I2C off them (see attic/linear/devkitc_wroom32.h), and a default
    // argument in a display library walks straight back into it: the display
    // talks to nothing while the stepper's enable line gets driven as a clock.
    _present() = _oled().begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR,
                               /*reset=*/true, /*periphBegin=*/false);
    if (!_present()) return false;
    _oled().clearDisplay();
    _oled().display();
    _lit() = true;
    note();
    return true;
}

// ---------------------------------------------------------------------------
// The signal glyph. Four bars in the 3×2-cell box the header band buys it —
// 18×16 device pixels, ~3.4 × 3.0mm on a 0.96" panel. It is the one thing on
// these screens that isn't in the GFX font, which is the whole reason it is
// drawn by hand here rather than spelled.
// ---------------------------------------------------------------------------
inline void _drawSignal(int x, int y, int bars, uint16_t colour) {
    const int h[4] = {5, 9, 13, 16};
    for (int i = 0; i < 4; i++) {
        const int bh = h[i];
        if (i < bars) _oled().fillRect(x + i * 5, y + 16 - bh, 3, bh, colour);
        else          _oled().drawRect(x + i * 5, y + 16 - bh, 3, bh, colour);
    }
}

inline void _drawText(int col, int row, const char* text, uint8_t size, uint16_t colour) {
    _oled().setTextSize(size);
    _oled().setTextColor(colour);
    _oled().setCursor(col * kCellW, row * kCellH);
    _oled().print(text);
}

inline void _draw(const Screen& s, uint32_t now) {
    _oled().clearDisplay();

    // -- header band, inverse video ---------------------------------------
    // A blinking band alternates between filled and plain rather than blanking
    // the row: the hostname stays readable through the blink, which matters
    // most in exactly the states that blink.
    const bool filled = !s.barBlink || ((now / 600) % 2) == 0;
    const int  bandH  = kBarRows * kCellH;
    if (filled) _oled().fillRect(0, 0, kPanelW, bandH, SSD1306_WHITE);
    const uint16_t barInk = filled ? SSD1306_BLACK : SSD1306_WHITE;

    // Vertically centred in the two-row band, one cell in from the left edge so
    // the inverse block has a margin rather than butting the first glyph. The
    // name stays size 1: size 2 would cap a hostname at 10 characters.
    _oled().setTextSize(1);
    _oled().setTextColor(barInk);
    _oled().setCursor(kCellW, (bandH - 7) / 2);
    _oled().print(s.bar);

    if (s.wifiBars >= 0)
        _drawSignal(kPanelW - 3 * kCellW, (bandH - 16) / 2, s.wifiBars, barInk);

    // -- the state word and the detail lines -------------------------------
    int row = kBarRows;
    for (int i = 0; i < s.lineCount; i++) {
        _drawText(0, row, s.lines[i].text, s.lines[i].size, SSD1306_WHITE);
        row += s.lines[i].size;
    }

    _oled().display();
}

/**
 * What counts as an EVENT, for the purpose of waking the screen.
 *
 * Deliberately NOT "the rendered text changed": half these screens carry an
 * age or a countdown ("last cmd 3s ago", "retrying in 12s") that ticks every
 * second, and a screen that woke for those would never sleep at all — which is
 * the burn-in this whole mechanism exists to avoid. Signal bars are excluded
 * for the same reason; RSSI wanders on its own.
 *
 * So: the state, the motion, what is running, and how much of the shop is
 * answering. Those are the things that mean something happened.
 */
// THE FIELDS THAT KEEP THE PANEL AWAKE, one sub-hash each.
//
// Split out of a single rolled-up hash on 2026-09-17 to answer a question that
// reading the code could not: "brains seem to be keeping the screen alive full
// time", and nothing in here is obviously time-varying. A change in ANY of these
// bumps _lastEvent(), so exactly one of them must be flapping — and a single
// hash can say THAT something changed while being useless about WHICH.
//
// The names are here so the log can say it out loud. Cheap: twelve uint32 and a
// static table of literals, computed on a path that already hashes all of this.
enum { kPartCount = 14 };
inline const char* const* _partNames() {
    static const char* n[kPartCount] = {
        "status", "motion", "role", "gates", "nodes", "collectorOn",
        "toolName", "openGate", "openingGate", "closingGate", "darkNode",
        "primaryHost", "layoutError", "bootFault",
    };
    return n;
}

inline void _stateParts(const Facts& f, uint32_t out[kPartCount]) {
    auto one = [](uint32_t v) { return (2166136261u ^ v) * 16777619u; };
    auto str = [&](const char* s) {
        uint32_t h = one(s ? 1u : 0u);
        for (const char* p = s; p && *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
        return h;
    };
    out[0]  = one((uint32_t)f.status);
    out[1]  = one((uint32_t)f.motion);
    out[2]  = one((uint32_t)f.role);
    out[3]  = one((uint32_t)(f.gatesReady + 1) * 31 + (uint32_t)(f.gatesTotal + 1));
    out[4]  = one((uint32_t)(f.nodesLinked + 1) * 31 + (uint32_t)(f.nodesTotal + 1));
    out[5]  = one(f.collectorOn ? 1u : 0u);
    out[6]  = str(f.toolName);
    out[7]  = str(f.openGate);
    out[8]  = str(f.openingGate);
    out[9]  = str(f.closingGate);
    out[10] = str(f.darkNode);
    out[11] = str(f.primaryHost);
    out[12] = str(f.layoutError);
    out[13] = str(f.bootFault);
}

inline uint32_t _stateHash(const Facts& f) {
    uint32_t parts[kPartCount];
    _stateParts(f, parts);
    uint32_t h = 2166136261u;
    for (int i = 0; i < kPartCount; i++) h = (h ^ parts[i]) * 16777619u;
    return h;
}

inline uint32_t* _lastParts() { static uint32_t p[kPartCount] = {0}; return p; }

/**
 * Say WHICH fact just re-lit the panel.
 *
 * Rate-limited to one line every two seconds, because the failure being chased
 * is a fact changing on every loop pass and an unthrottled line would be the
 * same bug in a different output. The throttle is per-BOARD, not per-field: the
 * question is "what is flapping", and one named field every two seconds answers
 * it in about ten.
 */
inline void _logWake(const Facts& f, bool flap) {
    uint32_t parts[kPartCount];
    _stateParts(f, parts);
    uint32_t* prev = _lastParts();
    static uint32_t lastLogMs = 0;
    static bool     seeded    = false;
    const uint32_t now = millis();

    if (seeded && (uint32_t)(now - lastLogMs) >= 2000) {
        bool any = false;
        for (int i = 0; i < kPartCount; i++) {
            if (parts[i] == prev[i]) continue;
            if (!any) {
                Serial.print(flap ? F("[SCREEN] rattling (not re-lit) —")
                                  : F("[SCREEN] awake — changed:"));
                any = true;
            }
            Serial.print(' '); Serial.print(_partNames()[i]);
        }
        if (any) { Serial.println(); lastLogMs = now; }
    }
    for (int i = 0; i < kPartCount; i++) prev[i] = parts[i];
    seeded = true;
}

/**
 * When THIS translation unit was compiled, in the short form the bottom row
 * takes. Formatted once and cached — __DATE__/__TIME__ are string literals, so
 * this costs one parse at first use and nothing after.
 *
 * It is the build time, not the flash time. A binary PlatformIO decided not to
 * rebuild keeps its original stamp, which is the honest answer to "what is
 * actually running" and occasionally a surprising one.
 */
inline const char* buildStamp() {
    static char s[24] = {0};
    static bool done = false;
    if (!done) { formatBuild(__DATE__, __TIME__, s, sizeof(s)); done = true; }
    return s;
}

/**
 * Call every loop() with the current facts. Owns all the timing, the same way
 * statusled::update() does, so no call site has to think about redraw rates or
 * the sleep timer.
 *
 * Fills in the build stamp when the caller left it null, so neither program has
 * to remember to — a call site that wants a different provenance string can
 * still set its own.
 */
inline void update(const Facts& in) {
    if (!_present()) return;
    Facts f = in;
    if (!f.buildStamp) f.buildStamp = buildStamp();
    const uint32_t now = millis();

    const uint32_t h = _stateHash(f);
    if (h != _lastHash()) {
        // AN OSCILLATION IS ONE CONDITION, NOT A STREAM OF EVENTS (2026-09-17).
        //
        // The panel is meant to light on a change and sleep two minutes later.
        // It was instead staying lit indefinitely on a shop where something
        // flaps — a node that connects and drops, a clamp hovering at its trip
        // point — because every flip counted as a fresh event and pushed the
        // timer out again. Nobody chose that: it falls out of "any hash change
        // bumps the clock", which is right for a sequence of real changes and
        // wrong for a state machine rattling between two values.
        //
        // Returning to the hash we held BEFORE the last one is exactly what an
        // A→B→A rattle looks like, and nothing else does. The first transition
        // still lights the panel — that is the report, and it is worth seeing —
        // and the rattle after it no longer keeps the glass on all night.
        const bool flap = (h == _prevHash());
        _prevHash() = _lastHash();
        _lastHash() = h;
        if (!flap) _lastEvent() = now;
        // Which fact did it? See _logWake.
        _logWake(f, flap);
    } else {
        // Keep the per-field snapshot current even when nothing changed, so the
        // first real change reports only the field that actually moved.
        _logWake(f, false);
    }

    // The sleep decision is statusscreen::awake() in the model — pure, and
    // host-tested, including the millis() rollover. It looks only at the clock:
    // a change in the facts above has already bumped _lastEvent(), which is how
    // a fault or a move lights the panel, and nothing holds it lit after that.
    const bool wantLit = awake(_lastEvent(), now);
    if (!wantLit) {
        if (_lit()) {
            // DISPLAYOFF, not a cleared framebuffer: it stops the panel driving
            // pixels at all, which is the point — a black screen still ages the
            // lit ones beside it.
            _oled().ssd1306_command(SSD1306_DISPLAYOFF);
            _lit() = false;
        }
        return;
    }
    if (!_lit()) {
        _oled().ssd1306_command(SSD1306_DISPLAYON);
        _lit() = true;
        _lastDraw() = 0;   // force the first frame after a wake
    }

    if (_lastDraw() && (now - _lastDraw()) < kRedrawMs) return;
    _lastDraw() = now;
    _draw(render(f), now);
}

} // namespace statusscreen

#else   // ---- no screen fitted: every entry point compiles to nothing ----

namespace statusscreen {
inline bool begin()   { return false; }
inline bool present() { return false; }
inline bool lit()     { return false; }
inline void note()    {}
inline void toggle()  {}
inline void update(const Facts&) {}
} // namespace statusscreen

#endif  // PIN_OLED_SDA && PIN_OLED_SCL

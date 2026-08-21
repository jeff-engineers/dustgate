#pragma once
// =============================================================================
// StatusVocab.h — the board's status vocabulary, and nothing else.
//
// PURE. No Arduino.h, no I/O, no state. It exists so the two things that show
// status can share one enum instead of two that agree today.
//
//   StatusLed.h        — the pixel. Colour per state, read across the shop.
//   StatusScreenModel.h — the optional OLED. Words per state, read at arm's
//                        length, and host-testable because this header is pure.
//
// A screen that could ever disagree with the pixel beside it would be worse
// than having neither, so "mirrors statusled::Status" is enforced by there
// being only one Status to mirror. The colour meanings, the brightness
// reasoning and the blink rates all stay in StatusLed.h — this is the enum, not
// the design.
// =============================================================================

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
                  //        primary: no topology stored, or a paired board is dark)
    READY         // green — node: primary linked.
                  //         primary: routing live AND every paired board answering.
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

} // namespace statusled

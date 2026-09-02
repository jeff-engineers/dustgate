#pragma once
// =============================================================================
// FaultPolicy.h — what a failed begin() stage actually costs.
//
// Three independent things can fail while a board starts, and they disable
// different amounts of the shop. This header owns the mapping from "which
// stage failed" to "what is refused", so the answer is one table instead of a
// condition repeated across the sketch — and so it can be tested on a host,
// which the sketch itself cannot be.
//
// PURE. No Arduino.h, no globals, no I/O.
//
// The failure this exists to prevent: one latched boolean covering all three
// stages, which meant a board whose WiFi mutex failed to allocate also refused
// to home its rack — two subsystems that share no wire. On a bench board the
// same latch took the servo gates down every time stepper power browned out,
// which is the common desk condition, not a broken shop.
// =============================================================================

namespace faults {

/** One flag per begin() stage. True = that stage FAILED. */
struct Stages {
    bool motor    = false;   // the drive failed to start — on a slider board,
                             // ST3215LinearDriver::begin() (UART, ping or the
                             // stepping-mode write). Was named for the TMC2209
                             // until 2026-08-28; that chip is in the attic.
    bool endstops = false;   // limit switches
    bool outlets  = false;   // WiFi / Shelly plugs
};

/** What the board refuses, given those stages. */
struct Policy {
    // Refuse home / jog / move / calibrate. The sketch's g_hardwareFault.
    bool linearMotion = false;
    // STATE_ERROR and the red status pixel: "something is WRONG here".
    bool errorState   = false;
    // Say it out loud so it stays a decision rather than an omission: servo
    // gates and topology routing are refused by NOTHING in this file. They are
    // on their own rail with their own driver, and a shop whose stepper died
    // still opens every ball valve it has.
    static constexpr bool servoGates = false;
};

/**
 * @param s           which stages failed
 * @param rackFitted  false on a board whose pin map wires no slider (!HAS_LINEAR)
 *
 * The two rules, and why each is drawn where it is:
 *
 *   LINEAR MOTION is refused by a motor fault (no driver to command) and by an
 *   endstop fault (there is a motor, but homing without a reference is how a
 *   carriage gets driven into its own end). A board with no rack refuses it
 *   too — permanently, by build, with nothing broken.
 *
 *   ERROR STATE is the red pixel, and it means "something is wrong", which is
 *   narrower than "something is refused". A rackless board refuses motion and
 *   is perfectly healthy; painting it red would spend the one at-a-glance
 *   diagnostic the board has on a build flag, and red would stop meaning
 *   anything for the faults that are real.
 *
 *   OUTLETS never set either one. It is a network problem — the link comes back
 *   on its own (WiFiProvisioner::maintain()), so a boot-time latch would outlive
 *   the fault it describes, and the pixel's NO_WIFI state already says it.
 */
inline Policy decide(Stages s, bool rackFitted) {
    if (!rackFitted) s.motor = false;   // no driver expected; not news

    const bool motionHardware = s.motor || s.endstops;
    Policy p;
    p.linearMotion = motionHardware || !rackFitted;
    p.errorState   = motionHardware;
    return p;
}

}  // namespace faults

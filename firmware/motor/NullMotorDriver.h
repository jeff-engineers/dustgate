// =============================================================================
// NullMotorDriver.h — the motor a board without a rack has.
//
// HAS_LINEAR == 0 used to be unbuildable for the primary sketch: config.h
// derived the capability from the pin map and then nothing read it, so a
// servo-only board could only be a NODE (firmware/node/, a different program
// with its own setup()/loop()). Making a XIAO C5 a PRIMARY on 2026-08-22 is what
// forced the seam to become real — same board, same pins, and the choice of
// which program it runs stops depending on whether a stepper exists.
//
// WHY A NULL OBJECT AND NOT #if AT THE CALL SITES. firmware.ino touches `motor`
// in 46 places, spread through the state machine, the calibration sweep and the
// serial commands. Guarding each one would put the rack's absence into every
// one of those places and change motion code that HAS run on hardware. A null
// driver changes none of it: the call sites compile unchanged, and what they
// call does nothing.
//
// It reports honestly rather than convincingly. begin() returns TRUE — there is
// no broken hardware here, which is the difference between this and a TMC2209
// that failed its UART handshake, and FaultPolicy must not see a fault on a
// board that was never fitted with a rack. Everything after that says "not
// moving, at position zero": isMoving() false, so the state machine's MOVING
// branches fall straight through; getPosition() 0; distanceToGo() 0, so the
// endstop supervisor has nothing to supervise. A moveTo() is dropped, and says
// so once on serial rather than every pass of loop().
//
// NOTE this is the small half of the job. The API layer still advertises the
// linear vocabulary (position, homing, stops) to the UI on a board that has
// none, which the web UI has never been shown. What that should report is a
// decision, not a cleanup — logged in TODO.md.
// =============================================================================

#pragma once
#include "MotorDriver.h"
#include "../config.h"

#if !HAS_LINEAR

class NullMotorDriver : public MotorDriver {
public:
    // True: nothing is broken. A board with no rack is a working board.
    bool begin() override { return true; }

    void startHoming() override        { refuse(F("home")); }
    void moveTo(long) override         { refuse(F("move")); }
    void stop() override               {}
    void update() override             {}
    bool isMoving() override           { return false; }
    long getPosition() override        { return 0; }
    void setHome() override            {}
    void enable(bool) override         {}

    // The two the sketch calls on the concrete type rather than through
    // MotorDriver*, so they have to exist here too.
    void setMaxSpeed(float)            {}
    long distanceToGo()                { return 0; }
    void printDriverRegs() {
        Serial.println(F("[MOTOR] no linear rack in this build (no PIN_TMC_STEP "
                         "in the board header) — no registers to read."));
    }

private:
    // Once per kind of request, not once per loop: a UI that polls a move it
    // cannot have would otherwise fill the log with it.
    void refuse(const __FlashStringHelper* what) {
        static const __FlashStringHelper* last = nullptr;
        if (last == what) return;
        last = what;
        Serial.print(F("[MOTOR] "));
        Serial.print(what);
        Serial.println(F(" ignored — this board has no linear rack."));
    }
};

#endif // !HAS_LINEAR

// =============================================================================
// LimitSwitchDistance.h — two endstops and a step count, for the sliding gate.
//
// THE ONE PIECE OF THE STEPPER RIG THAT SURVIVED IT. Retired with the stepper on
// 2026-08-23 and brought back for the ST3215 slider on 2026-08-28, unchanged in
// shape, because the reason it
// existed never went away. A bus servo in stepping mode reports how much of the
// last command is outstanding, never where the shaft is, so its zero does not
// survive a power cycle — and the homing sweep of
// docs/dual-endstop-calibration.md is still the only thing that can put a datum
// on the rail.
//
// WHAT CHANGED IN THE PORT: nothing structural. The arithmetic here counts
// "steps", and on this board a step is one encoder count instead of one
// microstep — utils/MotionMath.h absorbs that, so the sweep, the backoff and the
// span all read the same as they did against the stepper. The retirement note
// guessed this file's step arithmetic would have to go; it didn't, because the
// unit change happened one level down.
//
// Only the SWEEP lives here. Directional over-travel — refusing to drive further
// INTO a triggered switch while still allowing a move away from one — is the
// main loop's endstop supervisor, and has to be, because it must hold on a move
// that has nothing to do with homing.
// =============================================================================

#pragma once
#include "FeedbackSystem.h"
#include "../config.h"

#if HAS_LINEAR

class LimitSwitchDistance : public FeedbackSystem {
public:
    LimitSwitchDistance();
    bool begin(MotorDriver* motor) override;
    void resetHoming() override;
    bool updateHoming() override;
    bool updateMoving(int targetStop) override;
    long stepsForStop(int stopIndex) override;

    // Did the sweep give up? Set when the datum switch reads TRIGGERED and stays
    // that way even after backing the carriage off it — which is not a carriage
    // at the end of its rail, it is a switch that is lying.
    //
    // This exists because the failure it reports used to look like SUCCESS. With
    // NC wiring an unwired, miswired or ungrounded switch reads HIGH, which is
    // "triggered", so homing stopped one chunk into its sweep, backed off a
    // millimetre and announced a datum it had never found (2026-08-28). Silent
    // wrong answers about where zero is are the worst thing this file can
    // produce — every gate position is measured from it.
    bool failed() const { return _failed; }
    const char* failure() const { return _failure; }

    // How far the carriage ACTUALLY backed off the datum switch, in steps.
    //
    // Not the same as HOME_BACKOFF_STEPS, and the difference matters. The datum
    // is defined as "the trigger point, backed off", and the span math adds this
    // number back to the home→far sweep count. A backoff that had to be extended
    // because the switch was still made at the nominal distance therefore has to
    // be REPORTED, or calibration measures a span short by the extra and places
    // every gate wrong (2026-08-28: a 4-gate calibration put gate 4 about half
    // way along the rail).
    long backoffSteps() const { return _backoffUsed; }

    // PUBLIC because the slider NODE needs them too. It runs its own sweep
    // wrapper (node/dustgate_node.cpp) and has to spot the far switch answering
    // instead of the datum — the backwards-motor case. Exposing the readers
    // beats a third copy of the NC polarity convention: the primary sketch
    // already carries a second one, and a copy that disagreed about which level
    // means "triggered" would fail as a carriage driving into the rail end.
    bool readHomeSwitch();
    bool readMaxSwitch();

private:
    // The sweep has a phase before the sweep: get OFF the switch if we are
    // already on it. A carriage parked against its endstop at power-up is
    // ordinary — it is where the last session left it — and driving further into
    // it is not the answer.
    enum Phase : uint8_t {
        RELEASING,   // datum reads triggered; backing away until it clears
        SEEKING,     // the actual sweep toward the datum
        BACKING_OFF, // datum found; moving clear of it to set zero
        DONE
    };

    MotorDriver* _motor;
    bool _homed;
    bool _backingOff;
    Phase _phase;
    long  _releaseStart;      // position when the release began, to bound it
    long  _triggerPos;        // where the datum switch fired
    long  _backoffUsed;       // trigger → datum, actually travelled
    bool  _failed;
    const char* _failure;
};

#endif // HAS_LINEAR

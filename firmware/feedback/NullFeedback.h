// =============================================================================
// NullFeedback.h — the feedback system a board without endstops has.
//
// The other half of NullMotorDriver.h; read that file's header for why this is
// a null object rather than #if at the call sites. LimitSwitchDistance is the
// only real implementation, it needs two switches and a step count, and a
// servo-only board has neither.
//
// updateHoming() returns TRUE — "homing is finished" — which looks like a lie
// and isn't: the caller's question is "may I leave STATE_HOMING?", and on a
// board with nothing to home the answer is yes, immediately. Returning false
// would park the state machine in HOMING forever waiting for a switch that does
// not exist. updateMoving() answers the same way for the same reason.
//
// stepsForStop() returns 0 rather than a computed position. Nothing should be
// asking a rackless board where gate N is, but if something does, home is a
// safer answer than a plausible-looking number.
// =============================================================================

#pragma once
#include "FeedbackSystem.h"
#include "../config.h"

#if !HAS_LINEAR

class NullFeedback : public FeedbackSystem {
public:
    // True for the same reason NullMotorDriver::begin() is: nothing failed.
    bool begin(MotorDriver*) override { return true; }

    void resetHoming() override       {}
    bool updateHoming() override      { return true; }   // nothing to home: done
    bool updateMoving(int) override   { return true; }   // nowhere to go: arrived
    long stepsForStop(int) override   { return 0; }
};

#endif // !HAS_LINEAR

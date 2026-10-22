// =============================================================================
// control/FailsafeStop.h — the collector's board stopping its blower when the brain is gone (D-85, piece 4).
//
// WHY. A node closes its gates when it stops hearing the brain (NodeLink.h FailsafeTimer). If the brain itself is gone,
// every node does — and a blower left running against closed gates is the one thing this project never does. So the
// board at the collector, which has the remote's transmitter and the clamp on the blower, presses it OFF.
//
// THE PRESS IS A TOGGLE, so it is judged by the clamp, never assumed: no reading, no press (a press into an unknown
// state could just as well start the blower); a reading of OFF is done; a reading of ON presses, waits the same
// kPressCooldownOffMs the brain waits after an OFF press (a reading is slow to fall), and tries again, up to
// kMaxPressAttempts in total — the brain's own numbers (CollectorPress.h), so the two never argue about a stuck press.
//
// Pure — host-tested in test_failsafestop.cpp. The node ticks it from loop(); the brain coming back disarms it, and the
// brain's own press policy takes over from there.
// =============================================================================
#pragma once
#include <cstdint>
#include "CollectorPress.h"

namespace topo {

enum class StopAction : uint8_t {
    Nothing,   // not armed, no reading yet, or waiting out a cooldown
    Press,     // key the remote once
    Stopped,   // the clamp says it is off: done (said once)
    GaveUp,    // kMaxPressAttempts pressed and it still runs (said once)
};

class CollectorStopper {
public:
    void arm()    { _armed = true; _presses = 0; _lastPressMs = 0; _done = false; }
    void disarm() { _armed = false; }
    bool armed() const { return _armed; }

    // `known`: the clamp has a reading. `running`: that reading says the blower draws.
    StopAction tick(uint32_t nowMs, bool known, bool running) {
        if (!_armed || _done) return StopAction::Nothing;
        if (!known) return StopAction::Nothing;
        if (_presses && nowMs - _lastPressMs < kPressCooldownOffMs) return StopAction::Nothing;
        if (!running) { _done = true; return StopAction::Stopped; }
        if (_presses >= kMaxPressAttempts) { _done = true; return StopAction::GaveUp; }
        _presses++;
        _lastPressMs = nowMs ? nowMs : 1;
        return StopAction::Press;
    }
    uint8_t presses() const { return _presses; }

private:
    bool     _armed = false, _done = false;
    uint8_t  _presses = 0;
    uint32_t _lastPressMs = 0;
};

}  // namespace topo

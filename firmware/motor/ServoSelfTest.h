// =============================================================================
// ServoSelfTest.h — walk every servo on this board, one at a time, and sweep it.
//
// A BENCH TOOL, reached by holding the wake button for a second (WakeButton.h).
// Run on hardware 2026-08-22: all four channels, on both a primary and a node.
// The collector refusal in start() is the one path still unexercised.
// It exists because of a specific bug and the shape of that bug is worth keeping
// in mind: on Arduino core 3.x, Servo::detach() left the LEDC channel wedged, so
// a servo moved exactly ONCE per boot and was then silently dead — the UI still
// reported success, the log said nothing, and the only symptom was a valve that
// didn't turn. See ServoActuator::_deenergize().
//
// What that costs to test by hand is a laptop, a phone or a serial cable, plus a
// topology to drive gates from. This is the same question — "do all four
// channels still answer, more than once?" — asked with a finger, at the board,
// with the answer on the panel two feet away.
//
// RAW CHANNELS, DELIBERATELY. It talks to ServoActuator directly and knows
// nothing about topology, gates, calibration or LocalActuatorBus. A board with
// no shop stored, no gates allocated and no calibration answers this exactly the
// same as a fully configured one — which is the point: the layer it is testing
// is the bottom one, and every check above it is a way for the test to refuse
// when the thing you wanted to know is whether the pin still moves. Channels are
// the four PWM pads in board order, not gate numbers.
//
// SEQUENTIAL, one channel at a time, and not for tidiness: LocalActuatorBus's
// mutex exists because the 5V rail cannot drive two servos at once
// (docs/architecture-rfc.md §7). A self-test that swept all four together would
// brown out the board it is meant to be testing.
//
// NON-BLOCKING for the same reason everything else here is: loop() has a
// watchdog on it and a web server under it. update() advances one step per pass.
//
// A FULL CYCLE IN EACH DIRECTION means out to kMaxDeg, back to kMinDeg, per
// channel — deliberately NOT 0 and 180. A ball valve's hard stops live near the
// ends of the range and its calibrated open/closed angles are inside them, so a
// self-test that slammed the full mechanical range could drive a fitted valve
// into its stop and stall the servo against it. The default 60-120 is a wide,
// obvious, unmistakable movement that no sane calibration puts a hard stop
// inside of. Widen it in config.h for a bare servo on the bench with no valve
// attached, not for a shop.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "../config.h"
#include "ServoActuator.h"

#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)

namespace servoselftest {

// Sweep bounds. See the header note before widening these.
#ifndef SERVO_SELFTEST_MIN_DEG
#define SERVO_SELFTEST_MIN_DEG 60
#endif
#ifndef SERVO_SELFTEST_MAX_DEG
#define SERVO_SELFTEST_MAX_DEG 120
#endif
// Pause between legs, so a person watching can tell two moves apart rather than
// seeing one continuous wander.
#ifndef SERVO_SELFTEST_DWELL_MS
#define SERVO_SELFTEST_DWELL_MS 400
#endif

enum class Phase : uint8_t { IDLE, OUT, BACK, DWELL };

inline ServoActuator*& _servos()  { static ServoActuator* p = nullptr; return p; }
inline int&      _count()         { static int n = 0;  return n; }
inline Phase&    _phase()         { static Phase p = Phase::IDLE; return p; }
inline int&      _ch()            { static int c = -1; return c; }
inline int&      _angle()         { static int a = -1; return a; }
inline Phase&    _next()          { static Phase p = Phase::IDLE; return p; }
inline uint32_t& _dwellUntil()    { static uint32_t t = 0; return t; }
inline const char*& _refusal()    { static const char* r = nullptr; return r; }

/** Bind the board's servo bank. Call once from setup(), after servos[].begin(). */
inline void begin(ServoActuator* servos, int count) {
    _servos() = servos;
    _count()  = count;
}

inline bool active()      { return _phase() != Phase::IDLE; }
/** 1-BASED channel under test — it is shown to a person, and the pads say 1-4. */
inline int  channel()     { return active() ? _ch() + 1 : -1; }
inline int  angle()       { return active() ? _angle() : -1; }
/** Why the last start() was refused, or null. Cleared by the next start(). */
inline const char* refusal() { return _refusal(); }

/**
 * Begin the sweep. Returns false and sets refusal() if it must not run now.
 *
 * collectorRunning is the one hard refusal, and it is not a nicety: sweeping
 * every gate shut with the collector pulling is the dead-head the whole system
 * is designed to prevent (CLAUDE.md, "Never dead-head the collector"). The
 * button cannot ask "are you sure", so the answer has to be no. Switch the
 * collector off and press again.
 */
inline bool start(bool collectorRunning) {
    _refusal() = nullptr;
    if (active()) { _refusal() = "already running"; return false; }
    if (!_servos() || _count() <= 0) { _refusal() = "no servos"; return false; }
    if (collectorRunning) { _refusal() = "collector on"; return false; }

    _ch()    = 0;
    _angle() = SERVO_SELFTEST_MAX_DEG;
    _phase() = Phase::OUT;
    _servos()[0].moveTo(_angle());
    return true;
}

/** Stop wherever it is. The servo finishes its current sweep on its own. */
inline void cancel() { _phase() = Phase::IDLE; _ch() = -1; _angle() = -1; }

/**
 * Call every loop(). Waits on ServoActuator::isMoving(), which stays true
 * through the post-move hold — so each leg is complete, coil and all, before
 * the next one starts.
 */
inline void update() {
    if (!active()) return;
    ServoActuator* sv = _servos();

    if (_phase() == Phase::DWELL) {
        if ((int32_t)(millis() - _dwellUntil()) < 0) return;
        _phase() = _next();
        if (_phase() == Phase::IDLE) { cancel(); return; }
        _angle() = (_phase() == Phase::OUT) ? SERVO_SELFTEST_MAX_DEG
                                            : SERVO_SELFTEST_MIN_DEG;
        sv[_ch()].moveTo(_angle());
        return;
    }

    if (sv[_ch()].isMoving()) return;   // includes the hold: the leg isn't done

    // Leg finished. Out-and-back on this channel, then on to the next.
    _dwellUntil() = millis() + SERVO_SELFTEST_DWELL_MS;
    if (_phase() == Phase::OUT) {
        _next() = Phase::BACK;
    } else {
        _ch()++;
        _next() = (_ch() < _count()) ? Phase::OUT : Phase::IDLE;
    }
    _phase() = Phase::DWELL;
}

} // namespace servoselftest

#else   // ---- no servo bank on this board: every entry point compiles away ----

namespace servoselftest {
class ServoActuatorStub;
inline void begin(void*, int)      {}
inline bool start(bool)            { return false; }
inline void update()               {}
inline void cancel()               {}
inline bool active()               { return false; }
inline int  channel()              { return -1; }
inline int  angle()                { return -1; }
inline const char* refusal()       { return nullptr; }
} // namespace servoselftest

#endif  // ENABLE_SERVO && SERVO_PWM_PIN_1

// =============================================================================
// ServoActuator.h — positional-servo driver (ball-valve gates).
//
// A thin wrapper over ESP32Servo. Instead of snapping to the target, it SWEEPS
// there smoothly (a gentle eased motion, not a slam) over a duration proportional to
// how far it actually has to travel — a full quarter-turn takes the deliberate
// SERVO_SWEEP_MS, a few degrees land almost at once, which is what makes the setup
// jog control usable. It then
// HOLDS the servo energized for SERVO_HOLD_MS so an analog servo has time to
// catch up to the commanded angle, and finally auto-DETACHES so the coil
// de-energizes — the ball valve holds position by hard-stop friction / optional
// detent (see docs/topology-schema.md). Detaching is essential for ANALOG
// servos (e.g. Power HD 3001HB), which hunt/groan continuously while holding.
// Set holdAtRest(true) only for a build that would back-drive when de-energized.
//
// Non-blocking: moveTo() sets up the sweep; update() (called every loop) steps
// the angle, then performs the deferred detach. This is the muscle behind the
// branch-selector HAL: a servo state is state → angle.
//
// POWER: servos run off an EXTERNAL 5–6V rail, NOT the ESP32 pins — only the PWM
// signal wire goes to the GPIO, and grounds must be common.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "../config.h"

#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)

#include <ESP32Servo.h>

class ServoActuator {
public:
    // Bind to a GPIO (does not attach/energize yet — attach happens on first move).
    void begin(int pin) { _pin = pin; }

    // Keep the servo energized after moving (default false = move-then-detach).
    void setHoldAtRest(bool hold) { _holdAtRest = hold; }

    // Move to an angle (0–180°). Attaches on demand and starts a smooth eased
    // sweep from the current angle over SERVO_SWEEP_MS; update() drives it. The
    // very first move (position unknown) goes directly. Pulse bounds 500–2500µs.
    void moveTo(int angleDeg, int minUs = 500, int maxUs = 2500) {
        if (_pin < 0) return;
        angleDeg = constrain(angleDeg, 0, 180);
        if (!_servo.attached()) {
            _servo.setPeriodHertz(50);          // standard 50Hz servo frame
            _servo.attach(_pin, minUs, maxUs);
        }
        if (_curAngle < 0) {                     // first move: unknown start → snap
            _curAngle = angleDeg;
            _servo.write(angleDeg);
            _sweeping    = false;
            _detachAtMs  = millis() + SERVO_HOLD_MS;
            _detachArmed = !_holdAtRest;
            return;
        }
        _fromAngle    = _curAngle;
        _toAngle      = angleDeg;
        _sweepMs      = sweepMsFor(_toAngle - _fromAngle);
        _sweepStartMs = millis();
        _sweeping     = (_fromAngle != _toAngle);
        _detachArmed  = false;                   // re-armed when the sweep finishes
        if (!_sweeping) {                        // no movement → just (re)hold then detach
            _detachAtMs  = millis() + SERVO_HOLD_MS;
            _detachArmed = !_holdAtRest;
        }
    }

    // Call every loop(): advances the eased sweep, then performs the deferred
    // detach once the post-move hold has elapsed.
    void update() {
        const unsigned long now = millis();
        if (_sweeping) {
            const unsigned long el = now - _sweepStartMs;
            if (el >= _sweepMs) {
                _curAngle    = _toAngle;
                _servo.write(_toAngle);
                _sweeping    = false;
                _detachAtMs  = now + SERVO_HOLD_MS;   // hold powered so it can catch up
                _detachArmed = !_holdAtRest;
            } else {
                float t = (float)el / (float)_sweepMs;        // 0..1
                t = t * t * (3.0f - 2.0f * t);                // smoothstep ease-in-out
                _curAngle = _fromAngle + (int)lroundf((_toAngle - _fromAngle) * t);
                _servo.write(_curAngle);
            }
            return;                                  // never detach mid-sweep
        }
        if (_detachArmed && _servo.attached() && (long)(now - _detachAtMs) >= 0) {
            _servo.detach();
            _detachArmed = false;
        }
    }

    // Immediately de-energize (stop pulses; ball holds by friction/detent).
    void detach() { _servo.detach(); _detachArmed = false; _sweeping = false; }

    bool attached() { return _servo.attached(); }
    int  pin() const { return _pin; }

    // True while this servo is drawing move current: the eased sweep is running,
    // OR the post-move hold is still energizing the coil so an analog servo can
    // catch up. LocalActuatorBus gates on this to guarantee only one servo is
    // ever driven at a time — the 5V rail can't take two (see
    // docs/architecture-rfc.md §7). A holdAtRest servo reports false once the
    // sweep ends: it draws holding current indefinitely, so blocking on it would
    // deadlock the move queue.
    bool isMoving() const {
        if (_sweeping) return true;
        return _detachArmed && (long)(millis() - _detachAtMs) < 0;
    }

private:
    // Sweep duration for a move of this many degrees. Proportional to travel so a
    // full quarter-turn still eases over the deliberate SERVO_SWEEP_MS while a small
    // setup nudge lands immediately — see the SERVO_MS_PER_DEG note in config.h.
    static unsigned long sweepMsFor(int deltaDeg) {
        const unsigned long want = (unsigned long)abs(deltaDeg) * SERVO_MS_PER_DEG;
        if (want < SERVO_SWEEP_MIN_MS) return SERVO_SWEEP_MIN_MS;
        if (want > SERVO_SWEEP_MS)     return SERVO_SWEEP_MS;
        return want;
    }

    Servo         _servo;
    int           _pin         = -1;
    bool          _holdAtRest  = false;   // default: move then detach (analog-friendly)
    int           _curAngle    = -1;      // last commanded angle (-1 = unknown until first move)
    int           _fromAngle   = 0;
    int           _toAngle     = 0;
    bool          _sweeping    = false;
    unsigned long _sweepStartMs = 0;
    unsigned long _sweepMs     = SERVO_SWEEP_MS;   // this move's duration (travel-scaled)
    bool          _detachArmed = false;
    unsigned long _detachAtMs  = 0;
};

#endif // ENABLE_SERVO && SERVO_PWM_PIN_1

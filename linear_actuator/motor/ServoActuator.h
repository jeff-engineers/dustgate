// =============================================================================
// ServoActuator.h — Phase-2 positional-servo driver (ball-valve gates).
//
// A thin wrapper over ESP32Servo. Drives one positional servo to an angle and,
// by DEFAULT, auto-detaches once the move has settled (SERVO_MOVE_MS) so the coil
// de-energizes — the ball valve holds position by hard-stop friction / optional
// detent (see docs/v2-topology-schema.md). Detaching is essential for ANALOG
// servos (e.g. Power HD 3001HB), which hunt/groan continuously while holding.
// Set holdAtRest(true) only for a build that would back-drive when de-energized.
//
// Non-blocking: moveTo() writes the angle and arms a deferred detach; update()
// (called every loop) performs the detach when the settle time elapses. This is
// the muscle behind the branch-selector HAL: a servo state is state → angle.
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

    // Move to an angle (0–180°). Attaches (energizes) on demand, writes the angle,
    // and — unless holdAtRest — arms an auto-detach SERVO_MOVE_MS later so an
    // analog servo doesn't groan while holding. Pulse bounds default 500–2500µs.
    void moveTo(int angleDeg, int minUs = 500, int maxUs = 2500) {
        if (_pin < 0) return;
        angleDeg = constrain(angleDeg, 0, 180);
        if (!_servo.attached()) {
            _servo.setPeriodHertz(50);          // standard 50Hz servo frame
            _servo.attach(_pin, minUs, maxUs);
        }
        _servo.write(angleDeg);
        _detachAtMs   = millis() + SERVO_MOVE_MS;
        _detachArmed  = !_holdAtRest;
    }

    // Call every loop(): performs the deferred auto-detach once the move settles.
    void update() {
        if (_detachArmed && _servo.attached() && (long)(millis() - _detachAtMs) >= 0) {
            _servo.detach();
            _detachArmed = false;
        }
    }

    // Immediately de-energize (stop pulses; ball holds by friction/detent).
    void detach() { _servo.detach(); _detachArmed = false; }

    bool attached() { return _servo.attached(); }
    int  pin() const { return _pin; }

private:
    Servo         _servo;
    int           _pin        = -1;
    bool          _holdAtRest = false;   // default: move then detach (analog-friendly)
    bool          _detachArmed = false;
    unsigned long _detachAtMs = 0;
};

#endif // ENABLE_SERVO && SERVO_PWM_PIN_1

// =============================================================================
// LocalActuatorBus.h — the ActuatorBus backed by THIS board's hardware.
//
// Translates a selector state into motion on the local PWM bank or the local
// stepper:
//
//   servoGate / servoManifold → g_servos[servo.channel], commanded to
//                               topo::servoCommandAngle() (referenceAngle +
//                               state.offsetDeg, clamped). A selector with no
//                               calibrated referenceAngle can't be driven and is
//                               rejected rather than sent to a guessed angle.
//   linear                    → the rack, driven to states[].positionMm.
//
// The angle math is NOT reimplemented here — it comes from TopologyRouter.h so
// the firmware and the host conformance test can never disagree about where a
// valve should point.
//
// busy() is the current mutex: true while ANY bound servo is sweeping or holding,
// or the rack is moving. TopologyRuntime refuses to start the next move until it
// clears, which is what keeps four servos from ever moving at once.
//
// Hardware access is INJECTED (bindServo / bindLinear) rather than reached for
// via extern globals, so this file has no dependency on the sketch and the
// stepper half compiles out cleanly on a servo-only board.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "../config.h"
#include "ActuatorBus.h"
#include "TopologyRouter.h"   // topo::servoCommandAngle, topo::_eq

#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)
  #include "../motor/ServoActuator.h"
#endif

namespace topo {

// The rack, as the bus needs to see it. Implemented by the sketch (which owns
// the stepper, the feedback system and the homing state machine) so none of that
// leaks in here. A board with no stepper simply never binds one.
class LinearDrive {
public:
    virtual ~LinearDrive() {}
    // Command an absolute position in mm from the home datum. Return false if
    // the move can't be made right now (not homed, disabled, hardware fault).
    virtual bool moveToMm(float mm) = 0;
    virtual bool isMoving() const = 0;
};

class LocalActuatorBus : public ActuatorBus {
public:
#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)
    // Bind a PWM channel to its servo. Channels are the `servo.channel` values
    // the configurator writes into the topology (0-based, matching g_servos[]).
    void bindServo(int channel, ServoActuator* servo) {
        if (channel < 0 || channel >= SERVO_COUNT) return;
        _servos[channel] = servo;
    }
#endif

    void bindLinear(LinearDrive* drive) { _linear = drive; }

    bool online() const override { return true; }   // local hardware is always "reachable"

    bool busy() const override {
#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)
        for (int i = 0; i < SERVO_COUNT; i++)
            if (_servos[i] && _servos[i]->isMoving()) return true;
#endif
        if (_linear && _linear->isMoving()) return true;
        return false;
    }

    bool setState(const char* selectorId, JsonObjectConst sel, const char* stateId) override {
        const char* kind = sel["kind"].as<const char*>();
        if (!kind) return false;

        bool ok;
        if (strcmp(kind, "linear") == 0) ok = driveLinear(sel, stateId);
        else if (strcmp(kind, "servoGate") == 0 || strcmp(kind, "servoManifold") == 0)
            ok = driveServo(sel, stateId);
        else return false;   // unknown kind — reject rather than guess

        // Traced whether it worked or not: a refusal here (uncalibrated gate,
        // unbound channel, no stepper) is silent everywhere else, and looks
        // identical to the brain never deciding to move at all.
        DEBUG_PRINT(F("[LOCAL] ")); DEBUG_PRINT(selectorId);
        DEBUG_PRINT(F(" -> ")); DEBUG_PRINT(stateId);
        if (strcmp(kind, "linear") != 0) {
            DEBUG_PRINT(F("  servo")); DEBUG_PRINT((sel["servo"]["channel"] | -1) + 1);
            DEBUG_PRINT(F(" = ")); DEBUG_PRINT(servoCommandAngle(sel, stateId)); DEBUG_PRINT(F("deg"));
        }
        DEBUG_PRINTLN(ok ? F("  [sent]") : F("  [REFUSED]"));
        return ok;
    }

    void update() override {
#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)
        // Advance each sweep and effect the deferred detach. Harmless on a servo
        // that isn't moving, and idempotent if the sketch also pumps them.
        for (int i = 0; i < SERVO_COUNT; i++) if (_servos[i]) _servos[i]->update();
#endif
    }

private:
    bool driveServo(JsonObjectConst sel, const char* stateId) {
#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)
        if (!servoIsCalibrated(sel)) return false;   // never set up — don't guess an angle
        int angle = servoCommandAngle(sel, stateId);
        if (angle == INT32_MIN) return false;        // no such state / no offsetDeg

        JsonObjectConst sv = sel["servo"];
        int ch = sv["channel"] | -1;
        if (ch < 0 || ch >= SERVO_COUNT || !_servos[ch]) return false;

        _servos[ch]->setHoldAtRest(sv["holdAtRest"] | false);
        _servos[ch]->moveTo(angle);
        return true;
#else
        (void)sel; (void)stateId;
        return false;   // no servo support in this build
#endif
    }

    bool driveLinear(JsonObjectConst sel, const char* stateId) {
        if (!_linear) return false;              // no stepper on this board
        for (JsonObjectConst s : sel["states"].as<JsonArrayConst>()) {
            if (!_eq(s["id"], stateId)) continue;
            if (!s.containsKey("positionMm")) return false;   // uncalibrated
            return _linear->moveToMm(s["positionMm"].as<float>());
        }
        return false;
    }

#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)
    ServoActuator* _servos[SERVO_COUNT] = { nullptr };
#endif
    LinearDrive* _linear = nullptr;
};

} // namespace topo

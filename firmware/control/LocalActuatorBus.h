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
#include "NodeLink.h"       // kMaxSensorsPerNode / kMaxSensorIdLen — one cap, both sides of the seam
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

    // The local half of the setup jog — see ActuatorBus::jog(). A detach is
    // meaningful here in a way it is not over the wire: this owns the servo
    // object, so it can de-energise it directly.
    bool jog(int channel, int angle, bool detach) override {
#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)
        if (channel < 0 || channel >= SERVO_COUNT) return false;
        if (!_servos[channel]) return false;
        if (detach) _servos[channel]->detach();
        else        _servos[channel]->moveTo(angle);
        return true;
#else
        (void)channel; (void)angle; (void)detach;
        return false;      // no PWM bank compiled in — say so, do not pretend
#endif
    }

    // ── SENSORS ON THIS BOARD'S OWN PADS ────────────────────────────────
    //
    // A clamp wired to the BRAIN, which is the ordinary case for a collector:
    // the board at the cyclone watches the blower it also commands. Before this
    // existed, ActuatorBus's no-op stubs swallowed the whole path — the primary
    // pushed its own specs to `""`, they landed nowhere, and pollSensors() read
    // absent-therefore-off forever, silently. Only clamps on a REMOTE board
    // worked.
    //
    // The sketch owns the ADC and ticks sensing::CtTrip (one copy, shared with
    // the node — see sensing/CtTrip.h); this class only remembers which sensorIds
    // the layout put on this board and what the last answer was. It deliberately
    // does NOT touch CtSensor: this header is included by host tests that have no
    // Arduino ADC, and the seam is the same one RemoteActuatorBus sits behind.
    void configureSensors(JsonArrayConst sensors) override {
        _senseCount = 0;
        for (JsonObjectConst sen : sensors) {
            if (_senseCount >= nodelink::kMaxSensorsPerNode) break;
            const char* id = sen["sensorId"] | "";
            if (!*id) continue;
            strlcpy(_senseIds[_senseCount], id, sizeof(_senseIds[0]));
            _senseOn[_senseCount] = false;
            _senseCount++;
        }
        // atMs stays 0 until something is actually measured, so a configured but
        // never-sampled sensor reads as NOT REPORTED rather than as off. The two
        // are different: pollSensors() treats both as off today, and the day it
        // stops it must not be lied to here.
        _senseAtMs = 0;
    }

    bool senseOf(const char* sensorId, bool& on, uint32_t& atMs) const override {
        if (!sensorId || !_senseAtMs) return false;
        for (size_t i = 0; i < _senseCount; i++) {
            if (strcmp(_senseIds[i], sensorId) != 0) continue;
            on = _senseOn[i]; atMs = _senseAtMs;
            return true;
        }
        return false;
    }

    // Called by the sketch after each CtTrip tick. ONE CLAMP, ONE PAD: every
    // sensor configured onto this board reads the same ADC, exactly as on a
    // node. When a second analog pad exists, this is where it branches.
    void setSense(bool on, uint32_t atMs, float level = -1.0f) {
        for (size_t i = 0; i < _senseCount; i++) _senseOn[i] = on;
        _senseAtMs  = atMs ? atMs : 1;   // 0 means "never reported"
        _senseLevel = level;
    }

    // Same enumeration RemoteActuatorBus offers, so GET /api/nodes can report
    // the board it is running on exactly like any other. See senseAt() there.
    size_t senseCount() const { return _senseCount; }
    bool senseAt(size_t i, String& id, bool& reported, bool& on,
                 uint32_t& ageMs, float& level) const {
        if (i >= _senseCount) return false;
        id       = _senseIds[i];
        reported = _senseAtMs != 0;
        on       = _senseOn[i];
        ageMs    = reported ? (uint32_t)(millis() - _senseAtMs) : 0;
        level    = _senseLevel;
        return true;
    }

    // Is anything actually watching? The sketch skips the whole sampling window
    // when nothing is configured — the read busy-waits, and a board with no
    // clamp in the layout should not spend 60 ms in four times a second.
    bool sensesAnything() const { return _senseCount > 0; }

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

    // Sized by the same cap the wire uses, so a layout that a NODE would refuse
    // cannot be silently truncated here instead.
    char     _senseIds[nodelink::kMaxSensorsPerNode][nodelink::kMaxSensorIdLen] = {};
    bool     _senseOn[nodelink::kMaxSensorsPerNode] = { false };
    size_t   _senseCount = 0;
    uint32_t _senseAtMs  = 0;   // 0 = nothing measured yet
    float    _senseLevel = -1.0f;   // multiple of the trip point; diagnostic only
};

} // namespace topo

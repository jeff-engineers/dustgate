// =============================================================================
// LimitSwitchDistance.cpp — see the header. Ported from firmware/attic/linear/
// on 2026-08-28 with its logic intact.
// =============================================================================

#include "LimitSwitchDistance.h"

#if HAS_LINEAR

#include "../utils/MotionMath.h"

// How far the carriage may travel trying to get OFF a triggered datum switch
// before we call the switch a liar. A real switch releases within its own
// over-travel — a fraction of a millimetre of contact movement plus whatever
// the carriage overshot by. 15mm is generous for that and still far short of
// the gap to gate 1, so a release that runs this long is not a mechanical
// situation, it is a wiring one.
static const float kMaxReleaseMm = 15.0f;

LimitSwitchDistance::LimitSwitchDistance()
    : _motor(nullptr), _homed(false), _backingOff(false),
      _phase(RELEASING), _releaseStart(0), _triggerPos(0),
      _backoffUsed(HOME_BACKOFF_STEPS), _failed(false), _failure("")
{}

void LimitSwitchDistance::resetHoming() {
    _homed      = false;
    _backingOff = false;
    _phase      = RELEASING;
    _releaseStart = 0;
    _triggerPos   = 0;
    _backoffUsed  = HOME_BACKOFF_STEPS;
    _failed     = false;
    _failure    = "";
}

bool LimitSwitchDistance::begin(MotorDriver* motor) {
    _motor = motor;

    // NC switches wired between pin and GND — see the board header for why that
    // choice is a safety feature and not a preference.
    pinMode(PIN_ENDSTOP_HOME, INPUT_PULLUP);
    pinMode(PIN_ENDSTOP_MAX, INPUT_PULLUP);

    // Print the pin numbers, not nicknames: this used to say "D10/D11" from the
    // DevKitC, which is wrong on every board that has since carried it.
    DEBUG_PRINT(F("[ENDSTOP] home (GPIO")); DEBUG_PRINT(PIN_ENDSTOP_HOME);
    DEBUG_PRINT(F("): ")); DEBUG_PRINT(readHomeSwitch() ? F("TRIGGERED") : F("open"));
    DEBUG_PRINT(F("   max (GPIO")); DEBUG_PRINT(PIN_ENDSTOP_MAX);
    DEBUG_PRINT(F("): ")); DEBUG_PRINTLN(readMaxSwitch() ? F("TRIGGERED") : F("open"));

    // BOTH TRIGGERED AT BOOT IS NOT A CARRIAGE IN TWO PLACES. It is the reading
    // a missing ground or an unplugged loom gives, because an open input with a
    // pullup is indistinguishable from an open NC contact. Saying so here costs
    // one line and saves the hunt for a mechanical fault that isn't there.
    if (readHomeSwitch() && readMaxSwitch()) {
        Serial.println(F("[ENDSTOP] ⚠ BOTH switches read triggered. A carriage cannot be at both"));
        Serial.println(F("          ends — check the endstop ground and the connectors before homing."));
    }
    return true;
}

bool LimitSwitchDistance::updateHoming() {
    if (_homed) return true;
    if (_failed) return false;

    const bool datumTriggered = g_homeIsMaxEndstop ? readMaxSwitch() : readHomeSwitch();

    // ── RELEASING ───────────────────────────────────────────────────────────
    // Before seeking the datum, make sure we are not already sitting on it.
    //
    // THIS PHASE IS WHY THE FILE GREW. Without it, a datum that reads triggered
    // on the first pass sent homing straight to "found it, back off, done" — so
    // an unwired switch (NC + INPUT_PULLUP reads HIGH = triggered) produced a
    // confident datum a couple of millimetres from wherever the carriage
    // happened to start. It looked like success and it was noticed only because
    // the carriage visibly stopped after 2mm.
    if (_phase == RELEASING) {
        if (!datumTriggered) {
            // Clear. THIS is where the sweep starts — not in the sketch's
            // startHoming(), which used to command it before anyone had checked
            // whether the carriage was sitting on the switch already.
            //
            // That ordering was broken in a way specific to this servo: a new
            // command DOES NOT SUPERSEDE the one in flight, so the release move
            // could not cancel a sweep that had already been issued. Pressing
            // `home` with the datum made drove the carriage harder into it. The
            // sweep now starts only once there is somewhere to sweep from.
            _phase = SEEKING;
            _motor->startHoming();
            return false;
        }

        if (_releaseStart == 0) {
            // We ARE on the switch. Legitimate — the carriage is parked where
            // the last session left it, or the last home ended here.
            //
            // CANCEL FIRST. Anything already in flight is heading INTO this
            // switch, and on this part the only cancel is cutting torque; a
            // moveTo() on its own would just queue behind it.
            _motor->stop();

            _releaseStart = _motor->getPosition();
            if (_releaseStart == 0) _releaseStart = 1;   // 0 is our "not started" marker
            Serial.println(F("[ENDSTOP] datum reads TRIGGERED before the sweep started —"));
            Serial.println(F("          backing off it first. (Same either way round: the datum"));
            Serial.println(F("          is whichever switch g_homeIsMaxEndstop names, and the"));
            Serial.println(F("          release direction is derived from it.)"));
            _motor->moveTo(_motor->getPosition()
                           + (long)(kMaxReleaseMm * stepsPerMM()) * (-HOME_DIRECTION));
            return false;
        }

        // Still triggered. Either we are mid-release, or the switch is never
        // going to clear.
        if (!_motor->isMoving()) {
            // The release move ran its whole length and the switch never opened.
            _failed  = true;
            _failure = "datum endstop reads TRIGGERED and will not clear";
            Serial.println(F("[ENDSTOP] ✗ HOMING FAILED — the datum switch still reads"));
            Serial.print(F("          TRIGGERED after backing off "));
            Serial.print(kMaxReleaseMm, 0);
            Serial.println(F("mm. It is not the carriage."));
            Serial.println(F("          These switches are NORMALLY CLOSED to GND with"));
            Serial.println(F("          INPUT_PULLUP, so an OPEN circuit reads as triggered —"));
            Serial.println(F("          which is the fail-safe, and also exactly what an"));
            Serial.println(F("          unwired, unplugged or ungrounded switch looks like."));
            Serial.println(F("          Check, in order: is anything wired to the pins at all;"));
            Serial.println(F("          is the switch's common ground connected; is it wired"));
            Serial.println(F("          to the NC contact and not NO. Type 'e' to watch both."));
        }
        return false;
    }

    // ── BACKING_OFF ─────────────────────────────────────────────────────────
    //
    // BACK OFF UNTIL THE SWITCH RELEASES, not a fixed distance. A nominal
    // HOME_BACKOFF_STEPS assumes the contact opens within that travel, and on
    // 2026-08-28 it did not: homing "finished" with the datum switch still
    // reading TRIGGERED, which then blocked the calibration sweep (the
    // over-travel supervisor refuses travel INTO a made switch) and gave the
    // span math a datum that was really still on the switch. Every gate placed
    // from that span was wrong.
    //
    // The distance actually travelled is recorded in _backoffUsed and read back
    // by the span math, so extending it stays consistent instead of silently
    // shortening the measured span.
    if (_backingOff) {
        if (_motor->isMoving()) return false;

        if (datumTriggered) {
            // Still on the switch. Extend, up to the same bound the release
            // phase uses — past that it is not a switch with long hysteresis,
            // it is a switch that is not working.
            const long travelled = labs(_motor->getPosition() - _triggerPos);
            if (travelled >= (long)(kMaxReleaseMm * stepsPerMM())) {
                _failed  = true;
                _failure = "datum switch never released while backing off";
                Serial.println(F("[ENDSTOP] ✗ HOMING FAILED — backed off the datum switch by"));
                Serial.print(F("          "));
                Serial.print(kMaxReleaseMm, 0);
                Serial.println(F("mm and it still reads TRIGGERED."));
                Serial.println(F("          A switch that never opens reads the same as one that"));
                Serial.println(F("          is not wired (NC + INPUT_PULLUP). Type 'e' to watch it."));
                return false;
            }
            _motor->moveTo(_motor->getPosition() + HOME_BACKOFF_STEPS * (-HOME_DIRECTION));
            DEBUG_PRINTLN(F("[ENDSTOP] still on the datum switch — backing off further..."));
            return false;
        }

        // Released. THIS is the datum, and how far it is from the trigger point
        // is what the span math needs.
        _backoffUsed = labs(_motor->getPosition() - _triggerPos);
        _motor->setHome();
        _homed = true;
        _backingOff = false;
        _phase = DONE;
        DEBUG_PRINT(F("[ENDSTOP] homing complete — datum set, "));
        DEBUG_PRINT(_backoffUsed);
        DEBUG_PRINTLN(F(" steps off the trigger point."));
        return true;
    }

    // ── SEEKING ─────────────────────────────────────────────────────────────
    // Driving toward the HOME DATUM (the user's-left endstop, whichever physical
    // switch that is — g_homeIsMaxEndstop). Stop when that switch triggers. The
    // FAR switch firing instead means the motor is wired backwards; that is
    // detected in the main loop's homing state, not here.
    if (datumTriggered) {
        _motor->stop();
        delay(20);

        // Confirm it after the stop. One re-read costs nothing and rejects a
        // single bounce from carriage vibration — the same reason the far switch
        // is debounced over three loops during the calibration sweep.
        const bool stillTriggered = g_homeIsMaxEndstop ? readMaxSwitch() : readHomeSwitch();
        if (!stillTriggered) {
            DEBUG_PRINTLN(F("[ENDSTOP] datum blipped and cleared — ignoring, still seeking."));
            _motor->startHoming();
            return false;
        }

        // Backoff is RELATIVE to the trigger position. moveTo(absolute) from an
        // arbitrary position would crash the carriage.
        _triggerPos = _motor->getPosition();
        long backoffTarget = _triggerPos + HOME_BACKOFF_STEPS * (-HOME_DIRECTION);
        _motor->moveTo(backoffTarget);
        _backingOff = true;

        DEBUG_PRINTLN(F("[ENDSTOP] home datum hit, backing off..."));
    }

    return false;
}

bool LimitSwitchDistance::updateMoving(int /*targetStop*/) {
    // Over-travel is enforced directionally by the main-loop endstop supervisor
    // (stops travel INTO a triggered switch, allows travel away — needed so a
    // move can leave the far switch, e.g. returning home after a sweep). Here we
    // only report arrival: true once the carriage has reached target or been
    // stopped.
    return !_motor->isMoving();
}

long LimitSwitchDistance::stepsForStop(int stopIndex) {
    // Negate by HOME_DIRECTION: with HOME_DIRECTION=1, gates are in the negative
    // step direction from home (the carriage backs away from the datum toward
    // the gates).
    return ::mmToSteps(g_stopPositionsMM[stopIndex]) * (-HOME_DIRECTION);
}

bool LimitSwitchDistance::readHomeSwitch() {
    // NC switch wired between pin and GND, INPUT_PULLUP:
    //   Normal (contacts closed): pin pulled to GND → LOW
    //   Triggered (contacts open): pullup wins → HIGH
    // Also fail-safe: broken wire → pin HIGH → reads as triggered → carriage stops.
    return digitalRead(PIN_ENDSTOP_HOME) == HIGH;
}

bool LimitSwitchDistance::readMaxSwitch() {
    // Same NC + INPUT_PULLUP wiring as the home switch: triggered (contacts
    // open) → pin pulled HIGH; also fail-safe.
    // (Was `== LOW` once, which is inverted — it only appeared to work while no
    // max switch was installed and the floating pin read HIGH. With a real NC
    // switch that inversion reads "triggered" whenever the switch is normal.)
    return digitalRead(PIN_ENDSTOP_MAX) == HIGH;
}

#endif // HAS_LINEAR

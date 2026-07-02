// =============================================================================
// SensorlessHoming.cpp
// =============================================================================

#include "SensorlessHoming.h"
#include "../utils/MotionMath.h"

#if defined(MOTOR_STEPPER_TMC2209) && defined(FEEDBACK_SENSORLESS)

SensorlessHoming::SensorlessHoming()
    : _motor(nullptr), _homed(false), _stallDetected(false), _homingStartMs(0), _homingStartPos(0)
{}

void SensorlessHoming::resetHoming() {
    _homed          = false;
    _stallDetected  = false;
    _homingStartMs  = 0;
    _homingStartPos = 0;
}

bool SensorlessHoming::begin(MotorDriver* motor) {
    _motor = static_cast<StepperTMC2209Driver*>(motor);
    if (!_motor) {
        DEBUG_PRINTLN(F("SensorlessHoming: invalid motor pointer"));
        return false;
    }
    DEBUG_PRINTLN(F("SensorlessHoming initialized."));
    return true;
}

bool SensorlessHoming::updateHoming() {
    if (_homed) return true;

    if (_homingStartMs == 0) {
        _homingStartMs = millis();
        _homingStartPos = _motor->getPosition();
    }

    // Force-home if motor has traveled the full rack length + margin without a stall.
    // Covers cases where SGTHRS is too low to detect the endstop.
    long traveled = abs(_motor->getPosition() - _homingStartPos);
    long maxSteps  = (long)(HOMING_MAX_TRAVEL_MM * stepsPerMM());
    if (traveled >= maxSteps) {
        _motor->stop();
        delay(50);
        // Backoff is RELATIVE to where we stopped — not absolute 0.
        // Without this, moveTo(HOME_BACKOFF_STEPS * -1) from position ~5000
        // sends the carriage flying across the full rack to the right endstop.
        long backoffTarget = _motor->getPosition() + HOME_BACKOFF_STEPS * (-HOME_DIRECTION);
        _motor->moveTo(backoffTarget);
        while (_motor->isMoving()) { _motor->update(); }
        _motor->setHome();
        _homed = true;
        _homingStartMs = 0;
        DEBUG_PRINTLN(F("Homing: max travel reached — position forced to home."));
        return true;
    }

    // Wait for motor to reach homing speed before checking stall
    if (millis() - _homingStartMs < STALL_GUARD_SETTLE_MS) {
        return false;
    }

    // Log every 2000ms — reduces UART read impact on AccelStepper step timing.
    // At 500 steps/sec each step is 2ms; a 1ms UART read is 50% jitter per step,
    // which can prevent the TMC2209 StallGuard velocity filter from completing.
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 2000) {
        lastPrint = millis();
        uint32_t drv        = _motor->getDrvStatus();
        bool     diag       = _motor->isStalled();
        uint16_t sg_result  = drv & 0x1FF;           // bits [8:0] TMC2209 SG_RESULT (9-bit)
        uint8_t  cs_actual  = (drv >> 16) & 0x1F;    // bits [20:16] CS_ACTUAL
        bool     stealth    = (drv >> 30) & 0x01;    // 1=StealthChop, 0=SpreadCycle
        bool     ola        = (drv >> 24) & 0x01;    // open load phase A
        bool     olb        = (drv >> 25) & 0x01;    // open load phase B
        uint32_t tstep      = _motor->getTStep();
        DEBUG_PRINT(F("DRV:0x")); Serial.print(drv, HEX);
        DEBUG_PRINT(F(" TSTEP:")); Serial.print(tstep);
        DEBUG_PRINT(F(" SG:")); Serial.print(sg_result);
        DEBUG_PRINT(F(" CS:")); Serial.print(cs_actual);
        DEBUG_PRINT(F(" mode:")); Serial.print(stealth ? F("StlthChop!") : F("SpreadCyc"));
        if (ola || olb) { DEBUG_PRINT(F(" OL:")); if (ola) DEBUG_PRINT(F("A")); if (olb) DEBUG_PRINT(F("B")); DEBUG_PRINT(F("(wiring!)")); }
        DEBUG_PRINT(F(" DIAG:")); Serial.println(diag ? F("HIGH(stall)") : F("LOW"));
    }

    if (_motor->isStalled()) {
        _motor->stop();
        delay(50); // brief pause before backoff

        // Back off from mechanical stop — relative to stall position, not absolute 0.
        long backoffTarget = _motor->getPosition() + HOME_BACKOFF_STEPS * (-HOME_DIRECTION);
        _motor->moveTo(backoffTarget);

        // Wait for backoff to complete
        while (_motor->isMoving()) {
            _motor->update();
        }

        _motor->setHome();
        _homed = true;
        _homingStartMs = 0;
        DEBUG_PRINTLN(F("Sensorless homing complete."));
        return true;
    }

    return false;
}

bool SensorlessHoming::updateMoving(int targetStop) {
    // For stepper with step-based positioning, motion is complete when AccelStepper stops
    return !_motor->isMoving();
}

long SensorlessHoming::stepsForStop(int stopIndex) {
    // Stops are measured from home in the direction away from the endstop.
    // Negate by HOME_DIRECTION so this works regardless of which end home is on.
    return ::mmToSteps(g_stopPositionsMM[stopIndex]) * (-HOME_DIRECTION);
}

bool SensorlessHoming::isHomeTriggered() {
    return _motor->isStalled(); // approximate — only valid during homing
}

bool SensorlessHoming::isMaxTriggered() {
    return false; // no dedicated max endstop in sensorless mode
}

bool SensorlessHoming::isHomed() {
    return _homed;
}

// mmToSteps() now delegated to MotionMath.h

#endif // TMC2209 + SENSORLESS

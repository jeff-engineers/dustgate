// =============================================================================
// StepperTMC2209Driver.cpp
// =============================================================================

#include "StepperTMC2209Driver.h"

#ifdef MOTOR_STEPPER_TMC2209

// Serial1 = hardware UART on ESP32-S2 Feather RX/TX header pins.
// Wire: Feather TX → 1kΩ → TMC2209 UART pin; Feather RX → same node (after resistor).

StepperTMC2209Driver::StepperTMC2209Driver()
    : _driver(&Serial1, TMC2209_R_SENSE, (uint8_t)TMC2209_ADDRESS),
      _stepper(AccelStepper::DRIVER, PIN_TMC_STEP, PIN_TMC_DIR),
      _homing(false),
      _enabled(false)
{}

bool StepperTMC2209Driver::begin() {
    // Configure enable pin
    pinMode(PIN_TMC_EN, OUTPUT);
    digitalWrite(PIN_TMC_EN, HIGH); // disabled until configured

    // Init UART — Serial1 is the hardware UART on Feather RX/TX header pins
    Serial1.begin(115200);

    // Configure driver
    _driver.begin();
    // I_scale_analog defaults to 1 on power-on: uses external VREF (pot on Adafruit board).
    // Clearing it forces UART current control via rms_current(), which is required for
    // StallGuard to work — without adequate current, SG_RESULT is 0 on every read.
    _driver.I_scale_analog(false);
    _driver.internal_Rsense(false);             // Use external sense resistors (R_SENSE = 0.11Ω)
    _driver.toff(5);                            // Enable driver (must be > 0)
    // rms_current(run_mA, holdMultiplier): holdMultiplier = holdMA / runMA
    // Without the second arg the library defaults to 0.5 — ignores TMC2209_HOLD_CURRENT_MA.
    float holdMult = (float)TMC2209_HOLD_CURRENT_MA / (float)TMC2209_CURRENT_MA;
    _driver.rms_current(TMC2209_CURRENT_MA, holdMult);
    _driver.microsteps(MICROSTEPS);
    _driver.en_spreadCycle(false);              // StealthChop (quiet)
    _driver.pwm_autoscale(true);

    // -------------------------------------------------------------------------
    // UART communication health check — two stages.
    //
    // Stage 1 (READ):      Read the version register.
    //                      0x00 or 0xFF = no response at all.
    //                      Causes: TX not reaching UART pin, wrong baud, wrong
    //                      address, VDD missing, or UART pin not connected.
    //
    // Stage 2 (WRITE→READ): Write a canary byte to SGTHRS, read it back.
    //                      Mismatch = ESP32→TMC2209 (TX) path broken while
    //                      TMC2209→ESP32 (RX) path works, or vice versa.
    //                      Causes: resistor value wrong, TX/RX swapped,
    //                      or potentiometer drift off 1kΩ.
    //
    // Both stages must pass before the motor is enabled. The system halts in
    // STATE_ERROR otherwise — motor will not move until wiring is fixed and
    // the board is reset.
    //
    // Fix checklist:
    //   • Feather TX → 1kΩ resistor → TMC2209 UART pin
    //   • Feather RX  → same node (junction between resistor and UART pin)
    //   • VDD on TMC2209 board → Feather 3V3 pin
    //   • TMC2209_ADDRESS in config.h matches MS1/MS2 state (Adafruit board default = 0)
    // -------------------------------------------------------------------------
    uint8_t version = _driver.version();
    if (version == 0x00 || version == 0xFF) {
        Serial.println(F(""));
        Serial.println(F("!!! TMC2209 UART FAIL (Stage 1 — no read response)"));
        Serial.print  (F("!!! version() returned 0x")); Serial.println(version, HEX);
        Serial.println(F("!!! Check: TX→1kΩ→UART pin, RX to same node, VDD=3.3V, address."));
        Serial.println(F("!!! System halted — fix wiring and reset."));
        Serial.println(F(""));
        return false;
    }
    DEBUG_PRINT(F("TMC2209 version: 0x")); Serial.println(version, HEX);

    // Stage 2: write a canary, read it back to confirm ESP32→TMC2209 path.
    const uint8_t CANARY = 0xA5;
    _driver.SGTHRS(CANARY);
    uint8_t readback = (uint8_t)_driver.SGTHRS();
    if (readback != CANARY) {
        Serial.println(F(""));
        Serial.println(F("!!! TMC2209 UART FAIL (Stage 2 — write→read mismatch)"));
        Serial.print  (F("!!! Wrote 0xA5, read back 0x")); Serial.println(readback, HEX);
        Serial.println(F("!!! TX path likely broken: check 1kΩ resistor (not pot) and TX pin."));
        Serial.println(F("!!! System halted — fix wiring and reset."));
        Serial.println(F(""));
        return false;
    }
    _driver.SGTHRS(0); // clear canary; real value set during homing
    DEBUG_PRINTLN(F("TMC2209 UART OK (read + write verified)."));

    // DIAG pin: open-drain active LOW — pulls to GND on stall, floats otherwise.
    // INPUT_PULLUP required; without it the pin sits at 0V always.
    pinMode(PIN_TMC_DIAG, INPUT_PULLUP);

    // Configure AccelStepper
    _stepper.setMaxSpeed(MAX_SPEED_STEPS_PER_SEC);
    _stepper.setAcceleration(ACCELERATION_STEPS_PER_SEC2);

    // Enable driver
    enable(true);

    DEBUG_PRINTLN(F("StepperTMC2209Driver initialized."));
    return true;
}

void StepperTMC2209Driver::startHoming() {
    startHomingWithParams(HOMING_SPEED_STEPS_PER_SEC, TMC2209_STALL_THRESHOLD);
}

void StepperTMC2209Driver::startHomingWithParams(float speedStepsPerSec, uint8_t stallThreshold) {
    _homing = true;
    _stepper.setSpeed(speedStepsPerSec * HOME_DIRECTION);
    DEBUG_PRINT(F("Homing started — speed: ")); Serial.print(speedStepsPerSec, 0);
    DEBUG_PRINT(F(" steps/sec, SGTHRS: ")); Serial.println(stallThreshold);
}

void StepperTMC2209Driver::moveTo(long targetSteps) {
    _homing = false;
    _stepper.moveTo(targetSteps);
    DEBUG_PRINT(F("Moving to step: "));
    DEBUG_PRINTLN(targetSteps);
}

void StepperTMC2209Driver::setMaxSpeed(float speedStepsPerSec) {
    _stepper.setMaxSpeed(speedStepsPerSec);
}

void StepperTMC2209Driver::stop() {
    // IMMEDIATE stop — zero the remaining distance so run()/runSpeed() halts this
    // instant. AccelStepper::stop() only sets a DECELERATION target, which keeps
    // stepping toward the original target while slowing down — it coasts PAST a
    // hard limit (endstop) and isn't what an e-stop or limit hit wants. Setting
    // the current position to itself makes distanceToGo()==0 and zeroes speed
    // while preserving the coordinate.
    _stepper.setCurrentPosition(_stepper.currentPosition());
    _homing = false;
}

void StepperTMC2209Driver::update() {
    if (!_enabled) return;

    if (_homing) {
        _stepper.runSpeed();
    } else {
        _stepper.run();
    }
}

bool StepperTMC2209Driver::isMoving() {
    return (_homing || _stepper.distanceToGo() != 0);
}

long StepperTMC2209Driver::distanceToGo() {
    return _stepper.distanceToGo();
}

long StepperTMC2209Driver::getPosition() {
    return _stepper.currentPosition();
}

void StepperTMC2209Driver::setHome() {
    _stepper.setCurrentPosition(0);
    _homing = false;
    DEBUG_PRINTLN(F("Home position set."));
}

void StepperTMC2209Driver::enable(bool on) {
    _enabled = on;
    digitalWrite(PIN_TMC_EN, on ? LOW : HIGH); // Active LOW
}

void StepperTMC2209Driver::printDriverRegs() {
    // Read registers that are genuinely readable from hardware.
    // If these don't match what we wrote in begin()/startHomingWithParams(),
    // writes are not reaching the driver — check the 1kΩ UART resistor.

    uint32_t gconf    = _driver.GCONF();
    uint32_t chopconf = _driver.CHOPCONF();
    uint32_t ioinput  = _driver.IOIN();
    uint32_t tcool    = _driver.TCOOLTHRS();
    uint8_t  sgthrs   = (uint8_t)_driver.SGTHRS();
    uint32_t drv      = _driver.DRV_STATUS();

    Serial.println(F(""));
    Serial.println(F("=== TMC2209 Register Dump ==="));

    Serial.print(F("  GCONF:      0x")); Serial.println(gconf, HEX);
    bool spreadCycle = (gconf >> 2) & 0x01;
    bool pdn_disable = (gconf >> 6) & 0x01;
    Serial.print(F("    en_spreadCycle = ")); Serial.println(spreadCycle ? F("1 (SpreadCycle — GOOD for StallGuard)") : F("0 (StealthChop — StallGuard INACTIVE)"));
    Serial.print(F("    pdn_disable    = ")); Serial.println(pdn_disable ? F("1 (UART active)") : F("0 (PDN pin controls mode — check MS pins)"));

    Serial.print(F("  CHOPCONF:   0x")); Serial.println(chopconf, HEX);
    uint8_t toff      = chopconf & 0x0F;
    uint8_t msteps    = (chopconf >> 24) & 0x0F;
    Serial.print(F("    toff       = ")); Serial.println(toff);
    Serial.print(F("    mstep exp  = ")); Serial.print(msteps); Serial.print(F("  (divisor = ")); Serial.print(1 << (8 - msteps)); Serial.println(F(")"));

    Serial.print(F("  TCOOLTHRS:  0x")); Serial.println(tcool, HEX);
    Serial.println(tcool > 0 ? F("    (StallGuard speed window active)") : F("    WARNING: 0 — StallGuard disabled! Write not landing?"));
    Serial.print(F("  SGTHRS:     ")); Serial.print(sgthrs);
    Serial.println(sgthrs > 0 ? F("  (write confirmed)") : F("  WARNING: 0 — write may not have landed!"));

    Serial.print(F("  IOIN:       0x")); Serial.println(ioinput, HEX);
    Serial.print(F("    VERSION    = 0x")); Serial.println((ioinput >> 24) & 0xFF, HEX);

    Serial.print(F("  DRV_STATUS: 0x")); Serial.println(drv, HEX);
    uint16_t sg_result  = drv & 0x1FF;          // bits [8:0]
    uint8_t  cs_actual  = (drv >> 16) & 0x1F;   // bits [20:16] = CS_ACTUAL
    bool     stst        = (drv >> 31) & 0x01;
    bool     stealth_bit = (drv >> 30) & 0x01;
    bool diag_state = digitalRead(PIN_TMC_DIAG);
    Serial.print(F("    SG_RESULT  = ")); Serial.print(sg_result);
    Serial.println(F("  (0=max stall load, 511=no load; DIAG fires when < SGTHRS*2)"));
    Serial.print(F("    CS_ACTUAL  = ")); Serial.print(cs_actual);
    Serial.print(F(" / 31  ("));
    Serial.print((uint32_t)cs_actual * 100 / 31); Serial.println(F("% of run current)"));
    Serial.print(F("    standstill = ")); Serial.println(stst);
    Serial.print(F("    stealth    = ")); Serial.println(stealth_bit ? F("1 (StealthChop — StallGuard INACTIVE)") : F("0 (SpreadCycle — StallGuard ok)"));
    Serial.print(F("  DIAG pin:   ")); Serial.println(diag_state ? F("HIGH (stall asserted)") : F("LOW (normal)"));

    Serial.println(F("============================="));
    Serial.println(F(""));
}

#endif // MOTOR_STEPPER_TMC2209

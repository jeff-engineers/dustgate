// =============================================================================
// MotionMath.h — Shared unit conversion utilities
//
// g_stopPositionsMM[] is the single runtime source of truth for stop positions.
// It is populated at startup in firmware.ino from either:
//   - EEPROM (if valid calibration exists), or
//   - STOP_DISTANCES_MM in config.h (fallback)
// After training completes, it is updated in-place so normal operation
// immediately uses the new values without requiring a reflash.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "../config.h"

// Declared here, defined in firmware.ino
extern float g_stopPositionsMM[NUM_STOPS + 1];

// Highest stop index actually saved via /api/setstop (or loaded from valid
// EEPROM calibration) since boot or the last "Start Over". Indices beyond
// this are unset — g_stopPositionsMM[] still holds a value for them (either
// 0.0 after a reset, or an extrapolated default while loading calibration),
// but that value was never explicitly trained and must be reported as
// "not yet saved" (JSON null), not as a real position at that mm. Without
// this distinction, a freshly-reset device (all positions read 0.00mm) gets
// misread by the setup wizard's "too close to an existing gate" conflict
// check as every untrained gate already sitting saved at 0mm.
extern int g_numTrainedStops;

// Dual-endstop calibration + port roles (defined in firmware.ino, loaded
// from CalibrationData). See docs/dual-endstop-calibration.md.
extern uint8_t g_stopRoles[NUM_STOPS + 1];  // PortRole per stop (0 = home)
extern float   g_measuredStepsPerMM;        // calibrated steps/mm (0 = not calibrated)
extern long    g_measuredSpanSteps;         // near→far span in steps (0 = not calibrated)
extern char    g_manifoldModel[16];         // "rockler-2.5" | "rockler-4" | "custom"
extern bool    g_homeIsMaxEndstop;          // which endstop is the home datum (= user's
                                            //   LEFT): false = PIN_ENDSTOP_HOME, true =
                                            //   PIN_ENDSTOP_MAX. Homing always drives to
                                            //   this one. (Said "D10/D11" until 2026-08-28
                                            //   — the DevKitC's labels, and the XIAO C5
                                            //   has no D11 pad at all. The board header
                                            //   is the only thing that knows the pins.)

// Steps ↔ mm conversion using config.h gear parameters.
//
// A "STEP" IS WHATEVER THE BOARD'S DRIVE COUNTS IN, and the two are an order of
// magnitude apart:
//
//   ST3215 slider   4096 encoder counts / 165.8mm = 24.70 counts/mm
//   retired stepper (200 × 16) microsteps / 62.175mm = 51.47 microsteps/mm
//
// Putting the branch HERE rather than at the call sites is what let the ST3215
// slide in behind the existing seam: every mm↔step conversion in the sketch, the
// homing sweep, the stop table and the calibration store kept working with the
// units swapped underneath them. The cost is that a bare "steps" in a log line
// means different distances on different boards — which is why the driver prints
// mm alongside counts.
//
// A BOARD WITH NO RACK RETURNS 1. It used to compute the retired stepper's
// 51.47 microsteps/mm, which was a real-looking number for hardware that does
// not exist on any target: the conversions it feeds — mmToSteps, stepsForStop,
// the whole stop table — are only ever reached through NullMotorDriver and
// NullFeedback, which move nothing and answer 0. One is the honest scale for a
// unit that has no length, and it keeps the arithmetic from producing plausible
// distances nobody can travel.
inline float stepsPerMM() {
#if HAS_LINEAR
    return ST3215_COUNTS_PER_MM;
#else
    return 1.0f;
#endif
}

inline long mmToSteps(float mm) {
    return (long)(mm * stepsPerMM());
}

inline float stepsToMM(long steps) {
    return (float)steps / stepsPerMM();
}

// Canonical step position for a given stop index, using runtime calibration
inline long stepsForStop(int stopIndex) {
    return mmToSteps(g_stopPositionsMM[stopIndex]);
}

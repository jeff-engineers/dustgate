// =============================================================================
// tmc2209-params.h — ATTIC. Not compiled, not included by anything.
//
// These lived in firmware/config.h until 2026-08-23. Nothing outside
// StepperTMC2209Driver.cpp ever read them, so they came out with it rather than
// sitting in the live config describing a driver no board has. Kept verbatim:
// the current and standstill numbers are bench-tuned, and re-deriving them is
// exactly the kind of work this attic exists to avoid. See README.md.
// =============================================================================

#pragma once

// UART address (0–3, set by MS1/MS2 pins — Adafruit board default is 0)
#define TMC2209_ADDRESS            0

// -----------------------------------------------------------------------------
// TMC2209 PARAMETERS
// Adafruit TMC2209 Breakout (#6121) specifics:
//   - R_SENSE: 0.11Ω (verify on your board — check silkscreen or schematic)
//   - VDD: connect to 3.3V (Feather 3V3 pin) — board supports 3.3–5V logic
//   - Current pot: hardware ceiling; UART current setting cannot exceed pot limit
//   - UART: single-wire half-duplex on the board's "UART" pin
// -----------------------------------------------------------------------------
#define TMC2209_R_SENSE         0.11f   // Sense resistor (Ω) — verify on your board
#define TMC2209_CURRENT_MA       800    // Run current in mA — raise if stalls mid-travel
#define TMC2209_HOLD_CURRENT_MA   30    // Hold current — motor held between moves (low = cool).
                                        // Heat ~ I²: 30mA dissipates ~16% of what 75mA did. The
                                        // gate's rack-and-pinion friction holds position at idle;
                                        // set to 0 for a fully-freewheeling (coolest) standstill if
                                        // a little drift between moves is acceptable.

// Standstill power-down delay: clocks after the last step before the driver
// drops from run current (IRUN) to hold current (IHOLD). Set explicitly so the
// transition is guaranteed to engage promptly — otherwise a motor can linger at
// run current between moves and run hot even though the hold current is low.
// Range 0–255 (~0–5.6s); ~0.2s here.
#define TMC2209_TPOWERDOWN        10

// StallGuard threshold — not used for homing (physical limit switch) but left
// as a safety floor; TMC2209 still raises DIAG on severe overload/stall.
#define TMC2209_STALL_THRESHOLD   50

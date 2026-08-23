// =============================================================================
// sources.cpp — Unity build shim for Arduino IDE
//
// Arduino IDE only compiles .cpp files that sit directly in the sketch folder.
// Subdirectory source files are silently ignored, causing "undefined reference"
// linker errors for every class they define.
//
// This file fixes that by #including each subdirectory .cpp explicitly.
// The #ifdef guards inside each file ensure only the selected modules
// contribute symbols — the rest compile to nothing.
//
// DO NOT add this file to PlatformIO / arduino-cli builds; those tools
// handle subdirectory compilation automatically and will get duplicate symbols.
// =============================================================================

// The stepper and the limit-switch feedback moved to attic/linear/ on
// 2026-08-23 and are not compiled by anything. Nothing replaces them here: the
// sketch takes NullMotorDriver / NullFeedback, which are header-only.

#include "control/SerialDebugControl.cpp"
#include "control/SmartOutletControl.cpp"

#include "training/CalibrationStore.cpp"

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

#include "motor/StepperTMC2209Driver.cpp"

#include "feedback/SensorlessHoming.cpp"
#include "feedback/LimitSwitchDistance.cpp"
#include "feedback/LimitSwitchDetent.cpp"

#include "control/RotaryControl.cpp"
#include "control/SerialDebugControl.cpp"
#include "control/WiFiControl.cpp"
#include "control/SmartOutletControl.cpp"
#include "control/AppControl.cpp"

#include "output/RelayOutput.cpp"

#include "training/CalibrationStore.cpp"
#include "training/TrainingMode.cpp"
#include "training/AutoTuner.cpp"

// =============================================================================
// SerialDebugControl.h — Serial Monitor control for development/testing
// Use instead of the physical rotary + toggle switches while prototyping.
//
// Open Arduino IDE Serial Monitor at SERIAL_BAUD (115200), set line ending to
// "Newline" (bottom-right dropdown), then type commands and press Enter/Send.
//
// No enable/disable concept — the system always runs; only e-stop halts it.
//
// Commands:
//   0-7              Select position (0 = home)
//   estop / stop     Immediate stop — halts motion in place
//   home             Re-trigger homing sequence (resets estop if latched)
//   jog <mm>         Relative move: positive = away from home, negative = toward home
//   gconf            Read GCONF + CHOPCONF registers from driver
//   clearcal         Erase EEPROM calibration
//   provision <json> Write WiFi + Anthropic key to NVS
//   wifireset        Erase WiFi credentials, reboot into setup portal
//   status           Print current state and position
//   discover         Scan mDNS for Shelly outlets (CONTROL_SMART_OUTLET builds only)
//   help             Print this command list
// =============================================================================

#pragma once
#include "ControlInput.h"
#include "../config.h"

#if defined(CONTROL_SERIAL_DEBUG) || defined(ENABLE_SERIAL_COMMANDS)

class SerialDebugControl : public ControlInput {
public:
    SerialDebugControl();
    bool begin() override;
    int  readRequestedStop() override;
    bool isEnabled() override;

    // Returns true once per estop event, then clears the flag.
    // Check this each loop() and transition to STATE_ERROR if true.
    bool consumeEStop();

    // Returns true once per home-request event, then clears the flag.
    bool consumeHomeRequest();

    // Returns true once when user types 'clearcal'.
    bool consumeClearCalRequest();

    // Returns true once when user types 'gconf' — caller should read and print
    // GCONF + CHOPCONF from the driver to verify writes are landing.
    bool consumeGconfRequest();

    // Returns true once when user types 'jog <mm>'. outMM is positive = away from home,
    // negative = toward home. Caller issues the relative move.
    bool consumeJogRequest(float& outMM);

    // Live-tuning values. Read these each homing cycle.
    // -1 means "use config.h default".
    int   stallThreshold() const { return _stallThreshold; }
    float homeSpeed()      const { return _homeSpeed; }

private:
    int  _requestedStop;
    bool _eStopPending;
    bool _homePending;
    bool _clearCalPending;
    bool _gconfPending;
    int   _stallThreshold; // -1 = use config.h default
    float _homeSpeed;      // -1 = use config.h default
    bool  _jogPending;
    float _jogMM;

    String _inputBuffer;

    void processLine(const String& line);
    void printStatus();
    void printHelp();
#ifdef CONTROL_SMART_OUTLET
    void runDiscover();
#endif
};

#endif // CONTROL_SERIAL_DEBUG || ENABLE_SERIAL_COMMANDS

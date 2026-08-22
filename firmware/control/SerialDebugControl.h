// =============================================================================
// SerialDebugControl.h — Serial Monitor control for development/testing
// Drive the actuator by typing commands instead of using the HTTP API / outlets.
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
//   provision <json> Write WiFi credentials + hostname to NVS
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

    // Returns true once when user types 'calibrate <model> <gates>'. Kicks off the
    // dual-endstop reference sweep (same path as POST /api/calibrate).
    bool consumeCalibrateRequest(char* outModel, size_t modelLen, int& outGates);

    // Returns true once when user types 'homeside left|right'. outHomedLeft is the
    // reported side the carriage homed to (same path as POST /api/config/orientation).
    bool consumeHomeSideRequest(bool& outHomedLeft);

    // servo bring-up: 'servo <1-4> <angle>' moves a servo to an angle;
    // 'servo <1-4> detach' de-energizes it. Returns true once per request; caller
    // (the .ino servo bank) drives the actual servo. outDetach true = detach.
    bool consumeServoRequest(int& outIndex, int& outAngle, bool& outDetach);

private:
    int  _requestedStop;
    bool _eStopPending;
    bool _homePending;
    bool _clearCalPending;
    bool _gconfPending;
    bool  _jogPending;
    bool  _calPending;
    char  _calModel[16];
    int   _calGates;
    bool  _homeSidePending;
    bool  _homedLeftValue;
    float _jogMM;
    bool  _servoPending;
    int   _servoIndex;   // 1-based (1..4)
    int   _servoAngle;   // degrees, or ignored when _servoDetach
    bool  _servoDetach;

    String _inputBuffer;

    void processLine(const String& line);
    void printStatus();
    void printHelp();
#ifdef CONTROL_SMART_OUTLET
    void runDiscover();
    // Bus scan for bring-up. Takes the pins explicitly because I2C on an ESP32
    // is remappable and every board here puts it somewhere different — see the
    // command's own comment for why it refuses some of them.
    void runI2cScan(int sda, int scl, bool force = false);
#endif
};

#endif // CONTROL_SERIAL_DEBUG || ENABLE_SERIAL_COMMANDS

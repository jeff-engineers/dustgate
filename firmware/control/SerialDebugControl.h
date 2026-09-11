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

    // `press` — fire the collector's RF transmitter once, by hand.
    //
    // Bypasses the retry policy on purpose. The policy refuses to press in
    // several situations that are correct at runtime and useless at a bench —
    // the cooldown, the spin-up grace, an unreachable sensor — and a person
    // standing next to the collector wants to see the relay click NOW. It is
    // the same escape hatch `dc` gives for a switchable plug.
    bool consumePressRequest();

    // `rfscan` — try the four ways a DIP can be copied wrong, and keep the one
    // the collector answers. SETUP ONLY; see control/RfAddressGuess.h for why
    // this must never be a runtime fallback.
    bool consumeRfScanRequest();

    // `stroke` — drive a servo from one angle to another and back, N times, then
    // detach. THE POINT IS THE MEASUREMENT, not the motion: a servo has no torque
    // feedback, so "can this servo throw that switch" is answered by watching it
    // try, repeatably, with the arm and angles you actually intend to use.
    //
    // Detaching at the end matters. A servo left energised against a switch it
    // could not move sits there stalled, drawing its full stall current and
    // getting hot — which is both a bad measurement and a way to cook a 9 g
    // servo while you go and look at the next machine.
    bool consumeStrokeRequest(int& idx, int& from, int& to, int& reps, int& dwellMs);

    // Returns true once when user types 'reset'. The way back from a latched
    // fault WITHOUT power-cycling the board: the caller re-attempts the drive,
    // clears the boot fault flags and drops the estop. It exists because a
    // serial bus servo can arrive AFTER the board booted — plugging USB in first
    // and the servo lead second is the ordinary bench order — and because a
    // servo that has latched an overload needs its torque cycled to come back.
    bool consumeResetRequest();

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
    bool _pressRequest = false;   // `press` — fire the collector's RF once
    bool _rfScanRequest = false;  // `rfscan` — find the right address by trying
    int  _requestedStop;
    bool _eStopPending;
    bool _homePending;
    bool _resetPending;
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

    // `stroke` — a repeatable press, for finding out whether a servo can throw a
    // given switch. See consumeStrokeRequest().
    bool  _strokePending = false;
    int   _strokeIdx = 0, _strokeFrom = 0, _strokeTo = 0, _strokeReps = 1;
    int   _strokeDwellMs = 400;

    String _inputBuffer;

    void processLine(const String& line);
    void printStatus();
    void printHelp();
#if defined(CONTROL_SMART_OUTLET) || defined(ENABLE_HTTP_API)
    // Bench diagnostic: radio facts plus three long mDNS queries. See the
    // function's comment for what each of the three is there to rule out.
    void runMdnsProbe();
#endif
#ifdef CONTROL_SMART_OUTLET
    void runDiscover();

    // Sweep the local /24 looking for Tasmota plugs. Exists because Tasmota's
    // mDNS is OFF in stock builds, so runDiscover() cannot see one however
    // healthy it is — see the note on the implementation.
    void runSweep(int from, int to);

    // One address, verbosely, saying WHICH step failed. `sweep` can only ever
    // report "nothing found", which is the same answer for an unreachable
    // network, a wrong parse and an empty subnet.
    void runProbe(const String& ip);
    // Bus scan for bring-up. Takes the pins explicitly because I2C on an ESP32
    // is remappable and every board here puts it somewhere different — see the
    // command's own comment for why it refuses some of them.
    void runI2cScan(int sda, int scl, bool force = false);
#endif
};

#endif // CONTROL_SERIAL_DEBUG || ENABLE_SERIAL_COMMANDS

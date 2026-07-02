// =============================================================================
// AppControl.cpp — STUB IMPLEMENTATION
// Currently reads commands from Serial for development/testing.
// Commands: "POS:n" (n = 0-7), "ENABLE", "DISABLE"
// =============================================================================

#include "AppControl.h"

#ifdef CONTROL_APP

AppControl::AppControl()
    : _requestedStop(0), _enabled(false)
{}

bool AppControl::begin() {
    // TODO: Initialize BLE/WiFi module
    // _bleSerial = &Serial2;
    // _bleSerial->begin(9600);

    DEBUG_PRINTLN(F("AppControl: STUB — reading commands from Serial for testing."));
    DEBUG_PRINTLN(F("Commands: POS:n (0-7), ENABLE, DISABLE"));
    return true;
}

int AppControl::readRequestedStop() {
    processIncoming();
    return _requestedStop;
}

bool AppControl::isEnabled() {
    return _enabled;
}

void AppControl::processIncoming() {
    // TODO: swap Serial for _bleSerial when hardware is ready
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        parseCommand(cmd);
    }
}

void AppControl::parseCommand(const String& cmd) {
    if (cmd.startsWith("POS:")) {
        int stop = cmd.substring(4).toInt();
        if (stop >= 0 && stop <= NUM_STOPS) {
            _requestedStop = stop;
            DEBUG_PRINT(F("App command: position "));
            DEBUG_PRINTLN(stop);
        }
    } else if (cmd == "ENABLE") {
        _enabled = true;
        DEBUG_PRINTLN(F("App command: ENABLE"));
    } else if (cmd == "DISABLE") {
        _enabled = false;
        DEBUG_PRINTLN(F("App command: DISABLE"));
    } else {
        DEBUG_PRINT(F("Unknown command: "));
        DEBUG_PRINTLN(cmd);
    }
}

#endif // CONTROL_APP

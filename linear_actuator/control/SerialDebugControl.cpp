// =============================================================================
// SerialDebugControl.cpp
// =============================================================================

#include "SerialDebugControl.h"
#include "../utils/MotionMath.h"
#include "../utils/AgentConfig.h"   // NVS constants + applyProvisionJson() — safe to include always
#if defined(CONTROL_WIFI) || defined(CONTROL_SMART_OUTLET)
  #include "../utils/WiFiProvisioner.h"
#endif

#if defined(CONTROL_SERIAL_DEBUG) || defined(ENABLE_SERIAL_COMMANDS)

SerialDebugControl::SerialDebugControl()
    : _requestedStop(0),
      _eStopPending(false),
      _homePending(false),
      _clearCalPending(false),
      _gconfPending(false),
      _stallThreshold(-1),
      _homeSpeed(-1.0f),
      _jogPending(false),
      _jogMM(0.0f)
{}

bool SerialDebugControl::begin() {
    // Serial already started in setup() via Serial.begin(SERIAL_BAUD)
    printHelp();
    Serial.println(F("[DEBUG] System ready. Type 'home' to home."));
    return true;
}

int SerialDebugControl::readRequestedStop() {
    // Drain Serial into line buffer; process on newline
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            _inputBuffer.trim();
            if (_inputBuffer.length() > 0) {
                processLine(_inputBuffer);
                _inputBuffer = "";
            }
        } else {
            _inputBuffer += c;
        }
    }
    return _requestedStop;
}

bool SerialDebugControl::isEnabled() {
    // No enable/disable concept — the system always runs; only e-stop halts it.
    return true;
}

bool SerialDebugControl::consumeEStop() {
    if (_eStopPending) {
        _eStopPending = false;
        return true;
    }
    return false;
}

bool SerialDebugControl::consumeHomeRequest() {
    if (_homePending) {
        _homePending = false;
        return true;
    }
    return false;
}

bool SerialDebugControl::consumeClearCalRequest() {
    if (_clearCalPending) {
        _clearCalPending = false;
        return true;
    }
    return false;
}

bool SerialDebugControl::consumeGconfRequest() {
    if (_gconfPending) {
        _gconfPending = false;
        return true;
    }
    return false;
}

bool SerialDebugControl::consumeJogRequest(float& outMM) {
    if (_jogPending) {
        _jogPending = false;
        outMM = _jogMM;
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
void SerialDebugControl::processLine(const String& line) {
    Serial.print(F("> "));
    Serial.println(line);

    // Numeric: position selection
    if (line.length() == 1 && isDigit(line[0])) {
        int pos = line[0] - '0';
        if (pos >= 0 && pos <= NUM_STOPS) {
            _requestedStop = pos;
            Serial.print(F("[DEBUG] Position set to: "));
            Serial.println(pos);
        } else {
            Serial.print(F("[DEBUG] Invalid position. Enter 0-"));
            Serial.println(NUM_STOPS);
        }
        return;
    }

    // Text commands (case-insensitive)
    String cmd = line;
    cmd.toLowerCase();

    if (cmd == "estop" || cmd == "stop") {
        _eStopPending = true;
        Serial.println(F("[DEBUG] E-STOP — motion halted. Type 'home' to recover."));

    } else if (cmd == "home") {
        _eStopPending = false;     // clear any latched estop
        _homePending = true;
        Serial.println(F("[DEBUG] Homing requested."));

    } else if (cmd == "gconf") {
        _gconfPending = true;

    } else if (cmd.startsWith("jog ")) {
        float mm = cmd.substring(4).toFloat();
        String jogArg = cmd.substring(4);
        jogArg.trim();
        if (mm == 0.0f && jogArg != "0") {
            Serial.println(F("[JOG] Usage: jog <mm>  e.g. 'jog 10' or 'jog -5'"));
        } else {
            _jogMM = mm;
            _jogPending = true;
            Serial.print(F("[JOG] "));
            Serial.print(mm, 1);
            Serial.println(mm < 0 ? F(" mm toward home") : F(" mm away from home"));
        }

    } else if (cmd.startsWith("provision ")) {
        // provision {"ssid":"...","pass":"...","key":"...","host":"..."}
        // Writes WiFi/key/hostname directly to NVS via the shared helper in
        // AgentConfig.h (also used by the captive portal's own serial
        // listener). Doesn't reboot — this path only runs once WiFi is
        // already connected, so changes apply on the next boot.
        // Use original 'line' (not lowercased 'cmd') to preserve credential case.
        String json = line.substring(10);
        json.trim();
        String errMsg;
        bool wifiSet = WiFiProvisioner::applyProvisionJson(json, &errMsg);
        if (errMsg.length() > 0) {
            Serial.print(F("[PROVISION] JSON parse error: "));
            Serial.println(errMsg);
            Serial.println(F("[PROVISION] Usage: provision {\"ssid\":\"MyNet\",\"pass\":\"pw\",\"key\":\"sk-ant-...\",\"host\":\"dustgate\"}"));
            return;
        }
        if (wifiSet) {
            Serial.println(F("[PROVISION] WiFi credentials saved."));
        }
        Serial.println(F("OK provision"));

    } else if (cmd == "wifireset") {
#if defined(CONTROL_WIFI) || defined(CONTROL_SMART_OUTLET)
        Serial.println(F("[WiFi] Erasing stored credentials and rebooting into setup portal..."));
        delay(200);
        WiFiProvisioner::reset(); // does not return
#else
        Serial.println(F("[WiFi] WiFi not enabled in this build."));
#endif

    } else if (cmd == "clearcal") {
        _clearCalPending = true;
        Serial.println(F("[DEBUG] Calibration erase requested — config.h defaults will be used."));

    } else if (cmd == "status") {
        printStatus();

    } else if (cmd == "help" || cmd == "?") {
        printHelp();

    } else {
        Serial.print(F("[DEBUG] Unknown command: '"));
        Serial.print(line);
        Serial.println(F("' — type 'help' for commands."));
    }
}

void SerialDebugControl::printStatus() {
    Serial.println(F("--- Status ---"));
    Serial.print(F("  Requested stop:    ")); Serial.println(_requestedStop);
    Serial.print(F("  EStop pending:     ")); Serial.println(_eStopPending ? F("YES") : F("no"));
    Serial.print(F("  StallGuard thresh: "));
    if (_stallThreshold < 0) { Serial.print(F("config.h default (")); Serial.print(TMC2209_STALL_THRESHOLD); Serial.println(F(")")); }
    else { Serial.println(_stallThreshold); }
    Serial.print(F("  Homing speed:      "));
    if (_homeSpeed < 0) { Serial.print(F("config.h default (")); Serial.print(HOMING_SPEED_STEPS_PER_SEC, 0); Serial.println(F(" steps/sec)")); }
    else { Serial.print(_homeSpeed, 0); Serial.println(F(" steps/sec")); }
#ifdef FEEDBACK_LIMIT_DISTANCE
    Serial.print(F("  Home endstop:      "));
    Serial.println(digitalRead(PIN_ENDSTOP_HOME) == HIGH ? F("TRIGGERED") : F("open"));
#endif
    Serial.println(F("  Stop positions (from g_stopPositionsMM[]):"));
    for (int i = 0; i <= NUM_STOPS; i++) {
        Serial.print(F("    [")); Serial.print(i); Serial.print(F("]  "));
        Serial.print(g_stopPositionsMM[i], 2); Serial.print(F(" mm  = "));
        long steps = (long)(g_stopPositionsMM[i] * stepsPerMM()) * (-HOME_DIRECTION);
        Serial.print(steps); Serial.println(F(" steps"));
    }
    Serial.println(F("--------------"));
}

void SerialDebugControl::printHelp() {
    Serial.println(F(""));
    Serial.println(F("=== Serial Debug Control ==="));
    Serial.println(F("  0-7               Select position (0=home)"));
    Serial.println(F("  estop             Immediate stop (latches until 'home')"));
    Serial.println(F("  home              Re-trigger homing sequence"));
    Serial.println(F("  jog <mm>          Move relative: + = away from home, - = toward home"));
    Serial.println(F("  clearcal          Erase EEPROM calibration (reload from config.h)"));
    Serial.println(F("  wifireset         Erase WiFi credentials, reboot into setup portal"));
    Serial.println(F("  gconf             Read GCONF + CHOPCONF from driver"));
    Serial.println(F("  status            Print state, stop positions, endstop"));
    Serial.println(F("  provision <json>  Write WiFi+key+host to NVS: {\"ssid\":\"x\",\"pass\":\"y\",\"key\":\"sk-ant-...\",\"host\":\"dustgate\"}"));
    Serial.println(F("  help              Show this list"));
#if defined(CONTROL_SMART_OUTLET) || defined(ENABLE_HTTP_API)
    Serial.println(F("--- Network ---"));
    if (WiFi.status() == WL_CONNECTED) {
        String host = WiFiProvisioner::getHostname();
        Serial.print(F("  Web UI:     http://"));
        Serial.print(host);
        Serial.print(F(".local  (or http://"));
        Serial.print(WiFi.localIP().toString());
        Serial.println(F(")"));
        Serial.print(F("  Setup:      http://"));
        Serial.print(host);
        Serial.println(F(".local/#/setup"));
    } else {
        Serial.println(F("  WiFi not connected."));
        Serial.println(F("  Connect to \"" WIFI_PORTAL_SSID "\" to run first-time setup."));
    }
    Serial.println(F("  wifireset   Forget WiFi credentials, restart setup portal"));
#endif
    Serial.println(F("============================"));
    Serial.println(F(""));
}

#endif // CONTROL_SERIAL_DEBUG || ENABLE_SERIAL_COMMANDS

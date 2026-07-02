// =============================================================================
// WiFiControl.h — Web-based control interface served from the ESP32
//
// Creates a WiFi access point (or joins an existing network) and serves a
// single-page app at http://192.168.4.1 (AP) or the assigned IP (STA).
//
// First-run flow:
//   1. Open the page → Setup view: choose gate count, run autotune
//   2. Autotune result shown → click Save
//   3. Control view: home, select gates, enable/disable
//
// Config in config.h:
//   WIFI_AP_SSID / WIFI_AP_PASS  — hotspot name (AP mode, default)
//   WIFI_STA_SSID / WIFI_STA_PASS — join existing WiFi (optional)
//   WIFI_PORT                    — HTTP port (default 80)
//
// Tip: for WiFi mode, set NUM_STOPS to 7 in config.h so the gate count
// can be chosen at runtime without recompiling.
// =============================================================================

#pragma once
#include "../config.h"

#ifdef CONTROL_WIFI

#include "ControlInput.h"
#include <WiFi.h>
#include <WebServer.h>

class WiFiControl : public ControlInput {
public:
    WiFiControl();

    bool begin()             override;
    int  readRequestedStop() override;
    bool isEnabled()         override;
    void update()            override; // call every loop() — runs WebServer.handleClient()

    // Live-tuning values used by startHoming() after autotune + save.
    // Returns saved Preferences value, or -1 to use config.h defaults.
    int   stallThreshold() const override { return _savedSGTHRS; }
    float homeSpeed()      const override { return _savedSpeed;  }

    // --- Consume methods (same pattern as SerialDebugControl) ---
    bool consumeEStop();
    bool consumeHomeRequest();
    bool consumeTrainRequest()    { return false; }
    bool consumeClearCalRequest() { return false; }
    bool consumeAutotuneRequest();

    // WiFi-specific consumes
    bool consumeSaveRequest();
    bool consumeReconfigureRequest();

    // Gate count requested via web UI. Returns -1 if none pending.
    int  pendingGateCount()    const { return _pendingGateCount; }
    void clearPendingGateCount()     { _pendingGateCount = -1; }

    // Called by main.ino when save is consumed — persists to NVS flash.
    void performSave(int numGates, int sgthrs, float speed);

    // Called by main.ino when reconfigure is consumed — clears saved config.
    void clearConfiguration();

    // Push current system state for the /api/status endpoint.
    // Call once per loop() (cheap — just copies fields).
    void pushStatus(const char* state, int numGates, int currentStop,
                    bool relayOn, bool enabled, bool eStop,
                    bool autotuneRunning, bool autotuneDone,
                    int recSGTHRS, float recSpeed);

private:
    WebServer _server;

    // Command flags set by HTTP handlers, consumed by main.ino
    int  _requestedStop;
    bool _enabled;
    bool _eStopPending;
    bool _homePending;
    bool _autotunePending;
    bool _savePending;
    bool _reconfigPending;
    int  _pendingGateCount;

    // Saved settings (loaded from NVS on begin, updated on performSave)
    bool  _configured;
    int   _savedSGTHRS;  // -1 = use config.h default
    float _savedSpeed;   // -1 = use config.h default

    // Status snapshot written by pushStatus(), read by handleStatus()
    struct StatusSnapshot {
        char  state[20];
        int   numGates;
        int   maxGates;
        int   currentStop;
        bool  relayOn;
        bool  enabled;
        bool  eStop;
        bool  configured;
        bool  autotuneRunning;
        bool  autotuneDone;
        int   recSGTHRS;
        float recSpeed;
    } _snap;

    void startAP();
    void loadPreferences();

    // HTTP route handlers
    void handleRoot();
    void handleStatus();
    void handleCommand();
    void handleNotFound();

    // Tiny JSON helpers (no library dependency)
    static String  extractStr(const String& json, const char* key);
    static int     extractInt(const String& json, const char* key, int def = -1);

    // Singleton for lambda captures
    static WiFiControl* _instance;
};

#endif // CONTROL_WIFI

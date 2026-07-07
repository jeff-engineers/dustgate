// =============================================================================
// HttpApiServer.h — REST + WebSocket API server
//
// Runs alongside any control mode (CONTROL_SERIAL_DEBUG, CONTROL_SMART_OUTLET,
// etc.). Enabled by #define ENABLE_HTTP_API in config.h.
//
// REST endpoints:  http://<device-ip>/api/...
// WebSocket:       ws://<device-ip>/ws   (push on state change)
// Auth:            X-Api-Key: <key> header on all requests
//
// Thread safety:
//   Request handlers run on the AsyncTCP task (Core 0). They write only to
//   _pendingCmd (protected by _mutex). The main loop (Core 1) calls consume
//   methods to drain commands — no direct motor/feedback access from handlers.
//
// Status flow:
//   Main loop calls update(status) each iteration. If the serialized status
//   differs from the last push, all connected WebSocket clients are notified.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "../config.h"

#ifdef ENABLE_HTTP_API

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

// Forward declaration — outlets are optional (null if not in outlet mode)
#ifdef CONTROL_SMART_OUTLET
  class SmartOutletControl;
#endif

// ---------------------------------------------------------------------------
// Status snapshot passed from the main loop to update()
// ---------------------------------------------------------------------------
struct ApiStatus {
    const char* stateName;   // "IDLE", "HOMING", "MOVING", "AT_STOP", "ERROR", ...
    int  currentStop;        // last confirmed stop index (-1 = unknown)
    int  targetStop;
    long positionSteps;
    float positionMM;        // raw actuator position, independent of any saved stop —
                              // lets the UI show continuous movement while jogging,
                              // since currentStop/targetStop don't change during a jog
    bool homed;
    bool enabled;
    bool endstopHome;        // true = home switch currently triggered
    int  numActiveStops;     // runtime gate count (set from g_numActiveStops)
};

// ---------------------------------------------------------------------------
// HttpApiServer
// ---------------------------------------------------------------------------
class HttpApiServer {
public:
    HttpApiServer();

    // Call once from setup() after WiFi is connected.
    // Loads or generates the API key, starts the server.
    bool begin();

    // Call every loop(). Pushes a WebSocket frame if status has changed.
    // Pass current system state; also pass SmartOutletControl pointer for
    // outlet data (nullptr if not in outlet mode).
    void update(const ApiStatus& status
#ifdef CONTROL_SMART_OUTLET
                , SmartOutletControl* outlets = nullptr
#endif
    );

    // ------------------------------------------------------------------
    // Pending command consumers — call these from the main loop in order
    // of priority. Each returns true once, then clears the flag.
    // ------------------------------------------------------------------
    bool consumeEStopRequest();
    bool consumeHomeRequest();
    bool consumeEnableRequest();
    bool consumeDisableRequest();
    bool consumeMoveRequest(int& outStop);      // outStop: 0 = home, 1-N = gate
    bool consumeJogRequest(float& outMM);      // outMM: + = away from home
    bool consumeClearCalRequest();
    // Save current motor position as a numbered stop (used by setup agent).
    // Main loop must read current position and write it to g_stopPositionsMM.
    bool consumeSetStopRequest(int& outIndex); // outIndex: 1-N

    // Motor homing direction override (1 = normal, -1 = inverted).
    // Written to NVS by the handler; consumed by main loop to update g_homeDirection.
    bool consumeSetDirectionRequest(int& outDir);

    // Active gate count (runtime; bounded by compile-time NUM_STOPS).
    // Written to NVS by the handler; consumed by main loop to update g_numActiveStops.
    bool consumeSetNumGatesRequest(int& outN);

    // Visual orientation preference — home on right side vs left (default).
    // Loaded from NVS; returned in /api/info so Angular can render correctly.
    bool homeOnRight() const { return _homeOnRight; }

#ifdef CONTROL_SMART_OUTLET
    // Outlet configuration commands — consumed by main loop, forwarded to
    // SmartOutletControl.
    struct OutletConfigCmd {
        int   slot;
        int   generation;
        char  ip[16];
        char  name[32];
        int   stopIndex;
        float thresholdW;
    };
    bool consumeOutletConfigRequest(OutletConfigCmd& out);
    bool consumeOutletDeleteRequest(int& outSlot);
    bool consumeOutletSaveRequest();
#endif

    // Expose the API key for the setup agent / serial display
    const String& apiKey() const { return _apiKey; }

private:
    AsyncWebServer    _server;
    AsyncWebSocket    _ws;
    SemaphoreHandle_t _mutex;
    String            _apiKey;
    // Last serialised status — cached for GET /api/status
    String            _lastStatusJson;
    // Fingerprint of trigger fields — avoids WS pushes on positionSteps jitter
    uint32_t          _lastStatusHash;
    bool              _statusHashValid;
    // Throttled position-change push — positionSteps is excluded from the
    // fingerprint above (it changes every loop during any real move and would
    // flood the socket), but that also means a raw jog — which never touches
    // currentStop/targetStop/state — produced zero WS pushes at all, leaving
    // clients with a frozen position for the whole jog. This tracks the last
    // pushed position and forces an extra push when it has moved meaningfully,
    // no more often than every POSITION_PUSH_MIN_MS.
    long              _lastPushedPositionSteps;
    unsigned long     _lastPositionPushMs;

    // Pending commands (written by request handlers, read by main loop)
    bool  _estopPending;
    bool  _homePending;
    bool  _enablePending;
    bool  _disablePending;
    bool  _movePending;    int   _moveStop;
    bool  _jogPending;     float _jogMM;
    bool  _clearCalPending;
    bool  _setStopPending;         int  _setStopIndex;
    bool  _setDirectionPending;    int  _newDirection;        // 1 or -1
    bool  _setNumGatesPending;     int  _newNumGates;
    bool  _homeOnRight;            // persisted orientation preference
    int   _homeDirection;          // runtime direction; loaded from NVS, updated via API
    int   _cachedNumActiveStops;   // from ApiStatus.numActiveStops; returned in /api/info

#ifdef CONTROL_SMART_OUTLET
    bool            _outletConfigPending;
    OutletConfigCmd _outletConfigCmd;
    bool            _outletDeletePending;  int _outletDeleteSlot;
    bool            _outletSavePending;
#endif

    // Helpers
    bool   loadOrGenerateKey();
    bool   checkAuth(AsyncWebServerRequest* req);
    void   sendOk(AsyncWebServerRequest* req);
    void   sendError(AsyncWebServerRequest* req, int code, const char* msg);
    String buildStatusJson(const ApiStatus& status
#ifdef CONTROL_SMART_OUTLET
                           , SmartOutletControl* outlets
#endif
    );

    // Route registration
    void registerRoutes();
};

#endif // ENABLE_HTTP_API

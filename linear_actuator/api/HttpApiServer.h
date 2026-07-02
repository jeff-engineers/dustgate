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
    bool homed;
    bool enabled;
    bool endstopHome;        // true = home switch currently triggered
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
    bool consumeMoveRequest(int& outStop);   // outStop: 0 = home, 1-N = gate
    bool consumeJogRequest(float& outMM);   // outMM: + = away from home
    bool consumeClearCalRequest();

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

    // Pending commands (written by request handlers, read by main loop)
    bool  _estopPending;
    bool  _homePending;
    bool  _enablePending;
    bool  _disablePending;
    bool  _movePending;    int   _moveStop;
    bool  _jogPending;     float _jogMM;
    bool  _clearCalPending;

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

// =============================================================================
// HttpApiServer.cpp
// =============================================================================

#include "HttpApiServer.h"

#ifdef ENABLE_HTTP_API

#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <esp_random.h>      // hardware RNG for key generation
#include <cstdlib>           // labs() — position-drift check in update()
#include "../utils/MotionMath.h"
// AgentConfig.h provides getAnthropicKey/setAnthropicKey/reset without pulling
// in <WebServer.h>, which would conflict with ESPAsyncWebServer's HTTP enums.
#include "../utils/AgentConfig.h"
#include "../training/CalibrationStore.h"

#ifdef CONTROL_SMART_OUTLET
  #include "../control/SmartOutletControl.h"
  #include "../outlets/ShellyGen1Outlet.h"
  #include "../outlets/ShellyGen2Outlet.h"
  #include "../outlets/ShellyDeviceName.h"
  #include <HTTPClient.h>
#endif

static const char* NVS_NS  = "api_cfg";
static const char* NVS_KEY = "api_key";

// Minimum interval between position-drift-triggered pushes (see update()).
static const unsigned long POSITION_PUSH_MIN_MS = 150;

// =============================================================================
// Construction
// =============================================================================

// djb2-style hash over the fields that should trigger a WS push.
// positionSteps is intentionally excluded — it jitters every loop.
static uint32_t statusFingerprint(const ApiStatus& s
#ifdef CONTROL_SMART_OUTLET
                                  , SmartOutletControl* outlets = nullptr
#endif
) {
    uint32_t h = 5381;
    for (const char* p = s.stateName; *p; ++p) h = ((h << 5) + h) ^ (uint8_t)*p;
    h = ((h << 5) + h) ^ (uint32_t)(s.currentStop  & 0xFF);
    h = ((h << 5) + h) ^ (uint32_t)(s.targetStop   & 0xFF);
    h = ((h << 5) + h) ^ (uint32_t)(s.homed      ? 1 : 0);
    h = ((h << 5) + h) ^ (uint32_t)(s.enabled    ? 2 : 0);
    h = ((h << 5) + h) ^ (uint32_t)(s.endstopHome? 4 : 0);
    h = ((h << 5) + h) ^ (uint32_t)(s.numActiveStops & 0xFF);
    // manualOverride state is in SmartOutletControl, not ApiStatus — fingerprinted separately
#ifdef CONTROL_SMART_OUTLET
    // Outlet config (name/ip/stop mapping/threshold) was previously excluded
    // entirely, so reconfiguring outlets without also changing one of the
    // fields above (e.g. re-adding the same number of gates after "Start
    // Over") never invalidated the cached WS push — clients kept seeing the
    // previous session's outlet list indefinitely.
    if (outlets) {
        h = ((h << 5) + h) ^ (uint32_t)(outlets->outletCount() & 0xFF);
        for (int i = 0; i < outlets->outletCount(); i++) {
            SmartOutlet* o = outlets->outlet(i);
            if (!o) continue;
            for (const char* p = o->name(); *p; ++p) h = ((h << 5) + h) ^ (uint8_t)*p;
            for (const char* p = o->ip();   *p; ++p) h = ((h << 5) + h) ^ (uint8_t)*p;
            h = ((h << 5) + h) ^ (uint32_t)(o->getStopIndex() & 0xFF);
        }
        // Dust collector on/off + assigned state were missing here too — same
        // bug as the outlet list above: toggling the dashboard's DC switch
        // changed real state but never invalidated the cached WS push, so the
        // UI kept showing whatever dcOn value happened to be cached from the
        // last push that changed a *different* fingerprinted field.
        h = ((h << 5) + h) ^ (uint32_t)(outlets->dcOn()        ? 8  : 0);
        h = ((h << 5) + h) ^ (uint32_t)(outlets->dcConfigured() ? 16 : 0);
    }
#endif
    return h;
}

HttpApiServer::HttpApiServer()
    : _server(API_PORT),
      _ws("/ws"),
      _mutex(nullptr),
      _lastStatusHash(0),
      _statusHashValid(false),
      _lastPushedPositionSteps(0),
      _lastPositionPushMs(0),
      _estopPending(false),
      _homePending(false),
      _enablePending(false),
      _disablePending(false),
      _movePending(false),  _moveStop(0),
      _jogPending(false),   _jogMM(0.0f),
      _clearCalPending(false),
      _setStopPending(false),      _setStopIndex(0),
      _setDirectionPending(false), _newDirection(HOME_DIRECTION_DEFAULT),
      _setNumGatesPending(false),  _newNumGates(0),
      _calibratePending(false),    _calGateCount(0),
      _portRolePending(false),     _portRoleIndex(0), _portRoleValue(0),
      _orientationPending(false),  _orientationValue(false),
      _homeDirection(HOME_DIRECTION_DEFAULT),
      _cachedNumActiveStops(0),
      _idleTimeoutSec(IDLE_TIMEOUT_SEC_DEFAULT)
#ifdef CONTROL_SMART_OUTLET
    , _outletConfigPending(false),
      _outletDeletePending(false), _outletDeleteSlot(0),
      _outletSavePending(false),
      _dcConfigPending(false),
      _dcDeletePending(false),
      _dcSwitchPending(false), _dcSwitchOn(false),
      _discoverPending(false), _discoverReq(nullptr),
      _pingPending(false), _pingReq(nullptr)
#endif
{
    _calModel[0] = '\0';
#ifdef CONTROL_SMART_OUTLET
    _pingIp[0] = '\0';
#endif
}

// =============================================================================
// begin()
// =============================================================================

bool HttpApiServer::begin() {
    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) {
        DEBUG_PRINTLN(F("[API] Failed to create mutex."));
        return false;
    }

    if (!loadOrGenerateKey()) return false;

    // Load persisted orientation + direction preferences
    {
        Preferences prefs;
        prefs.begin(NVS_NS, true);
        // Mounting orientation now lives in CalibrationData (persisted by the main
        // loop), reported here via ApiStatus each update() — not loaded from NVS.
        _homeDirection = prefs.getInt("home_dir",  HOME_DIRECTION_DEFAULT);
        _cachedNumActiveStops = prefs.getInt("num_gates", 0); // 0 = not yet configured
        _idleTimeoutSec = prefs.getInt("idle_to", IDLE_TIMEOUT_SEC_DEFAULT);
        prefs.end();
    }

    DEBUG_PRINT(F("[API] Key: ")); Serial.println(_apiKey);
    DEBUG_PRINTLN(F("[API] Include this as 'X-Api-Key: <key>' on all requests."));

    // Mount LittleFS — serves the Angular front-end bundle from flash.
    // 'true' = format on first mount if partition is empty (safe, idempotent).
    // Partition label must be given explicitly: this board's default partition
    // table (partitions-4MB-tinyuf2.csv, for the UF2/TinyUF2 bootloader) names
    // the data partition "ffat" (it's meant for FATFS), not the "spiffs" label
    // LittleFS.begin() assumes by default — without this, mount always fails
    // even though the image itself is written and verified correctly.
    if (!LittleFS.begin(true, "/littlefs", 10, "ffat")) {
        DEBUG_PRINTLN(F("[API] LittleFS mount failed — front-end will not be served."));
    } else {
        DEBUG_PRINTLN(F("[API] LittleFS mounted."));
    }

    // WebSocket cleanup handler — free client resources on disconnect
    _ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType type,
                   void*, uint8_t*, size_t) {
        if (type == WS_EVT_DISCONNECT || type == WS_EVT_ERROR) {
            // AsyncWebServer handles cleanup; nothing extra needed here
        }
    });
    _server.addHandler(&_ws);

    registerRoutes();

    // CORS headers for Angular dev server (localhost:4200)
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin",  "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "X-Api-Key, Content-Type");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");

    _server.begin();
    DEBUG_PRINT(F("[API] Server running on port ")); Serial.println(API_PORT);
    return true;
}

// =============================================================================
// update() — call every loop(); pushes WS frame when status changes
// =============================================================================

void HttpApiServer::update(const ApiStatus& status
#ifdef CONTROL_SMART_OUTLET
                           , SmartOutletControl* outlets
#endif
) {
    _ws.cleanupClients();

    // Cache runtime gate count so /api/info always reflects current value
    if (status.numActiveStops > 0) _cachedNumActiveStops = status.numActiveStops;

#ifdef CONTROL_SMART_OUTLET
    uint32_t fp = statusFingerprint(status, outlets);
#else
    uint32_t fp = statusFingerprint(status);
#endif
    bool fieldsChanged = !_statusHashValid || (fp != _lastStatusHash);

    // A raw jog (consumeJogRequest) moves positionSteps but touches none of
    // the fingerprinted fields above, so on its own it would never trigger a
    // push — clients would see a frozen position for the whole jog. Force a
    // throttled push whenever the position has drifted by roughly a mm,
    // capped to POSITION_PUSH_MIN_MS so this can't flood the socket the way
    // pushing on every positionSteps tick would.
    long driftSteps = labs(status.positionSteps - _lastPushedPositionSteps);
    bool positionDrifted = driftSteps > (long)stepsPerMM()
                        && (millis() - _lastPositionPushMs) >= POSITION_PUSH_MIN_MS;

    bool changed = fieldsChanged || positionDrifted;

    if (!changed && _ws.count() == 0) return; // nothing to do

    if (changed) {
        // Serialise only when something meaningful changed
#ifdef CONTROL_SMART_OUTLET
        _lastStatusJson = buildStatusJson(status, outlets);
#else
        _lastStatusJson = buildStatusJson(status);
#endif
        _lastStatusHash          = fp;
        _statusHashValid         = true;
        _lastPushedPositionSteps = status.positionSteps;
        _lastPositionPushMs      = millis();

        if (_ws.count() > 0) {
            _ws.textAll(_lastStatusJson);
        }
    }
}

// =============================================================================
// Pending command consumers
// =============================================================================

#define CONSUME(flag) \
    xSemaphoreTake(_mutex, portMAX_DELAY); \
    bool v = flag; flag = false; \
    xSemaphoreGive(_mutex); \
    return v;

bool HttpApiServer::consumeEStopRequest()   { CONSUME(_estopPending)   }
bool HttpApiServer::consumeHomeRequest()    { CONSUME(_homePending)    }
bool HttpApiServer::consumeEnableRequest()  { CONSUME(_enablePending)  }
bool HttpApiServer::consumeDisableRequest() { CONSUME(_disablePending) }
bool HttpApiServer::consumeClearCalRequest(){ CONSUME(_clearCalPending)}

bool HttpApiServer::consumeMoveRequest(int& outStop) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = _movePending;
    if (v) { outStop = _moveStop; _movePending = false; }
    xSemaphoreGive(_mutex);
    return v;
}

bool HttpApiServer::consumeSetStopRequest(int& outIndex) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = _setStopPending;
    if (v) { outIndex = _setStopIndex; _setStopPending = false; }
    xSemaphoreGive(_mutex);
    return v;
}

bool HttpApiServer::consumeSetDirectionRequest(int& outDir) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = _setDirectionPending;
    if (v) { outDir = _newDirection; _setDirectionPending = false; }
    xSemaphoreGive(_mutex);
    return v;
}

bool HttpApiServer::consumeSetNumGatesRequest(int& outN) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = _setNumGatesPending;
    if (v) { outN = _newNumGates; _setNumGatesPending = false; }
    xSemaphoreGive(_mutex);
    return v;
}

bool HttpApiServer::consumeCalibrateRequest(char* outModel, size_t modelLen, int& outGateCount) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = _calibratePending;
    if (v) { strlcpy(outModel, _calModel, modelLen); outGateCount = _calGateCount; _calibratePending = false; }
    xSemaphoreGive(_mutex);
    return v;
}

bool HttpApiServer::consumePortRoleRequest(int& outIndex, int& outRole) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = _portRolePending;
    if (v) { outIndex = _portRoleIndex; outRole = _portRoleValue; _portRolePending = false; }
    xSemaphoreGive(_mutex);
    return v;
}

bool HttpApiServer::consumeOrientationRequest(bool& outHomedLeft) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = _orientationPending;
    if (v) { outHomedLeft = _orientationValue; _orientationPending = false; }
    xSemaphoreGive(_mutex);
    return v;
}

bool HttpApiServer::consumeJogRequest(float& outMM) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = _jogPending;
    if (v) { outMM = _jogMM; _jogPending = false; }
    xSemaphoreGive(_mutex);
    return v;
}

#ifdef CONTROL_SMART_OUTLET
bool HttpApiServer::consumeOutletConfigRequest(OutletConfigCmd& out) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = _outletConfigPending;
    if (v) { out = _outletConfigCmd; _outletConfigPending = false; }
    xSemaphoreGive(_mutex);
    return v;
}
bool HttpApiServer::consumeOutletDeleteRequest(int& outSlot) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = _outletDeletePending;
    if (v) { outSlot = _outletDeleteSlot; _outletDeletePending = false; }
    xSemaphoreGive(_mutex);
    return v;
}
bool HttpApiServer::consumeOutletSaveRequest() { CONSUME(_outletSavePending) }

bool HttpApiServer::consumeDustCollectorConfigRequest(DustCollectorCmd& out) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = _dcConfigPending;
    if (v) { out = _dcConfigCmd; _dcConfigPending = false; }
    xSemaphoreGive(_mutex);
    return v;
}
bool HttpApiServer::consumeDustCollectorDeleteRequest() { CONSUME(_dcDeletePending) }
bool HttpApiServer::consumeDustCollectorSwitchRequest(bool& outOn) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = _dcSwitchPending;
    if (v) { outOn = _dcSwitchOn; _dcSwitchPending = false; }
    xSemaphoreGive(_mutex);
    return v;
}

bool HttpApiServer::consumeDiscoverRequest() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = _discoverPending;
    xSemaphoreGive(_mutex);
    return v;
}

void HttpApiServer::respondDiscover(const String& json) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    AsyncWebServerRequest* req = _discoverReq;
    _discoverPending = false;
    _discoverReq     = nullptr;
    xSemaphoreGive(_mutex);
    if (req) req->send(200, "application/json", json);
}

bool HttpApiServer::consumePingRequest(char* outIp, size_t ipLen) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = _pingPending;
    if (v) strlcpy(outIp, _pingIp, ipLen);
    xSemaphoreGive(_mutex);
    return v;
}

void HttpApiServer::respondPing(const String& json) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    AsyncWebServerRequest* req = _pingReq;
    _pingPending = false;
    _pingReq     = nullptr;
    xSemaphoreGive(_mutex);
    if (req) req->send(200, "application/json", json);
}
#endif

#undef CONSUME

// =============================================================================
// Route registration
// =============================================================================

static const char* portRoleName(uint8_t role);  // defined below, used in routes + buildStatusJson

void HttpApiServer::registerRoutes() {

    // ------------------------------------------------------------------
    // GET /api/info — unauthenticated bootstrap endpoint.
    // Returns the API key so the Angular app can make authenticated
    // requests without the user having to enter it manually.
    // Security: only reachable on the local network (same as the device).
    // ------------------------------------------------------------------
    _server.on("/api/info", HTTP_GET, [this](AsyncWebServerRequest* req) {
        StaticJsonDocument<256> doc;
        doc["apiKey"]        = _apiKey;
        doc["numStops"]      = _cachedNumActiveStops;   // runtime; not compile-time NUM_STOPS
        doc["version"]       = "1.0.0";
        doc["motorInverted"]  = (_homeDirection < 0);    // true when direction was flipped
        doc["idleTimeoutSec"] = _idleTimeoutSec;
        doc["manifoldModel"]  = g_manifoldModel;
        doc["stepsPerMm"]     = serialized(String(g_measuredStepsPerMM > 0 ? g_measuredStepsPerMM : stepsPerMM(), 3));
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // ------------------------------------------------------------------
    // POST /api/setstop   body: {"index": N}
    // Saves the current motor position as stop N in CalibrationStore.
    // The main loop reads current position and writes g_stopPositionsMM[N].
    // ------------------------------------------------------------------
    _server.on("/api/setstop", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!checkAuth(req)) return;
            StaticJsonDocument<64> doc;
            if (deserializeJson(doc, data, len)) { sendError(req, 400, "invalid JSON"); return; }
            int idx = doc["index"] | -1;
            if (idx < 1 || idx > NUM_STOPS) { sendError(req, 400, "index out of range (1–NUM_STOPS)"); return; }
            DEBUG_PRINT(F("[UI] Set stop position: ")); DEBUG_PRINTLN(idx);
            xSemaphoreTake(_mutex, portMAX_DELAY);
            _setStopPending = true; _setStopIndex = idx;
            xSemaphoreGive(_mutex);
            sendOk(req);
        }
    );

    // ------------------------------------------------------------------
    // POST /api/config/orientation   body: {"homedLeft": true|false}
    // Reports which side the carriage homed to. The main loop keeps the home datum
    // on the user's LEFT, switching to the other endstop (and re-homing on the next
    // home) if it came up on the right.
    // ------------------------------------------------------------------
    _server.on("/api/config/orientation", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!checkAuth(req)) return;
            StaticJsonDocument<64> doc;
            if (deserializeJson(doc, data, len)) { sendError(req, 400, "invalid JSON"); return; }
            if (!doc.containsKey("homedLeft")) { sendError(req, 400, "missing homedLeft"); return; }
            bool homedLeft = doc["homedLeft"].as<bool>();
            // Pending-command pattern: the main loop makes the home datum the user's
            // left endstop, re-homing there if the carriage came up on the right.
            xSemaphoreTake(_mutex, portMAX_DELAY);
            _orientationPending = true;
            _orientationValue   = homedLeft;
            xSemaphoreGive(_mutex);
            DEBUG_PRINT(F("[API] Home side: homed on the "));
            DEBUG_PRINTLN(homedLeft ? F("left") : F("right"));
            sendOk(req);
        }
    );

    // ------------------------------------------------------------------
    // POST /api/config/motor   body: {"invertDirection": true|false}
    // Flips the homing direction.  Persists to NVS; takes effect immediately
    // via the pending-command pattern (main loop updates g_homeDirection).
    // ------------------------------------------------------------------
    _server.on("/api/config/motor", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!checkAuth(req)) return;
            StaticJsonDocument<64> doc;
            if (deserializeJson(doc, data, len)) { sendError(req, 400, "invalid JSON"); return; }
            if (!doc.containsKey("invertDirection")) { sendError(req, 400, "missing invertDirection"); return; }
            bool invert = doc["invertDirection"].as<bool>();
            int  dir    = invert ? -HOME_DIRECTION_DEFAULT : HOME_DIRECTION_DEFAULT;
            xSemaphoreTake(_mutex, portMAX_DELAY);
            _homeDirection       = dir;
            _setDirectionPending = true;
            _newDirection        = dir;
            xSemaphoreGive(_mutex);
            Preferences prefs;
            prefs.begin(NVS_NS, false);
            prefs.putInt("home_dir", dir);
            prefs.end();
            DEBUG_PRINT(F("[API] Motor direction: "));
            DEBUG_PRINTLN(invert ? F("inverted") : F("normal"));
            sendOk(req);
        }
    );

    // ------------------------------------------------------------------
    // POST /api/config/gates   body: {"numGates": N}
    // Sets the runtime active gate count (1–NUM_STOPS).
    // Persists to NVS and takes effect immediately.
    // ------------------------------------------------------------------
    _server.on("/api/config/gates", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!checkAuth(req)) return;
            StaticJsonDocument<64> doc;
            if (deserializeJson(doc, data, len)) { sendError(req, 400, "invalid JSON"); return; }
            int n = doc["numGates"] | -1;
            if (n < 1 || n > NUM_STOPS) {
                sendError(req, 400, ("numGates out of range (1-" + String(NUM_STOPS) + ")").c_str());
                return;
            }
            xSemaphoreTake(_mutex, portMAX_DELAY);
            _cachedNumActiveStops = n;
            _setNumGatesPending   = true;
            _newNumGates          = n;
            xSemaphoreGive(_mutex);
            Preferences prefs;
            prefs.begin(NVS_NS, false);
            prefs.putInt("num_gates", n);
            prefs.end();
            DEBUG_PRINT(F("[API] Active gates: ")); Serial.println(n);
            sendOk(req);
        }
    );

    // ------------------------------------------------------------------
    // POST /api/calibrate   body: {"model": "rockler-2.5", "gateCount": N}
    // Runs the dual-endstop reference sweep on the main loop (see the consume
    // handler in linear_actuator.ino) — measures the span, derives steps/mm, and
    // auto-places gates by proportion for a known manifold.
    // ------------------------------------------------------------------
    _server.on("/api/calibrate", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!checkAuth(req)) return;
            StaticJsonDocument<96> doc;
            if (deserializeJson(doc, data, len)) { sendError(req, 400, "invalid JSON"); return; }
            int n = doc["gateCount"] | -1;
            if (n < 1 || n > NUM_STOPS) { sendError(req, 400, "gateCount out of range"); return; }
            const char* model = doc["model"] | "custom";
            xSemaphoreTake(_mutex, portMAX_DELAY);
            strlcpy(_calModel, model, sizeof(_calModel));
            _calGateCount     = n;
            _calibratePending = true;
            xSemaphoreGive(_mutex);
            DEBUG_PRINT(F("[API] Calibrate: ")); DEBUG_PRINT(model);
            DEBUG_PRINT(F(" x")); DEBUG_PRINTLN(n);
            sendOk(req);
        }
    );

    // ------------------------------------------------------------------
    // POST /api/config/port-role   body: {"index": N, "role": "blocked"}
    // Sets a gate's role (tool|unassigned|blocked|feed). Persisted to
    // CalibrationData by the main loop; blocked ports are never move targets.
    // ------------------------------------------------------------------
    _server.on("/api/config/port-role", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!checkAuth(req)) return;
            StaticJsonDocument<96> doc;
            if (deserializeJson(doc, data, len)) { sendError(req, 400, "invalid JSON"); return; }
            int idx = doc["index"] | -1;
            if (idx < 1 || idx > NUM_STOPS) { sendError(req, 400, "index out of range"); return; }
            const char* r = doc["role"] | "";
            int role;
            if      (strcmp(r, "tool") == 0)       role = ROLE_TOOL;
            else if (strcmp(r, "unassigned") == 0) role = ROLE_UNASSIGNED;
            else if (strcmp(r, "blocked") == 0)    role = ROLE_BLOCKED;
            else if (strcmp(r, "feed") == 0)       role = ROLE_FEED;
            else { sendError(req, 400, "invalid role"); return; }
            xSemaphoreTake(_mutex, portMAX_DELAY);
            _portRoleIndex   = idx;
            _portRoleValue   = role;
            _portRolePending = true;
            xSemaphoreGive(_mutex);
            DEBUG_PRINT(F("[API] Port role: gate ")); DEBUG_PRINT(idx);
            DEBUG_PRINT(F(" -> ")); DEBUG_PRINTLN(r);
            sendOk(req);
        }
    );

    // ------------------------------------------------------------------
    // POST /api/config/idle-timeout   body: {"seconds": N}
    // Seconds of no move/home activity before the driver is powered off
    // (0 = never sleep). Persists to NVS; the main loop polls idleTimeoutSec()
    // directly each iteration, so this takes effect immediately with no
    // pending-command plumbing needed.
    // ------------------------------------------------------------------
    _server.on("/api/config/idle-timeout", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!checkAuth(req)) return;
            StaticJsonDocument<64> doc;
            if (deserializeJson(doc, data, len)) { sendError(req, 400, "invalid JSON"); return; }
            if (!doc.containsKey("seconds")) { sendError(req, 400, "missing seconds"); return; }
            int sec = doc["seconds"] | -1;
            if (sec < 0 || sec > 86400) { sendError(req, 400, "seconds out of range (0-86400)"); return; }

            _idleTimeoutSec = sec;
            Preferences prefs;
            prefs.begin(NVS_NS, false);
            prefs.putInt("idle_to", sec);
            prefs.end();
            DEBUG_PRINT(F("[API] Idle timeout: "));
            if (sec == 0) DEBUG_PRINTLN(F("disabled"));
            else { Serial.print(sec); DEBUG_PRINTLN(F("s")); }
            sendOk(req);
        }
    );

    // ------------------------------------------------------------------
    // POST /api/wifi/reset
    // Erases stored WiFi credentials and reboots into the captive setup
    // portal — the HTTP equivalent of the serial 'wifireset' command, for
    // when a user needs to move the device to a different network without
    // physical serial access. Does not return; WiFiProvisioner::reset()
    // restarts the device after a short delay (long enough for this
    // response to flush to the client first).
    // ------------------------------------------------------------------
    _server.on("/api/wifi/reset", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        DEBUG_PRINTLN(F("[UI] WiFi reset requested — erasing credentials and rebooting."));
        sendOk(req);
        delay(300);
        WiFiProvisioner::reset(); // does not return
    });

    // ------------------------------------------------------------------
    // Static file serving — Angular front-end bundle from LittleFS.
    // ESPAsyncWebServer automatically serves <file>.gz when a client
    // sends Accept-Encoding: gzip (all modern browsers do).
    // The Angular app uses hash routing (/#/route) so the server only
    // ever needs to serve index.html for the root — no catch-all needed.
    // ------------------------------------------------------------------
    _server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // OPTIONS preflight for CORS
    _server.onNotFound([](AsyncWebServerRequest* req) {
        if (req->method() == HTTP_OPTIONS) req->send(204);
        else req->send(404, "application/json", "{\"error\":\"not found\"}");
    });

    // ------------------------------------------------------------------
    // GET /api/status — full snapshot (no WS needed for one-shot queries)
    // ------------------------------------------------------------------
    _server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        req->send(200, "application/json",
                  _lastStatusJson.length() ? _lastStatusJson : "{\"state\":\"STARTING\"}");
    });

    // ------------------------------------------------------------------
    // GET /api/stops
    // ------------------------------------------------------------------
    _server.on("/api/stops", HTTP_GET, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        StaticJsonDocument<1536> doc; // up to 17 stops × ~55B each (incl role)
        JsonArray arr = doc.createNestedArray("stops");
        for (int i = 0; i <= _cachedNumActiveStops; i++) {
            JsonObject o = arr.createNestedObject();
            o["index"]  = i;
            // index 0 (home) is always real once homed; indices beyond what's
            // actually been trained hold a placeholder value in
            // g_stopPositionsMM (0.0, or an extrapolated default while
            // loading calibration) that was never explicitly saved — report
            // those as null rather than a false real position.
            if (i > 0 && i > g_numTrainedStops) {
                o["mm"] = nullptr;
            } else {
                o["mm"] = serialized(String(g_stopPositionsMM[i], 2));
            }
            o["steps"]  = (long)(g_stopPositionsMM[i] * stepsPerMM()) * (-HOME_DIRECTION);
            o["role"]   = portRoleName(g_stopRoles[i]);
        }
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // ------------------------------------------------------------------
    // Motion commands — body parsed in onBody handler
    // ------------------------------------------------------------------

    // POST /api/home
    _server.on("/api/home", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        DEBUG_PRINTLN(F("[UI] Home requested."));
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _homePending = true;
        xSemaphoreGive(_mutex);
        sendOk(req);
    });

    // POST /api/enable
    _server.on("/api/enable", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        DEBUG_PRINTLN(F("[UI] Enable requested."));
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _enablePending = true;
        xSemaphoreGive(_mutex);
        sendOk(req);
    });

    // POST /api/disable
    _server.on("/api/disable", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        DEBUG_PRINTLN(F("[UI] Disable requested."));
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _disablePending = true;
        xSemaphoreGive(_mutex);
        sendOk(req);
    });

    // POST /api/estop
    _server.on("/api/estop", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        DEBUG_PRINTLN(F("[UI] E-STOP requested."));
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _estopPending = true;
        xSemaphoreGive(_mutex);
        sendOk(req);
    });

    // POST /api/clearcal
    // Erases calibration EEPROM and resets the runtime gate count so the
    // device returns to an unconfigured state (setup wizard can restart).
    _server.on("/api/clearcal", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        DEBUG_PRINTLN(F("[UI] Clear calibration (Start Over) requested."));
        // Clear the NVS gate count so it doesn't persist across reboots
        {
            Preferences prefs;
            prefs.begin(NVS_NS, false);
            prefs.remove("num_gates");
            prefs.end();
        }
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _clearCalPending      = true;
        _cachedNumActiveStops = 0;
        xSemaphoreGive(_mutex);
        sendOk(req);
    });

    // POST /api/reboot
    _server.on("/api/reboot", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        DEBUG_PRINTLN(F("[UI] Reboot requested."));
        sendOk(req);
        delay(200);
        ESP.restart();
    });

    // ------------------------------------------------------------------
    // POST /api/move   body: {"stop": 2}
    // POST /api/jog    body: {"mm": -5.0}
    // ------------------------------------------------------------------
    _server.on("/api/move", HTTP_POST,
        [](AsyncWebServerRequest* req) {},  // handled in body callback
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!checkAuth(req)) return;
            StaticJsonDocument<64> doc;
            if (deserializeJson(doc, data, len)) { sendError(req, 400, "invalid JSON"); return; }
            int stop = doc["stop"] | -1;
            if (stop < 0 || stop > NUM_STOPS) { sendError(req, 400, "stop out of range"); return; }
            DEBUG_PRINT(F("[UI] Move to stop: ")); DEBUG_PRINTLN(stop);
            xSemaphoreTake(_mutex, portMAX_DELAY);
            _movePending = true; _moveStop = stop;
            xSemaphoreGive(_mutex);
            sendOk(req);
        }
    );

    _server.on("/api/jog", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!checkAuth(req)) return;
            StaticJsonDocument<64> doc;
            if (deserializeJson(doc, data, len)) { sendError(req, 400, "invalid JSON"); return; }
            if (!doc.containsKey("mm")) { sendError(req, 400, "missing 'mm'"); return; }
            float mm = doc["mm"].as<float>();
            DEBUG_PRINT(F("[UI] Jog: ")); DEBUG_PRINTLN(mm);
            xSemaphoreTake(_mutex, portMAX_DELAY);
            _jogPending = true; _jogMM = mm;
            xSemaphoreGive(_mutex);
            sendOk(req);
        }
    );

    // ------------------------------------------------------------------
    // Outlet endpoints (compiled only in outlet mode)
    // ------------------------------------------------------------------
#ifdef CONTROL_SMART_OUTLET

    // GET /api/outlets/discover
    // Queries mDNS for _http._tcp services and filters to hostnames that look
    // like Shelly devices, probing each match the same way /api/outlets/ping
    // does so the response is ready to populate the wizard's outlet list.
    //
    // The actual mDNS query + probing happens on the MAIN LOOP TASK, not here
    // and not in a spawned FreeRTOS task — see consumeDiscoverRequest() in
    // linear_actuator.ino. ESP32's mDNS responder is not safe to call from a
    // task other than the one that called MDNS.begin() (the main loop task,
    // via WiFiProvisioner) while it's also actively advertising the device's
    // own hostname service; doing so previously corrupted the heap and caused
    // a crash-reboot shortly after almost every discover call, HTTP or serial.
    // This handler just stashes the request; the main loop calls
    // respondDiscover() once the scan is done.
    //
    // MUST be registered before GET /api/outlets below: ESPAsyncWebServer
    // matches a registered URI as a prefix (uri == url() || url().startsWith(uri
    // + "/")) and dispatches to the first match in registration order, so with
    // the plain list route registered first every /api/outlets/discover request
    // was being swallowed by it and returning the wrong (object, not array)
    // JSON shape — which is why the wizard's scan silently found nothing.
    _server.on("/api/outlets/discover", HTTP_GET, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _discoverPending = true;
        _discoverReq     = req;
        xSemaphoreGive(_mutex);
        // response sent later by the main loop via respondDiscover()
    });

    // GET /api/outlets — list with live readings
    _server.on("/api/outlets", HTTP_GET, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        // Data is embedded in the status push; return last known for GET
        req->send(200, "application/json", _lastStatusJson.length() ? _lastStatusJson : "{}");
    });

    // POST /api/outlets/ping   body: {"ip":"192.168.1.x"}
    // Auto-detects the Shelly API generation rather than requiring the caller
    // to know it. Like discover, the actual probe runs on the MAIN LOOP TASK
    // (see consumePingRequest() in linear_actuator.ino), not a spawned task —
    // the probe can block for a couple seconds on an unreachable host, and a
    // detached task holding this raw request across that window risked a
    // use-after-free if the browser disconnected/retried mid-probe. This
    // handler just stashes the IP; the main loop calls respondPing() when done.
    _server.on("/api/outlets/ping", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!checkAuth(req)) return;
            StaticJsonDocument<128> doc;
            if (deserializeJson(doc, data, len)) { sendError(req, 400, "invalid JSON"); return; }
            const char* ip = doc["ip"] | "";
            if (strlen(ip) == 0) { sendError(req, 400, "missing 'ip'"); return; }

            xSemaphoreTake(_mutex, portMAX_DELAY);
            _pingPending = true;
            _pingReq     = req;
            strlcpy(_pingIp, ip, sizeof(_pingIp));
            xSemaphoreGive(_mutex);
            // response sent later by the main loop via respondPing()
        }
    );

    // POST /api/outlets/save
    _server.on("/api/outlets/save", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        DEBUG_PRINTLN(F("[UI] Save all outlet config to NVS."));
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _outletSavePending = true;
        xSemaphoreGive(_mutex);
        sendOk(req);
    });

    // PUT /api/outlets/:slot   body: {"gen":1,"ip":"...","name":"...","stop":2,"threshold":5.0}
    _server.on("/api/outlets", HTTP_PUT,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!checkAuth(req)) return;
            // Extract slot from URL: /api/outlets/0
            String url = req->url();
            int slot = url.substring(url.lastIndexOf('/') + 1).toInt();
            if (slot < 0 || slot >= SMART_OUTLET_COUNT) { sendError(req, 400, "slot out of range"); return; }

            StaticJsonDocument<320> doc;
            if (deserializeJson(doc, data, len)) { sendError(req, 400, "invalid JSON"); return; }

            OutletConfigCmd cmd;
            cmd.slot       = slot;
            cmd.generation = doc["gen"]       | 1;
            cmd.stopIndex  = doc["stop"]      | 0;
            cmd.thresholdW = doc["threshold"] | OUTLET_DEFAULT_THRESHOLD_W;
            strlcpy(cmd.ip,   doc["ip"]   | "",  sizeof(cmd.ip));
            strlcpy(cmd.host, doc["host"] | "",  sizeof(cmd.host));
            strlcpy(cmd.name, doc["name"] | "",  sizeof(cmd.name));

            // ip is optional: an empty ip is a name-only gate (no smart plug —
            // labelled, but not power-polled). name is always required.
            if (strlen(cmd.name) == 0) { sendError(req, 400, "missing 'name'"); return; }
            if (cmd.stopIndex <= 0)    { sendError(req, 400, "missing 'stop'"); return; }

            DEBUG_PRINT(F("[UI] Configure outlet slot ")); DEBUG_PRINT(slot);
            DEBUG_PRINT(F(": ")); DEBUG_PRINT(cmd.name);
            DEBUG_PRINT(F(" -> stop ")); DEBUG_PRINTLN(cmd.stopIndex);
            xSemaphoreTake(_mutex, portMAX_DELAY);
            _outletConfigPending = true;
            _outletConfigCmd     = cmd;
            xSemaphoreGive(_mutex);
            sendOk(req);
        }
    );

    // DELETE /api/outlets/:slot
    _server.on("/api/outlets", HTTP_DELETE, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        String url = req->url();
        int slot = url.substring(url.lastIndexOf('/') + 1).toInt();
        if (slot < 0 || slot >= SMART_OUTLET_COUNT) { sendError(req, 400, "slot out of range"); return; }
        DEBUG_PRINT(F("[UI] Delete outlet slot: ")); DEBUG_PRINTLN(slot);
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _outletDeletePending = true; _outletDeleteSlot = slot;
        xSemaphoreGive(_mutex);
        sendOk(req);
    });

    // PUT /api/dustcollector   body: {"gen":2,"ip":"192.168.1.x"}
    // Assigns the switchable Shelly plug that powers the dust collector.
    _server.on("/api/dustcollector", HTTP_PUT,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!checkAuth(req)) return;

            StaticJsonDocument<192> doc;
            if (deserializeJson(doc, data, len)) { sendError(req, 400, "invalid JSON"); return; }

            DustCollectorCmd cmd;
            cmd.generation = doc["gen"] | 2;
            strlcpy(cmd.ip,   doc["ip"]   | "", sizeof(cmd.ip));
            strlcpy(cmd.host, doc["host"] | "", sizeof(cmd.host));
            if (strlen(cmd.ip) == 0) { sendError(req, 400, "missing 'ip'"); return; }

            DEBUG_PRINT(F("[UI] Configure dust collector: ")); DEBUG_PRINTLN(cmd.ip);
            xSemaphoreTake(_mutex, portMAX_DELAY);
            _dcConfigPending = true;
            _dcConfigCmd     = cmd;
            xSemaphoreGive(_mutex);
            sendOk(req);
        }
    );

    // DELETE /api/dustcollector — unassign the dust collector plug
    _server.on("/api/dustcollector", HTTP_DELETE, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        DEBUG_PRINTLN(F("[UI] Remove dust collector."));
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _dcDeletePending = true;
        xSemaphoreGive(_mutex);
        sendOk(req);
    });

    // POST /api/dustcollector/switch   body: {"on": true|false}
    // Manual dashboard override; holds until the next automatic tool event.
    _server.on("/api/dustcollector/switch", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!checkAuth(req)) return;
            StaticJsonDocument<64> doc;
            if (deserializeJson(doc, data, len)) { sendError(req, 400, "invalid JSON"); return; }
            bool on = doc["on"] | false;
            DEBUG_PRINT(F("[UI] Dust collector manual switch: ")); DEBUG_PRINTLN(on ? F("ON") : F("OFF"));
            xSemaphoreTake(_mutex, portMAX_DELAY);
            _dcSwitchPending = true;
            _dcSwitchOn      = on;
            xSemaphoreGive(_mutex);
            sendOk(req);
        }
    );

#endif // CONTROL_SMART_OUTLET

    // ------------------------------------------------------------------
    // POST /api/agent/chat — stateless Claude API proxy
    //
    // Body: full Anthropic /v1/messages request JSON (messages[], model, tools…)
    //       Angular holds conversation history and sends it every turn.
    //
    // The ESP32 adds auth headers and forwards to Anthropic; the response
    // is returned verbatim.  Angular runs the tool-use loop (see REQUIREMENTS.md §7).
    //
    // SECURITY NOTE: WiFiClientSecure is configured with setInsecure() below.
    // TODO: before any cloud/public deployment, validate the Anthropic root CA
    //       cert instead of skipping verification.
    // ------------------------------------------------------------------
    _server.on("/api/agent/chat", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!checkAuth(req)) return;
            if (len == 0) { sendError(req, 400, "empty body"); return; }

            // Not logging the body — it's the full chat history/tool payloads,
            // too noisy and potentially sensitive for serial.
            DEBUG_PRINTLN(F("[UI] AI setup assistant message sent."));

            String anthropicKey = WiFiProvisioner::getAnthropicKey();
            if (anthropicKey.length() == 0) {
                sendError(req, 503, "Anthropic API key not configured — set it via the setup portal or PUT /api/agent/key");
                return;
            }

            // Copy body; raw pointer is only valid during this callback
            String body((const char*)data, len);

            struct ChatArgs {
                AsyncWebServerRequest* req;
                String body;
                String key;
            };
            auto* args = new ChatArgs { req, body, anthropicKey };

            xTaskCreate([](void* arg) {
                auto* a = static_cast<ChatArgs*>(arg);

                WiFiClientSecure client;
                client.setInsecure(); // TODO: validate cert before cloud deployment

                const char* HOST = "api.anthropic.com";
                if (!client.connect(HOST, 443)) {
                    a->req->send(502, "application/json", "{\"error\":\"upstream connect failed\"}");
                    delete a;
                    vTaskDelete(nullptr);
                    return;
                }

                // Build HTTP/1.1 request
                String httpReq =
                    String("POST /v1/messages HTTP/1.1\r\n") +
                    "Host: " + HOST + "\r\n" +
                    "Content-Type: application/json\r\n" +
                    "x-api-key: " + a->key + "\r\n" +
                    "anthropic-version: 2023-06-01\r\n" +
                    "Content-Length: " + String(a->body.length()) + "\r\n" +
                    "Connection: close\r\n\r\n" +
                    a->body;
                client.print(httpReq);

                // Wait for response
                unsigned long t = millis();
                while (!client.available() && client.connected() && millis() - t < 30000UL)
                    delay(10);

                // Skip HTTP response headers
                bool headersDone = false;
                String respBody  = "";
                int    httpStatus = 200;
                bool   firstLine  = true;
                while (client.available() || client.connected()) {
                    String line = client.readStringUntil('\n');
                    if (!headersDone) {
                        if (firstLine) {
                            // e.g. "HTTP/1.1 200 OK"
                            int sp1 = line.indexOf(' ');
                            int sp2 = line.indexOf(' ', sp1 + 1);
                            if (sp1 > 0 && sp2 > sp1)
                                httpStatus = line.substring(sp1 + 1, sp2).toInt();
                            firstLine = false;
                        }
                        if (line == "\r" || line == "") headersDone = true;
                    } else {
                        respBody += line + "\n";
                    }
                    if (!client.connected() && !client.available()) break;
                }
                client.stop();

                // Forward Anthropic's status code so Angular sees real errors
                a->req->send(httpStatus, "application/json", respBody);
                delete a;
                vTaskDelete(nullptr);
            }, "chat_task", 8192, args, 1, nullptr);
            // Response sent from task — handler returns without calling send()
        }
    );

    // ------------------------------------------------------------------
    // PUT /api/agent/key   body: {"key":"sk-ant-..."}
    // Update the Anthropic key in NVS without re-running the setup portal.
    // ------------------------------------------------------------------
    _server.on("/api/agent/key", HTTP_PUT,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!checkAuth(req)) return;
            StaticJsonDocument<256> doc;
            if (deserializeJson(doc, data, len)) { sendError(req, 400, "invalid JSON"); return; }
            const char* key = doc["key"] | "";
            if (strlen(key) < 8) { sendError(req, 400, "key too short"); return; }
            WiFiProvisioner::setAnthropicKey(String(key));
            sendOk(req);
        }
    );

}

// =============================================================================
// Status JSON builder
// =============================================================================

// PortRole enum → wire string (mirrors shared/device-model PORT_ROLES).
static const char* portRoleName(uint8_t role) {
    switch (role) {
        case ROLE_TOOL:       return "tool";
        case ROLE_BLOCKED:    return "blocked";
        case ROLE_FEED:       return "feed";
        case ROLE_HOME:       return "home";
        case ROLE_UNASSIGNED:
        default:              return "unassigned";
    }
}

String HttpApiServer::buildStatusJson(const ApiStatus& s
#ifdef CONTROL_SMART_OUTLET
                                      , SmartOutletControl* outlets
#endif
) {
    // Size: base fields ~250B + up to 17 stops×~45B (incl role) + up to 16 outlets×80B ≈ 2800B
    StaticJsonDocument<3072> doc;

    doc["state"]         = s.stateName;
    doc["currentStop"]   = s.currentStop;
    doc["targetStop"]    = s.targetStop;
    doc["positionSteps"] = s.positionSteps;
    doc["positionMM"]    = s.positionMM;
    doc["homed"]         = s.homed;
    doc["enabled"]       = s.enabled;
    doc["endstopHome"]   = s.endstopHome;
    doc["farEndstop"]    = s.endstopMax;
    doc["manifoldModel"] = s.manifoldModel;
    doc["measuredSpanSteps"] = s.measuredSpanSteps > 0 ? s.measuredSpanSteps : (long)0;
    doc["stepsPerMm"]    = serialized(String(s.measuredStepsPerMM > 0 ? s.measuredStepsPerMM : stepsPerMM(), 3));

    JsonArray stops = doc.createNestedArray("stops");
    for (int i = 0; i <= _cachedNumActiveStops; i++) {
        JsonObject o = stops.createNestedObject();
        o["index"] = i;
        // See the identical check in the /api/stops handler above — indices
        // beyond what's actually been trained must read null, not a false
        // real position, or the setup wizard's conflict check misreads a
        // freshly-reset device's placeholder zeros as saved gates.
        if (i > 0 && i > g_numTrainedStops) {
            o["mm"] = nullptr;
        } else {
            o["mm"] = serialized(String(g_stopPositionsMM[i], 2));
        }
        o["role"] = portRoleName(g_stopRoles[i]);
    }

#ifdef CONTROL_SMART_OUTLET
    if (outlets) {
        doc["manualOverride"] = outlets->isManualOverride();
        doc["dcConfigured"]   = outlets->dcConfigured();
        doc["dcOn"]           = outlets->dcOn();
    }
    JsonArray outArr = doc.createNestedArray("outlets");
    if (outlets) {
        for (int i = 0; i < outlets->outletCount(); i++) {
            SmartOutlet* o = outlets->outlet(i);
            if (!o) continue;
            JsonObject jo = outArr.createNestedObject();
            jo["slot"]      = i;
            jo["name"]      = o->name();
            jo["stop"]      = o->getStopIndex();
            jo["powerW"]    = serialized(String(o->getPowerW(), 1));
            jo["active"]    = o->isActive();
            jo["reachable"] = o->isReachable();
            jo["hasSwitch"] = (strlen(o->ip()) > 0);  // false = name-only gate
        }
    }
#endif

    String out;
    serializeJson(doc, out);
    return out;
}

// =============================================================================
// Helpers
// =============================================================================

bool HttpApiServer::loadOrGenerateKey() {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    _apiKey = prefs.getString(NVS_KEY, "");
    prefs.end();

    if (_apiKey.length() > 0) return true;

    // Generate a random key from the ESP32 hardware RNG
    uint8_t buf[API_KEY_BYTES];
    esp_fill_random(buf, sizeof(buf));
    _apiKey = "";
    for (int i = 0; i < API_KEY_BYTES; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", buf[i]);
        _apiKey += hex;
    }

    prefs.begin(NVS_NS, false);
    prefs.putString(NVS_KEY, _apiKey);
    prefs.end();

    DEBUG_PRINTLN(F("[API] Generated new API key (stored in NVS)."));
    return true;
}

bool HttpApiServer::checkAuth(AsyncWebServerRequest* req) {
    if (!req->hasHeader("X-Api-Key") ||
        req->getHeader("X-Api-Key")->value() != _apiKey) {
        sendError(req, 401, "unauthorized");
        return false;
    }
    return true;
}

void HttpApiServer::sendOk(AsyncWebServerRequest* req) {
    req->send(200, "application/json", "{\"ok\":true}");
}

void HttpApiServer::sendError(AsyncWebServerRequest* req, int code, const char* msg) {
    StaticJsonDocument<64> doc;
    doc["error"] = msg;
    String out; serializeJson(doc, out);
    req->send(code, "application/json", out);
}

#endif // ENABLE_HTTP_API

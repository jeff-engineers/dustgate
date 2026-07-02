// =============================================================================
// HttpApiServer.cpp
// =============================================================================

#include "HttpApiServer.h"

#ifdef ENABLE_HTTP_API

#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <esp_random.h>      // hardware RNG for key generation
#include "../utils/MotionMath.h"
// AgentConfig.h provides getAnthropicKey/setAnthropicKey/reset without pulling
// in <WebServer.h>, which would conflict with ESPAsyncWebServer's HTTP enums.
#include "../utils/AgentConfig.h"
#include "../training/CalibrationStore.h"

#ifdef CONTROL_SMART_OUTLET
  #include "../control/SmartOutletControl.h"
  #include "../outlets/ShellyGen1Outlet.h"
  #include "../outlets/ShellyGen2Outlet.h"
#endif

static const char* NVS_NS  = "api_cfg";
static const char* NVS_KEY = "api_key";

// =============================================================================
// Construction
// =============================================================================

// djb2-style hash over the fields that should trigger a WS push.
// positionSteps is intentionally excluded — it jitters every loop.
static uint32_t statusFingerprint(const ApiStatus& s) {
    uint32_t h = 5381;
    for (const char* p = s.stateName; *p; ++p) h = ((h << 5) + h) ^ (uint8_t)*p;
    h = ((h << 5) + h) ^ (uint32_t)(s.currentStop  & 0xFF);
    h = ((h << 5) + h) ^ (uint32_t)(s.targetStop   & 0xFF);
    h = ((h << 5) + h) ^ (uint32_t)(s.homed      ? 1 : 0);
    h = ((h << 5) + h) ^ (uint32_t)(s.enabled    ? 2 : 0);
    h = ((h << 5) + h) ^ (uint32_t)(s.endstopHome? 4 : 0);
    return h;
}

HttpApiServer::HttpApiServer()
    : _server(API_PORT),
      _ws("/ws"),
      _mutex(nullptr),
      _lastStatusHash(0),
      _statusHashValid(false),
      _estopPending(false),
      _homePending(false),
      _enablePending(false),
      _disablePending(false),
      _movePending(false),  _moveStop(0),
      _jogPending(false),   _jogMM(0.0f),
      _clearCalPending(false)
#ifdef CONTROL_SMART_OUTLET
    , _outletConfigPending(false),
      _outletDeletePending(false), _outletDeleteSlot(0),
      _outletSavePending(false)
#endif
{}

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

    DEBUG_PRINT(F("[API] Key: ")); Serial.println(_apiKey);
    DEBUG_PRINTLN(F("[API] Include this as 'X-Api-Key: <key>' on all requests."));

    // Mount LittleFS — serves the Angular front-end bundle from flash.
    // 'true' = format on first mount if partition is empty (safe, idempotent).
    if (!LittleFS.begin(true)) {
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

    uint32_t fp = statusFingerprint(status);
    bool changed = !_statusHashValid || (fp != _lastStatusHash);

    if (!changed && _ws.count() == 0) return; // nothing to do

    if (changed) {
        // Serialise only when something meaningful changed
#ifdef CONTROL_SMART_OUTLET
        _lastStatusJson = buildStatusJson(status, outlets);
#else
        _lastStatusJson = buildStatusJson(status);
#endif
        _lastStatusHash  = fp;
        _statusHashValid = true;

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
#endif

#undef CONSUME

// =============================================================================
// Route registration
// =============================================================================

void HttpApiServer::registerRoutes() {

    // ------------------------------------------------------------------
    // GET /api/info — unauthenticated bootstrap endpoint.
    // Returns the API key so the Angular app can make authenticated
    // requests without the user having to enter it manually.
    // Security: only reachable on the local network (same as the device).
    // ------------------------------------------------------------------
    _server.on("/api/info", HTTP_GET, [this](AsyncWebServerRequest* req) {
        StaticJsonDocument<128> doc;
        doc["apiKey"]   = _apiKey;
        doc["numStops"] = NUM_STOPS;
        doc["version"]  = "1.0.0";
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
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
        StaticJsonDocument<512> doc;
        JsonArray arr = doc.createNestedArray("stops");
        for (int i = 0; i <= NUM_STOPS; i++) {
            JsonObject o = arr.createNestedObject();
            o["index"]  = i;
            o["mm"]     = serialized(String(g_stopPositionsMM[i], 2));
            o["steps"]  = (long)(g_stopPositionsMM[i] * stepsPerMM()) * (-HOME_DIRECTION);
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
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _homePending = true;
        xSemaphoreGive(_mutex);
        sendOk(req);
    });

    // POST /api/enable
    _server.on("/api/enable", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _enablePending = true;
        xSemaphoreGive(_mutex);
        sendOk(req);
    });

    // POST /api/disable
    _server.on("/api/disable", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _disablePending = true;
        xSemaphoreGive(_mutex);
        sendOk(req);
    });

    // POST /api/estop
    _server.on("/api/estop", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _estopPending = true;
        xSemaphoreGive(_mutex);
        sendOk(req);
    });

    // POST /api/clearcal
    _server.on("/api/clearcal", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _clearCalPending = true;
        xSemaphoreGive(_mutex);
        sendOk(req);
    });

    // POST /api/reboot
    _server.on("/api/reboot", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        sendOk(req);
        delay(200);
        ESP.restart();
    });

    // POST /api/wifireset
    _server.on("/api/wifireset", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        sendOk(req);
        delay(200);
        WiFiProvisioner::reset(); // does not return
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

    // GET /api/outlets — list with live readings
    _server.on("/api/outlets", HTTP_GET, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        // Data is embedded in the status push; return last known for GET
        req->send(200, "application/json", _lastStatusJson.length() ? _lastStatusJson : "{}");
    });

    // POST /api/outlets/ping   body: {"gen":1,"ip":"192.168.1.x"}
    // Runs poll in a one-shot FreeRTOS task so the async handler never blocks.
    _server.on("/api/outlets/ping", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!checkAuth(req)) return;
            StaticJsonDocument<128> doc;
            if (deserializeJson(doc, data, len)) { sendError(req, 400, "invalid JSON"); return; }
            int gen = doc["gen"] | 1;
            const char* ip = doc["ip"] | "";
            if (strlen(ip) == 0) { sendError(req, 400, "missing 'ip'"); return; }

            struct PingArgs {
                AsyncWebServerRequest* req;
                SmartOutlet*           outlet;
            };
            auto* args = new PingArgs {
                req,
                (gen == 2) ? (SmartOutlet*)new ShellyGen2Outlet(ip, "ping")
                           : (SmartOutlet*)new ShellyGen1Outlet(ip, "ping")
            };

            xTaskCreate([](void* arg) {
                auto* a = static_cast<PingArgs*>(arg);
                bool ok  = a->outlet->poll();
                float pw = a->outlet->getPowerW();
                delete a->outlet;

                StaticJsonDocument<64> resp;
                resp["reachable"] = ok;
                resp["powerW"]    = pw;
                String out; serializeJson(resp, out);
                a->req->send(200, "application/json", out);
                delete a;
                vTaskDelete(nullptr);
            }, "ping_task", 4096, args, 1, nullptr);
            // response sent from task — handler returns without calling send()
        }
    );

    // POST /api/outlets/save
    _server.on("/api/outlets/save", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
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

            StaticJsonDocument<256> doc;
            if (deserializeJson(doc, data, len)) { sendError(req, 400, "invalid JSON"); return; }

            OutletConfigCmd cmd;
            cmd.slot       = slot;
            cmd.generation = doc["gen"]       | 1;
            cmd.stopIndex  = doc["stop"]      | 0;
            cmd.thresholdW = doc["threshold"] | OUTLET_DEFAULT_THRESHOLD_W;
            strlcpy(cmd.ip,   doc["ip"]   | "",  sizeof(cmd.ip));
            strlcpy(cmd.name, doc["name"] | "",  sizeof(cmd.name));

            if (strlen(cmd.ip) == 0)   { sendError(req, 400, "missing 'ip'");   return; }
            if (strlen(cmd.name) == 0) { sendError(req, 400, "missing 'name'"); return; }

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
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _outletDeletePending = true; _outletDeleteSlot = slot;
        xSemaphoreGive(_mutex);
        sendOk(req);
    });

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

String HttpApiServer::buildStatusJson(const ApiStatus& s
#ifdef CONTROL_SMART_OUTLET
                                      , SmartOutletControl* outlets
#endif
) {
    StaticJsonDocument<1024> doc;

    doc["state"]         = s.stateName;
    doc["currentStop"]   = s.currentStop;
    doc["targetStop"]    = s.targetStop;
    doc["positionSteps"] = s.positionSteps;
    doc["homed"]         = s.homed;
    doc["enabled"]       = s.enabled;
    doc["endstopHome"]   = s.endstopHome;

    JsonArray stops = doc.createNestedArray("stops");
    for (int i = 0; i <= NUM_STOPS; i++) {
        JsonObject o = stops.createNestedObject();
        o["index"] = i;
        o["mm"]    = serialized(String(g_stopPositionsMM[i], 2));
    }

#ifdef CONTROL_SMART_OUTLET
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

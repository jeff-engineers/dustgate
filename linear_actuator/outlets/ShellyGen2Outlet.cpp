// =============================================================================
// ShellyGen2Outlet.cpp
// =============================================================================

#include "ShellyGen2Outlet.h"

#ifdef CONTROL_SMART_OUTLET

#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>

ShellyGen2Outlet::ShellyGen2Outlet(const char* ip, const char* name) {
    strlcpy(_ip,   ip,   sizeof(_ip));
    strlcpy(_name, name, sizeof(_name));
}

bool ShellyGen2Outlet::reresolve() {
    if (_host[0] == '\0') return false;
    IPAddress resolved = MDNS.queryHost(_host, 2000);
    if (resolved == IPAddress(0, 0, 0, 0)) return false;
    strlcpy(_ip, resolved.toString().c_str(), sizeof(_ip));
    return true;
}

bool ShellyGen2Outlet::poll() {
    // Paired by hostname with no address yet (DHCP outlet, no static IP) —
    // resolve before polling rather than burning a guaranteed-failed request.
    if (_ip[0] == '\0') {
        if (!reresolve()) {
            _reachable  = false;
            _lastPowerW = 0.0f;
            return false;
        }
    }
    if (doPoll()) return true;
    // Poll failed — the IP may be stale after a DHCP lease change. If we know
    // this outlet's mDNS hostname, re-resolve and retry once before giving up.
    if (reresolve()) return doPoll();
    return false;
}

// Same shape as poll() but with a caller-chosen timeout — the provisioning path
// passes a generous window so a marginal plug that can't answer the tight 400ms
// poll probe still gets provisioned on the first pass instead of flapping.
bool ShellyGen2Outlet::probe(uint32_t timeoutMs) {
    if (_ip[0] == '\0' && !reresolve()) return false;
    if (doPoll(timeoutMs)) return true;
    if (reresolve()) return doPoll(timeoutMs);
    return false;
}

bool ShellyGen2Outlet::doPoll(uint32_t timeoutMs) {
    char url[64];
    snprintf(url, sizeof(url), "http://%s/rpc/Switch.GetStatus?id=0", _ip);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(timeoutMs);

    int code = http.GET();
    if (code != 200) {
        http.end();
        _reachable  = false;
        _lastPowerW = 0.0f;
        return false;
    }

    StaticJsonDocument<64> filter;
    filter["apower"] = true;

    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();

    if (err) {
        _reachable  = false;
        _lastPowerW = 0.0f;
        return false;
    }

    _lastPowerW = doc["apower"] | 0.0f;
    _reachable  = true;
    return true;
}

// POST a JSON-RPC body to the plug's /rpc endpoint (used for SetConfig calls,
// which carry structured config that's awkward to URL-encode into a GET query).
bool ShellyGen2Outlet::rpcPost(const char* jsonBody) {
    if (_ip[0] == '\0' && !reresolve()) return false;

    // Up to two attempts: a connection-level failure (negative code) may mean the
    // stored IP is stale after a DHCP change, so re-resolve via mDNS and retry.
    for (int attempt = 0; attempt < 2; attempt++) {
        char url[48];
        snprintf(url, sizeof(url), "http://%s/rpc", _ip);

        HTTPClient http;
        http.begin(url);
        http.addHeader("Content-Type", "application/json");
        // Config writes hit flash — give them a generous window, not the fast-poll
        // timeout. (This runs only at provisioning time, never on the poll path.)
        http.setTimeout(OUTLET_RPC_WRITE_TIMEOUT_MS);
        int code = http.POST((uint8_t*)jsonBody, strlen(jsonBody));
        String body = (code > 0) ? http.getString() : String();
        http.end();

        if (code == 200) {
            // CAUTION: Shelly RPC returns HTTP 200 even for RPC-level failures —
            // the error rides in the body as {"error":{"code":..,"message":..}},
            // while success carries {"result":...}. So HTTP 200 alone is NOT
            // success; only the absence of an error object is. (This is why the
            // Ws/name writes reported "ok" yet nothing actually stored.)
            if (body.indexOf("\"error\"") < 0) return true;
            DEBUG_PRINT(F("[Outlets] rpc ")); DEBUG_PRINT(_ip);
            DEBUG_PRINT(F(" RPC error: ")); DEBUG_PRINTLN(body);
            return false;   // rejected params — retrying the same body won't help
        }

        DEBUG_PRINT(F("[Outlets] rpc POST ")); DEBUG_PRINT(_ip);
        DEBUG_PRINT(F(" HTTP ")); DEBUG_PRINT(code);
        DEBUG_PRINT(F("  body: ")); DEBUG_PRINTLN(body.length() ? body : String("(empty)"));

        // Connection-level failure (code < 0) → the IP may be stale; re-resolve
        // and retry once.
        if (code < 0 && attempt == 0 && reresolve()) continue;
        return false;
    }
    return false;
}

// Ws.SetConfig — tell the plug to open (and keep) an outbound WebSocket to us,
// so it pushes status changes instead of us polling it.
bool ShellyGen2Outlet::configureOutboundWs(const char* wsUrl) {
    char body[192];
    snprintf(body, sizeof(body),
             "{\"id\":1,\"method\":\"Ws.SetConfig\",\"params\":{\"config\":"
             "{\"enable\":true,\"server\":\"%s\"}}}",
             wsUrl);
    bool ok = rpcPost(body);
    DEBUG_PRINT(F("[Outlets] Ws.SetConfig ")); DEBUG_PRINT(_ip);
    DEBUG_PRINT(F(" -> ")); DEBUG_PRINTLN(ok ? F("ok") : F("FAILED"));
    return ok;
}

// Switch.SetConfig — set the plug's app-visible name (the label discovery reads
// back via ShellyDeviceName.h), so a plug is self-identifying after setup.
bool ShellyGen2Outlet::setName(const char* name) {
    // Minimal JSON escaping for the name (quotes/backslashes) — gate names are
    // user text. Everything else is passed through; control chars are unlikely
    // from the wizard's single-line input.
    char esc[48]; size_t j = 0;
    for (const char* p = name; *p && j < sizeof(esc) - 2; ++p) {
        if (*p == '"' || *p == '\\') { if (j < sizeof(esc) - 3) esc[j++] = '\\'; }
        esc[j++] = *p;
    }
    esc[j] = '\0';

    char body[160];

    // Preferred: the switch component's own name — what the app shows and edits
    // for a single-relay plug (and what ShellyDeviceName.h reads back first).
    snprintf(body, sizeof(body),
             "{\"id\":1,\"method\":\"Switch.SetConfig\",\"params\":"
             "{\"id\":0,\"config\":{\"name\":\"%s\"}}}",
             esc);
    if (rpcPost(body)) {
        DEBUG_PRINT(F("[Outlets] Switch.SetConfig name=")); DEBUG_PRINT(esc);
        DEBUG_PRINT(F(" @ ")); DEBUG_PRINT(_ip); DEBUG_PRINTLN(F(" -> ok"));
        return true;
    }

    // Fallback: the device-level name (Sys), for firmware/models that reject a
    // Switch.SetConfig name write.
    snprintf(body, sizeof(body),
             "{\"id\":1,\"method\":\"Sys.SetConfig\",\"params\":"
             "{\"config\":{\"device\":{\"name\":\"%s\"}}}}",
             esc);
    bool ok = rpcPost(body);
    DEBUG_PRINT(F("[Outlets] Sys.SetConfig device.name=")); DEBUG_PRINT(esc);
    DEBUG_PRINT(F(" @ ")); DEBUG_PRINT(_ip);
    DEBUG_PRINT(F(" -> ")); DEBUG_PRINTLN(ok ? F("ok (Switch failed, Sys ok)") : F("FAILED (both)"));
    return ok;
}

// Gen 2 RPC switch: GET http://<ip>/rpc/Switch.Set?id=0&on=true|false
bool ShellyGen2Outlet::setSwitch(bool on) {
    char url[80];
    snprintf(url, sizeof(url), "http://%s/rpc/Switch.Set?id=0&on=%s",
             _ip, on ? "true" : "false");

    HTTPClient http;
    http.begin(url);
    http.setTimeout(OUTLET_HTTP_TIMEOUT_MS);
    int code = http.GET();
    http.end();

    return code == 200;
}

#endif // CONTROL_SMART_OUTLET

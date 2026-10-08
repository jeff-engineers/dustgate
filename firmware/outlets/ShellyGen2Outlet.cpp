// =============================================================================
// ShellyGen2Outlet.cpp
// =============================================================================

#include "ShellyGen2Outlet.h"
#ifdef ARDUINO
#include "../config.h"   // where CONTROL_SMART_OUTLET is defined — the headers no longer drag it in
#endif

#if defined(CONTROL_SMART_OUTLET) || defined(DUSTGATE_NODE_PLUG_POLL) || defined(DUSTGATE_NATIVE)   // a node polls plugs for the primary

#include <cstdio>
#include <ArduinoJson.h>
#include "PlugHttp.h"

ShellyGen2Outlet::ShellyGen2Outlet(const char* ip, const char* name) {
    strlcpy(_ip,   ip,   sizeof(_ip));
    strlcpy(_name, name, sizeof(_name));
}

bool ShellyGen2Outlet::reresolve() {
    if (_host[0] == '\0') return false;
    // Serialised across tasks by the platform (the ESP's one mDNS querier, utils/MdnsLock.h).
    std::string resolved;
    if (!plughttp::resolveHost(_host, resolved, 2000)) return false;
    strlcpy(_ip, resolved.c_str(), sizeof(_ip));
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

    // The CONNECT timeout too, as TasmotaOutlet::doPoll() has always passed. Without it a plug with nothing at its address
    // (unplugged at the wall) cost the platform's whole connect budget — 5 s on the native brain — and plugs are polled one
    // after another, so one dead Shelly made every other plug's reading 5 s late, every pass (found 2026-10-06).
    const plughttp::Reply r = plughttp::get(url, timeoutMs, timeoutMs);
    if (r.code != 200) {
        _reachable  = false;
        _lastPowerW = 0.0f;
        return false;
    }

    StaticJsonDocument<64> filter;
    filter["apower"] = true;

    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, r.body,
                                               DeserializationOption::Filter(filter));

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

        // Config writes hit flash — give them a generous window, not the fast-poll
        // timeout. (This runs only at provisioning time, never on the poll path.)
        const plughttp::Reply r = plughttp::post(url, jsonBody, "application/json", OUTLET_RPC_WRITE_TIMEOUT_MS);
        const int code = r.code;
        const std::string& body = r.body;

        if (code == 200) {
            // CAUTION: Shelly RPC returns HTTP 200 even for RPC-level failures —
            // the error rides in the body as {"error":{"code":..,"message":..}},
            // while success carries {"result":...}. So HTTP 200 alone is NOT
            // success; only the absence of an error object is. (This is why the
            // Ws/name writes reported "ok" yet nothing actually stored.)
            if (body.find("\"error\"") == std::string::npos) return true;
            plughttp::log(std::string("[Outlets] rpc ") + _ip + " RPC error: " + body);
            return false;   // rejected params — retrying the same body won't help
        }

        plughttp::log(std::string("[Outlets] rpc POST ") + _ip + " HTTP " + std::to_string(code) +
                      "  body: " + (body.empty() ? std::string("(empty)") : body));

        // Connection-level failure (code < 0) → the IP may be stale; re-resolve
        // and retry once.
        if (code < 0 && attempt == 0 && reresolve()) continue;
        return false;
    }
    return false;
}

// Ws.GetConfig — who does this plug currently push to?
//
// The ownership authority (RFC §8). Read before any Ws.SetConfig: repointing a
// plug that belongs to another controller is silent theft, and the previous
// owner just stops hearing that its tool started.
bool ShellyGen2Outlet::readPushConfig(std::string& outServer, bool& outEnabled, uint32_t timeoutMs) {
    outServer = "";
    outEnabled = false;
    if (_ip[0] == '\0' && !reresolve()) return false;

    // A default argument binds to the STATIC type of the call, and the caller holds
    // a SmartOutlet*, whose default is 0 — so this arrived as 0 ms and HTTPClient
    // gave up on its first 10 ms poll: "HTTP -11" in ~25 ms, from a plug that
    // answers in 35 ms. Every Gen4 plug failed this way (found 2026-10-04), which
    // blocked takeover, rename and pairing, since a failed read is never permission.
    if (timeoutMs == 0) timeoutMs = OUTLET_RPC_WRITE_TIMEOUT_MS;

    char url[80];
    snprintf(url, sizeof(url), "http://%s/rpc/Ws.GetConfig", _ip);

    const plughttp::Reply r = plughttp::get(url, timeoutMs, timeoutMs);   // connect too: see doPoll()
    const int code = r.code;
    const std::string& body = r.body;

    if (code != 200) {
        plughttp::log(std::string("[Outlets] Ws.GetConfig ") + _ip + " -> HTTP " + std::to_string(code));
        return false;
    }

    StaticJsonDocument<64> filter;
    filter["server"] = true;
    filter["enable"] = true;
    StaticJsonDocument<192> doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
        plughttp::log(std::string("[Outlets] Ws.GetConfig ") + _ip + " -> unparseable: " + body);
        return false;
    }
    outServer  = doc["server"] | "";
    outEnabled = doc["enable"] | false;
    return true;
}

// The claim, read and decided in one call — SmartOutlet::readClaim().
bool ShellyGen2Outlet::readClaim(const char* ourHost, const char* deviceName, const char* ourName,
                                 plugclaim::Claim& out, std::string* pushUrl) {
    std::string server; bool enabled = false;
    if (!readPushConfig(server, enabled)) return false;
    out = plugclaim::decide(server.c_str(), enabled, ourHost ? ourHost : "", deviceName ? deviceName : "",
                            ourName ? ourName : "");
    if (pushUrl) *pushUrl = server;
    return true;
}

// Ws.SetConfig — tell the plug to open (and keep) an outbound WebSocket to us,
// so it pushes status changes instead of us polling it.
//
// CALLERS: check plugclaim::decide(...).mayRepoint FIRST. This function is the
// theft; the policy that stops it lives in outlets/PlugClaim.h.
bool ShellyGen2Outlet::configureOutboundWs(const char* wsUrl) {
    char body[192];
    snprintf(body, sizeof(body),
             "{\"id\":1,\"method\":\"Ws.SetConfig\",\"params\":{\"config\":"
             "{\"enable\":true,\"server\":\"%s\"}}}",
             wsUrl);
    bool ok = rpcPost(body);
    plughttp::log(std::string("[Outlets] Ws.SetConfig ") + _ip + " -> " + (ok ? "ok" : "FAILED"));
    return ok;
}

// Ws.SetConfig, in reverse. Unpairing has to undo what pairing did, or a plug
// you detached keeps dialling us forever and whoever we took it from stays deaf.
//
// Two cases, and the difference matters to somebody: if we took the plug from
// another controller we point it back at exactly where it was (the URL stored on
// the takeover), so that controller starts hearing from it again with no action
// on their part. If it was unclaimed when we found it, there is nothing to
// restore and we disable pushing — back to how it shipped.
bool ShellyGen2Outlet::releasePush(const char* restoreUrl) {
    char body[192];
    const bool restore = restoreUrl && *restoreUrl;
    if (restore) {
        snprintf(body, sizeof(body),
                 "{\"id\":1,\"method\":\"Ws.SetConfig\",\"params\":{\"config\":"
                 "{\"enable\":true,\"server\":\"%s\"}}}",
                 restoreUrl);
    } else {
        snprintf(body, sizeof(body),
                 "{\"id\":1,\"method\":\"Ws.SetConfig\",\"params\":{\"config\":"
                 "{\"enable\":false,\"server\":\"\"}}}");
    }
    bool ok = rpcPost(body);
    plughttp::log(std::string("[Outlets] Ws release ") + _ip + (restore ? " -> restored to " : " -> push disabled") +
                  (restore ? restoreUrl : "") + " " + (ok ? "ok" : "FAILED"));
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
        plughttp::log(std::string("[Outlets] Switch.SetConfig name=") + esc + " @ " + _ip + " -> ok");
        return true;
    }

    // Fallback: the device-level name (Sys), for firmware/models that reject a
    // Switch.SetConfig name write.
    snprintf(body, sizeof(body),
             "{\"id\":1,\"method\":\"Sys.SetConfig\",\"params\":"
             "{\"config\":{\"device\":{\"name\":\"%s\"}}}}",
             esc);
    bool ok = rpcPost(body);
    plughttp::log(std::string("[Outlets] Sys.SetConfig device.name=") + esc + " @ " + _ip + " -> " +
                  (ok ? "ok (Switch failed, Sys ok)" : "FAILED (both)"));
    return ok;
}

// Gen 2 RPC switch: GET http://<ip>/rpc/Switch.Set?id=0&on=true|false
bool ShellyGen2Outlet::setSwitch(bool on) {
    char url[80];
    snprintf(url, sizeof(url), "http://%s/rpc/Switch.Set?id=0&on=%s",
             _ip, on ? "true" : "false");

    return plughttp::get(url, OUTLET_HTTP_TIMEOUT_MS, OUTLET_HTTP_TIMEOUT_MS).code == 200;   // connect too: see doPoll()
}

#endif // CONTROL_SMART_OUTLET || DUSTGATE_NODE_PLUG_POLL || DUSTGATE_NATIVE

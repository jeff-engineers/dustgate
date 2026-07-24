// =============================================================================
// ShellyGen1Outlet.cpp
// =============================================================================

#include "ShellyGen1Outlet.h"

#ifdef CONTROL_SMART_OUTLET

#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>

ShellyGen1Outlet::ShellyGen1Outlet(const char* ip, const char* name) {
    strlcpy(_ip,   ip,   sizeof(_ip));
    strlcpy(_name, name, sizeof(_name));
}

bool ShellyGen1Outlet::reresolve() {
    if (_host[0] == '\0') return false;
    IPAddress resolved = MDNS.queryHost(_host, 2000);
    if (resolved == IPAddress(0, 0, 0, 0)) return false;
    strlcpy(_ip, resolved.toString().c_str(), sizeof(_ip));
    return true;
}

bool ShellyGen1Outlet::poll() {
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

bool ShellyGen1Outlet::doPoll() {
    char url[48];
    snprintf(url, sizeof(url), "http://%s/status", _ip);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(OUTLET_HTTP_TIMEOUT_MS);

    int code = http.GET();
    if (code != 200) {
        http.end();
        _reachable  = false;
        _lastPowerW = 0.0f;
        return false;
    }

    // Parse only the fields we need — filter reduces memory vs. full parse
    StaticJsonDocument<128> filter;
    filter["meters"][0]["power"] = true;

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();

    if (err) {
        _reachable  = false;
        _lastPowerW = 0.0f;
        return false;
    }

    _lastPowerW = doc["meters"][0]["power"] | 0.0f;
    _reachable  = true;
    return true;
}

// Gen 1 relay switch: GET http://<ip>/relay/0?turn=on|off
bool ShellyGen1Outlet::setSwitch(bool on) {
    char url[48];
    snprintf(url, sizeof(url), "http://%s/relay/0?turn=%s", _ip, on ? "on" : "off");

    HTTPClient http;
    http.begin(url);
    http.setTimeout(OUTLET_HTTP_TIMEOUT_MS);
    int code = http.GET();
    http.end();

    return code == 200;
}

#endif // CONTROL_SMART_OUTLET

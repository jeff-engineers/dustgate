// =============================================================================
// ShellyGen1Outlet.cpp
// =============================================================================

#include "ShellyGen1Outlet.h"

#ifdef CONTROL_SMART_OUTLET

#include <HTTPClient.h>
#include <ArduinoJson.h>

ShellyGen1Outlet::ShellyGen1Outlet(const char* ip, const char* name) {
    strlcpy(_ip,   ip,   sizeof(_ip));
    strlcpy(_name, name, sizeof(_name));
}

bool ShellyGen1Outlet::poll() {
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

// =============================================================================
// ShellyGen2Outlet.cpp
// =============================================================================

#include "ShellyGen2Outlet.h"

#ifdef CONTROL_SMART_OUTLET

#include <HTTPClient.h>
#include <ArduinoJson.h>

ShellyGen2Outlet::ShellyGen2Outlet(const char* ip, const char* name) {
    strlcpy(_ip,   ip,   sizeof(_ip));
    strlcpy(_name, name, sizeof(_name));
}

bool ShellyGen2Outlet::poll() {
    char url[64];
    snprintf(url, sizeof(url), "http://%s/rpc/Switch.GetStatus?id=0", _ip);

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

#endif // CONTROL_SMART_OUTLET

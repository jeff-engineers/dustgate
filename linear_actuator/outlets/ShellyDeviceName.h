// =============================================================================
// ShellyDeviceName.h — fetch the user-assigned name of a Shelly device
//
// Gen 1:  GET /settings                       -> top-level "name"
// Gen 2+: GET /rpc/Switch.GetConfig?id=0       -> "name" (switch instance name)
//         falls back to GET /rpc/Sys.GetConfig -> "device": { "name": ... }
//
// For a single-relay Plug (this project's reference hardware), the label the
// Shelly app shows/lets you edit for the device is actually the switch
// component's own name (Switch.GetConfig), not the device-level name
// (Sys.GetConfig) — Shelly.GetDeviceInfo (even with ?ident=true) has no name
// field at all. Multi-channel devices may only have the device-level name
// set, so that's tried as a fallback. Confirmed against:
//   https://shelly-api-docs.shelly.cloud/gen2/ComponentsAndServices/Switch/#configuration
//   https://shelly-api-docs.shelly.cloud/gen2/ComponentsAndServices/Sys/
//   https://shelly-api-docs.shelly.cloud/gen2/ComponentsAndServices/Shelly/#shellygetdeviceinfo
//   https://shelly-api-docs.shelly.cloud/gen1/#settings
// =============================================================================

#pragma once
#include <Arduino.h>
#include "../config.h"

#ifdef CONTROL_SMART_OUTLET

#include <HTTPClient.h>
#include <ArduinoJson.h>

inline String fetchShellyName(const char* url, const char* jsonPath) {
    HTTPClient http;
    http.begin(url);
    http.setTimeout(OUTLET_HTTP_TIMEOUT_MS);
    int code = http.GET();

    String name;
    if (code == 200) {
        String body = http.getString();
        StaticJsonDocument<96> filter;
        if (strcmp(jsonPath, "name") == 0) {
            filter["name"] = true;
        } else {
            filter["device"]["name"] = true;
        }
        StaticJsonDocument<192> doc;
        if (!deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
            name = (strcmp(jsonPath, "name") == 0) ? (doc["name"] | "")
                                                     : (doc["device"]["name"] | "");
        }
        if (name.length() == 0) {
            DEBUG_PRINT(F("      [name] "));
            DEBUG_PRINT(url);
            DEBUG_PRINT(F(" -> 200, no name found. Raw body: "));
            DEBUG_PRINTLN(body);
        }
    } else {
        DEBUG_PRINT(F("      [name] "));
        DEBUG_PRINT(url);
        DEBUG_PRINT(F(" -> HTTP "));
        DEBUG_PRINTLN(code);
    }
    http.end();
    return name;
}

// Returns "" if unset, unreachable, or the response didn't parse.
inline String fetchShellyDeviceName(const char* ip, int gen) {
    if (gen < 2) {   // < 2, not != 2: Gen3+ uses the Gen2 RPC endpoints
        char url[96];
        snprintf(url, sizeof(url), "http://%s/settings", ip);
        return fetchShellyName(url, "name");
    }

    char switchUrl[96];
    snprintf(switchUrl, sizeof(switchUrl), "http://%s/rpc/Switch.GetConfig?id=0", ip);
    String name = fetchShellyName(switchUrl, "name");
    if (name.length() > 0) return name;

    char sysUrl[96];
    snprintf(sysUrl, sizeof(sysUrl), "http://%s/rpc/Sys.GetConfig", ip);
    return fetchShellyName(sysUrl, "device.name");
}

#endif // CONTROL_SMART_OUTLET

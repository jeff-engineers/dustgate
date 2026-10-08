#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include "OutletTimeouts.h"
#include "PlugHttp.h"
#ifdef ARDUINO
#include "../config.h"   // CONTROL_SMART_OUTLET
#endif

#if defined(CONTROL_SMART_OUTLET) || defined(DUSTGATE_NATIVE)

#include <ArduinoJson.h>

inline std::string fetchShellyName(const char* url, const char* jsonPath) {
    const plughttp::Reply r = plughttp::get(url, OUTLET_HTTP_TIMEOUT_MS, OUTLET_HTTP_TIMEOUT_MS);   // connect too: see ShellyGen2Outlet::doPoll()

    std::string name;
    if (r.code == 200) {
        StaticJsonDocument<96> filter;
        if (strcmp(jsonPath, "name") == 0) {
            filter["name"] = true;
        } else {
            filter["device"]["name"] = true;
        }
        StaticJsonDocument<192> doc;
        if (!deserializeJson(doc, r.body, DeserializationOption::Filter(filter))) {
            name = (strcmp(jsonPath, "name") == 0) ? (doc["name"] | "")
                                                     : (doc["device"]["name"] | "");
        }
        if (name.empty()) {
            plughttp::log(std::string("      [name] ") + url + " -> 200, no name found. Raw body: " + r.body);
        }
    } else {
        plughttp::log(std::string("      [name] ") + url + " -> HTTP " + std::to_string(r.code));
    }
    return name;
}

// Returns "" if unset, unreachable, or the response didn't parse.
inline std::string fetchShellyDeviceName(const char* ip, int gen) {
    if (gen < 2) {   // < 2, not != 2: Gen3+ uses the Gen2 RPC endpoints
        char url[96];
        snprintf(url, sizeof(url), "http://%s/settings", ip);
        return fetchShellyName(url, "name");
    }

    char switchUrl[96];
    snprintf(switchUrl, sizeof(switchUrl), "http://%s/rpc/Switch.GetConfig?id=0", ip);
    std::string name = fetchShellyName(switchUrl, "name");
    if (!name.empty()) return name;

    char sysUrl[96];
    snprintf(sysUrl, sizeof(sysUrl), "http://%s/rpc/Sys.GetConfig", ip);
    return fetchShellyName(sysUrl, "device.name");
}

#endif // CONTROL_SMART_OUTLET

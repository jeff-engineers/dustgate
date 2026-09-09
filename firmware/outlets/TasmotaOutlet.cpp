// =============================================================================
// TasmotaOutlet.cpp
// =============================================================================

#include "TasmotaOutlet.h"

#ifdef CONTROL_SMART_OUTLET

#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>

TasmotaOutlet::TasmotaOutlet(const char* ip, const char* name) {
    strlcpy(_ip,   ip,   sizeof(_ip));
    strlcpy(_name, name, sizeof(_name));
}

// Same DHCP-survival trick as ShellyGen2Outlet: a paired plug knows its mDNS
// hostname, so a stale IP after a lease change costs one re-resolve instead of
// going silently unreachable.
bool TasmotaOutlet::reresolve() {
    if (_host[0] == '\0') return false;
    IPAddress resolved = MDNS.queryHost(_host, 2000);
    if (resolved == IPAddress(0, 0, 0, 0)) return false;
    strlcpy(_ip, resolved.toString().c_str(), sizeof(_ip));
    return true;
}

bool TasmotaOutlet::poll() {
    if (_ip[0] == '\0') {
        if (!reresolve()) {
            _reachable  = false;
            _lastPowerW = 0.0f;
            return false;
        }
    }
    if (doPoll()) return true;
    if (reresolve()) return doPoll();
    return false;
}

bool TasmotaOutlet::probe(uint32_t timeoutMs) {
    if (_ip[0] == '\0' && !reresolve()) return false;
    if (doPoll(timeoutMs)) return true;
    if (reresolve()) return doPoll(timeoutMs);
    return false;
}

bool TasmotaOutlet::doPoll(uint32_t timeoutMs) {
    // Status 8 is Tasmota's sensor report. The %20 is a literal space in the
    // command — "Status 8" — not an encoding of ours to strip.
    char url[64];
    snprintf(url, sizeof(url), "http://%s/cm?cmnd=Status%%208", _ip);

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

    // Status 8 answers with the whole sensor tree — voltage, current, today's
    // and yesterday's energy totals, and on a multi-channel device an array per
    // channel. Filtering to the one field keeps the parse bounded on a device
    // whose reply we do not control the size of.
    //
    //   {"StatusSNS":{"Time":"...","ENERGY":{"Power":12.3, ...}}}
    StaticJsonDocument<128> filter;
    filter["StatusSNS"]["ENERGY"]["Power"] = true;

    StaticJsonDocument<192> doc;
    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();

    if (err) {
        _reachable  = false;
        _lastPowerW = 0.0f;
        return false;
    }

    // A REACHABLE PLUG WITH NO ENERGY BLOCK IS NOT REACHABLE, for our purposes.
    // Tasmota answers Status 8 on any build, including one with no energy
    // monitor fitted — the reply is simply missing ENERGY. `| 0.0f` would turn
    // that into a confident "0 watts", i.e. a tool that is never on, forever,
    // with nothing anywhere saying why. Treat it as unreachable so the UI shows
    // the plug as a problem instead of the tool as idle.
    JsonVariant p = doc["StatusSNS"]["ENERGY"]["Power"];
    if (p.isNull()) {
        _reachable  = false;
        _lastPowerW = 0.0f;
        return false;
    }

    _lastPowerW = p.as<float>();
    _reachable  = true;
    return true;
}

#endif  // CONTROL_SMART_OUTLET

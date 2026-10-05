// =============================================================================
// PlugHttpEsp.cpp — plughttp (PlugHttp.h) on the ESP32: Arduino's HTTPClient and ESPmDNS.
// =============================================================================
#include "PlugHttp.h"
#include "../config.h"   // CONTROL_SMART_OUTLET lives here, so it must precede the guard; also DEBUG_PRINTLN

#if defined(CONTROL_SMART_OUTLET) || defined(DUSTGATE_NODE_PLUG_POLL)   // a node polls plugs for the primary

#include <Arduino.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include "../utils/MdnsLock.h"   // one mDNS search at a time, across every task

namespace plughttp {

Reply get(const std::string& url, uint32_t readTimeoutMs, uint32_t connectTimeoutMs) {
    Reply r;
    HTTPClient http;
    http.begin(url.c_str());
    // The CONNECT timeout is its own budget (setTimeout() bounds the read) and defaults to seconds:
    // see TasmotaOutlet.cpp for what leaving it alone cost a sweep.
    if (connectTimeoutMs) http.setConnectTimeout(connectTimeoutMs);
    http.setTimeout(readTimeoutMs);
    r.code = http.GET();
    // getString(), NOT getStream(): a Tasmota answers chunked, and the raw stream still has the framing.
    if (r.code == 200) r.body = http.getString().c_str();
    http.end();
    return r;
}

Reply post(const std::string& url, const std::string& body, const char* contentType, uint32_t timeoutMs) {
    Reply r;
    HTTPClient http;
    http.begin(url.c_str());
    if (contentType && *contentType) http.addHeader("Content-Type", contentType);
    http.setTimeout(timeoutMs);
    r.code = http.POST((uint8_t*)body.data(), body.size());
    if (r.code > 0) r.body = http.getString().c_str();
    http.end();
    return r;
}

bool resolveHost(const char* host, std::string& ipOut, uint32_t timeoutMs) {
    if (!host || !*host) return false;
    // Runs on the outlet poller's task, which is a THIRD thing querying the one mDNS searcher.
    mdnslock::Guard lock(host);
    if (!lock.held()) return false;
    IPAddress resolved = MDNS.queryHost(host, timeoutMs);
    if (resolved == IPAddress(0, 0, 0, 0)) return false;
    ipOut = resolved.toString().c_str();
    return true;
}

void log(const std::string& line) { DEBUG_PRINTLN(line.c_str()); }

}  // namespace plughttp

#endif

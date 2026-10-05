// =============================================================================
// outlets/PlugHttp.h — the ONE seam between a smart-plug driver and the network.
//
// ShellyGen2Outlet and TasmotaOutlet each built a URL, set timeouts, issued a GET or POST, checked the code
// and parsed — ten HTTPClient blocks in two files, which is where the Gen4 timeout bug lived (a default
// argument that arrived as 0 ms). The protocols are the same on every platform; only the socket is not.
// So the drivers talk to THIS, and each platform supplies it: Arduino's HTTPClient and ESPmDNS on the ESP32
// (outlets/PlugHttpEsp.cpp), a Boost.Beast client and the system resolver on the native brain.
//
// PURE declarations — no Arduino.h, no ArduinoJson — so the drivers compile on a laptop.
// =============================================================================
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

// strlcpy is BSD/Arduino; glibc only has it from 2.38. One definition so the drivers stay unchanged.
#if !defined(ARDUINO) && !defined(__APPLE__) && !defined(strlcpy)
#if !(defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 38)))
inline size_t strlcpy(char* dst, const char* src, size_t n) {
    const size_t l = std::strlen(src);
    if (n) { const size_t c = l >= n ? n - 1 : l; std::memcpy(dst, src, c); dst[c] = '\0'; }
    return l;
}
#endif
#endif

namespace plughttp {

struct Reply {
    int         code = -1;   // the HTTP status, or negative when the request never completed
    std::string body;        // only read on a 200 for get(); on any answered request for post()
};

// A GET. `connectTimeoutMs` 0 leaves the platform's own connect budget alone — a sweep over addresses with
// nothing at them spends all its time connecting, so it must be able to bound that too.
Reply get(const std::string& url, uint32_t readTimeoutMs, uint32_t connectTimeoutMs = 0);
Reply post(const std::string& url, const std::string& body, const char* contentType, uint32_t timeoutMs);

// An mDNS host name ("shellyplugus-abc123", no ".local") to a dotted address. False when nothing answered.
// Serialised across tasks where the platform needs it (the ESP's one mDNS querier, utils/MdnsLock.h).
bool resolveHost(const char* host, std::string& ipOut, uint32_t timeoutMs);

// One console line, no trailing newline.
void log(const std::string& line);

}  // namespace plughttp

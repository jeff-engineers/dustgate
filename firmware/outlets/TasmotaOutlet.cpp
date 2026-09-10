// =============================================================================
// TasmotaOutlet.cpp
// =============================================================================

#include "TasmotaOutlet.h"

#ifdef CONTROL_SMART_OUTLET

#include <HTTPClient.h>
#include <ArduinoJson.h>

TasmotaOutlet::TasmotaOutlet(const char* ip, const char* name) {
    strlcpy(_ip,   ip,   sizeof(_ip));
    strlcpy(_name, name, sizeof(_name));
}

// NO reresolve() HERE, AND ITS ABSENCE IS THE DESIGN.
//
// ShellyGen2Outlet recovers a DHCP lease change by re-resolving its stored mDNS
// hostname. This class carried a verbatim copy of that until 2026-09-09, with a
// comment calling it "the same DHCP-survival trick" — and it could never once
// have worked. Tasmota's mDNS needs USE_DISCOVERY compiled in and is not in the
// precompiled builds (the same fact that makes discovery need an IP sweep; see
// the warning on MdnsHit::devicetype in utils/MdnsQuery.h). `_host` is never
// even populated for a Tasmota, because the sweep finds these by address. So it
// returned false on its first line every time, and poll()'s retry was a no-op
// dressed as a safety net.
//
// A dead branch that reads like recovery is worse than no recovery at all: it
// answers the question "what happens when the plug's address changes?" wrongly,
// and stops anyone asking again. Now poll() simply fails, which is the truth.
//
// RECOVERY BELONGS ELSEWHERE, and Mem1 is what makes it possible: the claim we
// wrote is a durable identifier that survives a lease change, so a plug that
// goes missing can be found again by sweeping and reading Mem1 for the one that
// says it is ours. That is a ~60s blocking pass over 254 addresses — categorically
// not something to run from the poll task on a failed read — so it is a
// deliberate, user-visible action. See docs/tool-sensing-rfc.md §12.
bool TasmotaOutlet::poll() {
    if (_ip[0] == '\0') {
        _reachable  = false;
        _lastPowerW = 0.0f;
        return false;
    }
    return doPoll();
}

bool TasmotaOutlet::probe(uint32_t timeoutMs) {
    if (_ip[0] == '\0') return false;
    return doPoll(timeoutMs);
}

bool TasmotaOutlet::doPoll(uint32_t timeoutMs) {
    // Status 8 is Tasmota's sensor report. The %20 is a literal space in the
    // command — "Status 8" — not an encoding of ours to strip.
    char url[64];
    snprintf(url, sizeof(url), "http://%s/cm?cmnd=Status%%208", _ip);

    HTTPClient http;
    http.begin(url);
    // CONNECT TIMEOUT TOO, and it is the one that matters here. setTimeout()
    // bounds the socket READ; establishing the connection has its own budget,
    // and without this it defaults to seconds. A sweep spends almost all its
    // time on addresses with nothing at them, where connect is the entire cost
    // — 10 addresses took 45s before this line existed, against a 250ms
    // timeout that was doing nothing at all.
    http.setConnectTimeout(timeoutMs);
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

    // getString(), NOT getStream(). Tasmota answers `Transfer-Encoding: chunked`
    // and getStream() hands back the raw socket WITH the chunk framing still in
    // it — hex length lines and CRLFs — so ArduinoJson chokes on the first chunk
    // header before it ever reaches the JSON. getString() decodes the framing.
    //
    // ShellyGen2Outlet streams the same way and is fine because a Shelly sends
    // Content-Length. That difference cost a bench session: the parse failed on
    // hardware while curl showed a perfectly good reply, because curl decodes
    // chunking and getStream() does not.
    //
    // Safe to buffer: Status 8 is a few hundred bytes. Do not copy this to an
    // endpoint that can answer with kilobytes.
    StaticJsonDocument<192> doc;
    const String body = http.getString();
    http.end();
    DeserializationError err = deserializeJson(doc, body,
                                               DeserializationOption::Filter(filter));

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

// -----------------------------------------------------------------------------
// Ownership marker
//
// Mem1 is queried by sending the command with no argument, and set by sending it
// with one. Both answer with the same shape: {"Mem1":"<value>"}.
// -----------------------------------------------------------------------------

bool TasmotaOutlet::readOwner(String& out, uint32_t timeoutMs) {
    if (_ip[0] == '\0') return false;

    char url[64];
    snprintf(url, sizeof(url), "http://%s/cm?cmnd=Mem1", _ip);

    HTTPClient http;
    http.begin(url);
    // CONNECT TIMEOUT TOO, and it is the one that matters here. setTimeout()
    // bounds the socket READ; establishing the connection has its own budget,
    // and without this it defaults to seconds. A sweep spends almost all its
    // time on addresses with nothing at them, where connect is the entire cost
    // — 10 addresses took 45s before this line existed, against a 250ms
    // timeout that was doing nothing at all.
    http.setConnectTimeout(timeoutMs);
    http.setTimeout(timeoutMs);
    if (http.GET() != 200) { http.end(); return false; }

    // getString() for the same reason doPoll() uses it — Tasmota chunks its
    // replies and getStream() would deliver the framing along with them.
    StaticJsonDocument<128> doc;
    const String body = http.getString();
    http.end();
    DeserializationError err = deserializeJson(doc, body);
    if (err) return false;

    // A plug that answers without a Mem1 key is not "unclaimed" — it is a plug
    // whose reply we did not understand, and the caller must be able to tell
    // those apart. Absent → false, so the difference survives.
    JsonVariant v = doc["Mem1"];
    if (v.isNull()) return false;

    out = v.as<const char*>();
    return true;
}

bool TasmotaOutlet::writeOwner(const char* owner) {
    if (_ip[0] == '\0') return false;

    // Tasmota clears a Mem to empty when the argument is the literal two-char
    // token `"` — a bare `Mem1` with no argument is a QUERY, not a clear, so
    // sending an empty string would read the value back and change nothing.
    const bool clearing = (owner == nullptr || owner[0] == '\0');

    char url[96];
    if (clearing) {
        snprintf(url, sizeof(url), "http://%s/cm?cmnd=Mem1%%20%%22", _ip);
    } else {
        // Hostnames are [A-Za-z0-9-], so no escaping is needed beyond the space
        // that separates the command from its argument.
        snprintf(url, sizeof(url), "http://%s/cm?cmnd=Mem1%%20%s", _ip, owner);
    }

    HTTPClient http;
    http.begin(url);
    http.setConnectTimeout(OUTLET_RPC_WRITE_TIMEOUT_MS);
    http.setTimeout(OUTLET_RPC_WRITE_TIMEOUT_MS);
    const bool ok = (http.GET() == 200);
    http.end();

    if (ok) {
        DEBUG_PRINT(F("[Outlets] Tasmota Mem1 "));
        DEBUG_PRINT(clearing ? "cleared" : owner);
        DEBUG_PRINT(F(" on ")); DEBUG_PRINTLN(_ip);
    }
    return ok;
}

// Fire-and-check one Tasmota command. Returns whether it answered 200; the body
// is not parsed, because every one of these answers with the value it just set
// and there is nothing to learn from reading it back that a 200 does not say.
static bool sendCmd(const char* ip, const char* cmd) {
    char url[96];
    snprintf(url, sizeof(url), "http://%s/cm?cmnd=%s", ip, cmd);
    HTTPClient http;
    http.begin(url);
    http.setConnectTimeout(OUTLET_RPC_WRITE_TIMEOUT_MS);
    http.setTimeout(OUTLET_RPC_WRITE_TIMEOUT_MS);
    const bool ok = (http.GET() == 200);
    http.end();
    return ok;
}

static bool sendCmd(const char* ip, const char* cmd);

// Percent-encode a value for the /cm?cmnd= query string.
//
// writeOwner() gets away without this because a hostname is [A-Za-z0-9-]. A
// DEVICE NAME is user text — "Table Saw", "Jointer · dustgate" — and the space
// alone would truncate the command at the first word, silently renaming the plug
// to something shorter than asked. The middle dot in an owner suffix is
// multi-byte UTF-8, which encodes per byte.
static void urlEncode(const char* in, char* out, size_t outLen) {
    static const char* kHex = "0123456789ABCDEF";
    size_t j = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p && j + 4 < outLen; p++) {
        const unsigned char c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = (char)c;
        } else {
            out[j++] = '%'; out[j++] = kHex[c >> 4]; out[j++] = kHex[c & 0x0F];
        }
    }
    out[j] = '\0';
}

// Tasmota's DeviceName. The equivalent of a Shelly's Switch.SetConfig name, and
// like it, the label the picker shows.
//
// DeviceName also drives FriendlyName1 when that has not been set separately,
// which is what makes it the right one of Tasmota's several name fields: it is
// the one the plug's own web UI puts at the top of the page, so a plug renamed
// here reads the same in both places.
bool TasmotaOutlet::setName(const char* name) {
    if (_ip[0] == '\0') return false;
    char enc[128];
    urlEncode(name, enc, sizeof(enc));
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "DeviceName%%20%s", enc);
    const bool ok = sendCmd(_ip, cmd);
    DEBUG_PRINT(F("[Outlets] Tasmota DeviceName=")); DEBUG_PRINT(name);
    DEBUG_PRINT(F(" @ ")); DEBUG_PRINT(_ip);
    DEBUG_PRINT(F(" -> ")); DEBUG_PRINTLN(ok ? F("ok") : F("FAILED"));
    return ok;
}

// Read it back. Same query-with-no-argument shape as Mem1.
bool TasmotaOutlet::readName(String& out, uint32_t timeoutMs) {
    if (_ip[0] == '\0') return false;

    char url[64];
    snprintf(url, sizeof(url), "http://%s/cm?cmnd=DeviceName", _ip);

    HTTPClient http;
    http.begin(url);
    http.setConnectTimeout(timeoutMs);
    http.setTimeout(timeoutMs);
    if (http.GET() != 200) { http.end(); return false; }

    StaticJsonDocument<192> doc;
    const String body = http.getString();
    http.end();
    if (deserializeJson(doc, body)) return false;

    JsonVariant v = doc["DeviceName"];
    if (v.isNull()) return false;
    out = v.as<const char*>();
    return true;
}

bool TasmotaOutlet::provision(const char* owner) {
    if (_ip[0] == '\0') return false;

    if (!writeOwner(owner)) return false;

    // Boot ON first, THEN lock. The other order nails down whatever state the
    // plug happens to be in, and a plug locked OFF is a tool that silently has
    // no power — the failure this whole sequence exists to prevent.
    const bool onState = sendCmd(_ip, "PowerOnState%201");
    const bool locked  = sendCmd(_ip, "PowerLock%201");

    if (onState && locked) {
        DEBUG_PRINT(F("[Outlets] Tasmota ")); DEBUG_PRINT(_ip);
        DEBUG_PRINTLN(F(" locked on — its Toggle button is now inert."));
    } else {
        // The claim landed and the lock did not. Say so rather than failing the
        // whole pairing: a claimed plug that can still be toggled is a working
        // sensor with a confusing web page, not a broken one.
        DEBUG_PRINT(F("[Outlets] Tasmota ")); DEBUG_PRINT(_ip);
        DEBUG_PRINTLN(F(" claimed, but PowerOnState/PowerLock did not take."));
    }
    return true;
}

bool TasmotaOutlet::release() {
    if (_ip[0] == '\0') return false;
    // Unlock BEFORE clearing the claim. If the second call fails, the plug is
    // still marked as ours and still operable — which is recoverable. The other
    // order can leave one that nobody owns and nobody can switch.
    sendCmd(_ip, "PowerLock%200");
    return writeOwner("");
}

#endif  // CONTROL_SMART_OUTLET

// =============================================================================
// TasmotaOutlet.cpp
// =============================================================================

#include "TasmotaOutlet.h"
#ifdef ARDUINO
#include "../config.h"   // where CONTROL_SMART_OUTLET is defined — the headers no longer drag it in
#endif

#if defined(CONTROL_SMART_OUTLET) || defined(DUSTGATE_NODE_PLUG_POLL) || defined(DUSTGATE_NATIVE)   // a node polls plugs for the primary

#include <cctype>
#include <cstdio>
#include <ArduinoJson.h>
#include "PlugHttp.h"

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

    // CONNECT TIMEOUT TOO, and it is the one that matters here. The read timeout bounds the socket
    // READ; establishing the connection has its own budget, and without this it defaults to seconds.
    // A sweep spends almost all its time on addresses with nothing at them, where connect is the
    // entire cost — 10 addresses took 45s before this was passed, against a 250ms timeout that was
    // doing nothing at all.
    const plughttp::Reply reply = plughttp::get(url, timeoutMs, timeoutMs);
    if (reply.code != 200) {
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

    // The WHOLE body, not a stream. Tasmota answers `Transfer-Encoding: chunked` and a raw stream
    // hands back the chunk framing — hex length lines and CRLFs — so ArduinoJson chokes on the first
    // chunk header before it ever reaches the JSON. plughttp::get() decodes the framing.
    //
    // (Shelly used to stream and was fine because it sends Content-Length. That difference cost a
    // bench session: the parse failed on hardware while curl showed a perfectly good reply, because
    // curl decodes chunking and a raw stream does not.)
    //
    // Safe to buffer: Status 8 is a few hundred bytes. Do not copy this to an
    // endpoint that can answer with kilobytes.
    StaticJsonDocument<192> doc;
    DeserializationError err = deserializeJson(doc, reply.body,
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

    // A MULTI-CHANNEL METER REPORTS `Power` AS AN ARRAY, and `as<float>()` on a
    // JSON array yields 0.0 — which is a working meter reported as a tool that
    // is never on, forever. Exactly the failure the guard above exists to
    // prevent, arriving by a different door: ENERGY is present, so that check
    // does not fire.
    //
    // THIS BITES ON THE FIRST EM2, not on the first ganged pair. An Athom EM2
    // has two channels in hardware whether or not both have a clamp on them, so
    // it answers with an array either way. There is no single-tool wiring that
    // avoids it — a lone tool on an EM2 reads 0 W just as thoroughly as two.
    // And §6.0 of docs/tool-sensing-rfc.md now RECOMMENDS the EM2 over building
    // our own sensor, so this is the normal case rather than the edge one.
    //
    // Refusing is deliberately the whole fix for now. Picking a channel here
    // would mean guessing which one carries the tool, and a meter silently
    // watching the wrong channel is worse than one that says it cannot cope.
    // Channel selection (`sensor.outlet.channel`, absent = scalar) is the
    // follow-up.
    if (p.is<JsonArray>()) {
        // Once per plug, not once per poll: this runs on the poll interval
        // forever, and a line every second would bury the log it is trying to
        // be found in. Cleared below, so a plug that is swapped or reconfigured
        // says so again.
        if (!_warnedMultiChannel) {
            _warnedMultiChannel = true;
            plughttp::log(std::string("[Outlets] Tasmota at ") + _ip + " reports Power as an ARRAY \xE2\x80\x94 this is a multi-channel");
            plughttp::log("          meter (Athom EM2/EM6). DustGate cannot yet say WHICH");
            plughttp::log("          channel a tool is on, so it is refusing to guess.");
            plughttp::log("          Treating it as unreachable rather than as 0 W.");
        }
        _reachable  = false;
        _lastPowerW = 0.0f;
        return false;
    }

    _warnedMultiChannel = false;
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

bool TasmotaOutlet::readOwner(std::string& out, uint32_t timeoutMs) {
    if (_ip[0] == '\0') return false;

    char url[64];
    snprintf(url, sizeof(url), "http://%s/cm?cmnd=Mem1", _ip);

    // The connect timeout too, for the reason given in doPoll().
    const plughttp::Reply reply = plughttp::get(url, timeoutMs, timeoutMs);
    if (reply.code != 200) return false;

    // The whole decoded body, for the same reason doPoll() reads it so.
    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, reply.body);
    if (err) return false;

    // A plug that answers without a Mem1 key is not "unclaimed" — it is a plug
    // whose reply we did not understand, and the caller must be able to tell
    // those apart. Absent → false, so the difference survives.
    JsonVariant v = doc["Mem1"];
    if (v.isNull()) return false;

    out = v.as<const char*>();
    return true;
}

// Fire-and-check one Tasmota command. Returns whether it answered 200; the body
// is not parsed, because every one of these answers with the value it just set
// and there is nothing to learn from reading it back that a 200 does not say.
static bool sendCmd(const char* ip, const char* cmd) {
    char url[96];
    snprintf(url, sizeof(url), "http://%s/cm?cmnd=%s", ip, cmd);
    return plughttp::get(url, OUTLET_RPC_WRITE_TIMEOUT_MS, OUTLET_RPC_WRITE_TIMEOUT_MS).code == 200;
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

    const bool ok = plughttp::get(url, OUTLET_RPC_WRITE_TIMEOUT_MS, OUTLET_RPC_WRITE_TIMEOUT_MS).code == 200;

    if (ok) plughttp::log(std::string("[Outlets] Tasmota Mem1 ") + (clearing ? "cleared" : owner) + " on " + _ip);
    return ok;
}

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
    plughttp::log(std::string("[Outlets] Tasmota DeviceName=") + name + " @ " + _ip + " -> " + (ok ? "ok" : "FAILED"));
    return ok;
}

// Read it back. Same query-with-no-argument shape as Mem1.
bool TasmotaOutlet::readName(std::string& out, uint32_t timeoutMs) {
    if (_ip[0] == '\0') return false;

    char url[64];
    snprintf(url, sizeof(url), "http://%s/cm?cmnd=DeviceName", _ip);

    const plughttp::Reply reply = plughttp::get(url, timeoutMs, timeoutMs);
    if (reply.code != 200) return false;

    StaticJsonDocument<192> doc;
    if (deserializeJson(doc, reply.body)) return false;

    JsonVariant v = doc["DeviceName"];
    if (v.isNull()) return false;
    out = v.as<const char*>();
    return true;
}

bool TasmotaOutlet::readMac(std::string& out, uint32_t timeoutMs) {
    if (_ip[0] == '\0') return false;

    char url[64];
    snprintf(url, sizeof(url), "http://%s/cm?cmnd=Status%%205", _ip);

    const plughttp::Reply reply = plughttp::get(url, timeoutMs, timeoutMs);
    if (reply.code != 200) return false;

    // Status 5 is the network block: {"StatusNET":{"Hostname":..,"IPAddress":..,"Mac":".."}}.
    // A filter keeps the parse small — the full reply carries gateway, DNS, WiFi
    // power and more.
    StaticJsonDocument<64> filter;
    filter["StatusNET"]["Mac"] = true;
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, reply.body, DeserializationOption::Filter(filter))) return false;

    JsonVariant v = doc["StatusNET"]["Mac"];
    if (v.isNull()) return false;
    out = v.as<const char*>();
    return !out.empty();
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
        plughttp::log(std::string("[Outlets] Tasmota ") + _ip + " locked on \xE2\x80\x94 its Toggle button is now inert.");
    } else {
        // The claim landed and the lock did not. Say so rather than failing the
        // whole pairing: a claimed plug that can still be toggled is a working
        // sensor with a confusing web page, not a broken one.
        plughttp::log(std::string("[Outlets] Tasmota ") + _ip + " claimed, but PowerOnState/PowerLock did not take.");
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

bool TasmotaOutlet::readClaim(const char* /*ourHost*/, const char* /*deviceName*/, const char* ourName,
                              plugclaim::Claim& out, std::string* pushUrl) {
    std::string marker;
    if (!readOwner(marker)) return false;
    out = plugclaim::decideMarker(marker.c_str(), ourName ? ourName : "");
    if (pushUrl) *pushUrl = "";
    return true;
}

#endif  // CONTROL_SMART_OUTLET || DUSTGATE_NODE_PLUG_POLL || DUSTGATE_NATIVE

// =============================================================================
// node/BrainLink.h — a node dials its OWN primary (2026-10-04).
//
// WHY. The primary used to dial every node: one task and ~7-10 KB each, a cached
// address per node that goes stale, an mDNS lookup per node, and a link that fails
// outright when the path works in one direction only (2026-09-27: the primary's TCP
// to a node timed out while a laptop reached the same node in 10 ms). Here the NODE
// dials, so the primary's cost for a node is one idle socket and no task, and a node
// that cannot find its primary keeps looking by itself — nobody has to power-cycle it.
//
// PAIRING IS UNCHANGED. A node never dials a primary it is not owned by: no owner,
// no dialling. The primary still pairs by dialling the node once (HELLO claims it);
// this file only takes over the steady-state link after that. See
// docs/nodes-dial-the-brain-plan.md.
//
// HOW IT FINDS THE PRIMARY — every step is a fast path, none is required, and the last
// asks nothing of the network at all:
//
//   1. a WHERE from the primary (it knows this node is down and says where it is);
//   2. a UDP BROADCAST beacon the primary sends every few seconds (broadcast is not
//      multicast, and passes most guest networks);
//   3. the address it last linked on (kept in NVS, so it survives a power cut);
//   4. `<owner>.local` by mDNS;
//   5. a sweep of the local /24 for anything that accepts a NodeLink JOIN — slow,
//      jittered, and rare, but it needs nothing from the network but IP.
//
// CLAUDE.md: "Never require anything a network is allowed to block." Steps 1-4 are
// conveniences; step 5 is the floor.
//
// THREADING. Everything here runs on ONE task of its own: WebSocketsClient is not
// thread-safe, and connect() blocks, which must never stall a servo or the clamp.
// Frames received are handed to a callback ON THIS TASK; anything the node wants to
// SEND goes through send(), which queues it for this task to write.
// =============================================================================
#pragma once
#include <Arduino.h>
#include <stdarg.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include "../control/NodeLink.h"

namespace brainlink {

static const uint32_t kBeaconStaleMs  = 40000;   // a beacon older than this is not a lead
static const uint32_t kBackoffMinMs   = 1500;
static const uint32_t kBackoffMaxMs   = 20000;
static const uint32_t kProbeMs        = 400;     // is anything listening at this address?
static const uint32_t kConnectMs      = 4000;    // from begin() to an open, upgraded socket
static const uint32_t kSweepEveryMs   = 120000;  // the floor is slow on purpose
static const uint8_t  kRoundsBeforeSweep = 4;    // ...and only after the fast paths have failed this often
static const size_t   kQueueLen       = 6;
static const size_t   kFrameMax       = 512;

using FrameFn = void (*)(const uint8_t* data, size_t len);
using StateFn = void (*)(bool up);
using OwnerFn = String (*)();
using BusyFn  = bool (*)();

// WebSocketsClient with the one protected field made readable: a failed TCP connect
// fires no event, so the library's own record of it is the only way to see one.
class Client : public WebSocketsClient {
public:
    unsigned long lastFailMs() const { return _lastConnectionFail; }
};

struct State {
    Client         ws;
    WiFiUDP        udp;
    bool           udpOpen    = false;
    Preferences    prefs;
    char           nodeId[48] = "";
    FrameFn        onFrame    = nullptr;
    StateFn        onState    = nullptr;
    OwnerFn        owner      = nullptr;
    BusyFn         inboundUp  = nullptr;     // is a primary already linked to our own listener?
    SemaphoreHandle_t qMutex  = nullptr;
    char           q[kQueueLen][kFrameMax]   = {};
    size_t         qHead = 0, qCount = 0;
    volatile bool  up         = false;       // an upgraded socket to the primary is open
    IPAddress      remote;
    char           cachedIp[16]  = "";
    // leads
    char           hintIp[16]    = "";
    uint16_t       hintPort      = 80;
    char           beaconIp[16]  = "";
    uint16_t       beaconPort    = 80;
    uint32_t       beaconAtMs    = 0;
    uint32_t       backoffMs     = kBackoffMinMs;
    uint32_t       nextTryMs     = 0;
    uint8_t        rounds        = 0;
    uint32_t       lastSweepMs   = 0;
    uint32_t       jitter        = 0;
    uint32_t       joinedAtMs    = 0;
    // for the log
    uint32_t       attempts      = 0;
    TaskHandle_t   task          = nullptr;
    // The last few things this task did, newest last, for GET /api/brainlink — a node
    // has no screen and its serial is usually not attached, so "why has it not found
    // the primary?" has to be answerable over the network.
    static const size_t kNotes = 8;
    char           notes[kNotes][96] = {};
    uint8_t        noteN         = 0;
};
inline State& S() { static State s; return s; }

// Remember a line AND print it. printf-style; kept short.
inline void note(const char* fmt, ...) {
    State& s = S();
    char line[96];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    Serial.print(F("[BRAIN] ")); Serial.println(line);
    snprintf(s.notes[s.noteN % State::kNotes], sizeof(s.notes[0]), "%lus %s", (unsigned long)(millis() / 1000UL), line);
    s.noteN++;
}

// The state as JSON, for the node's own /api/brainlink.
inline String statusJson() {
    State& s = S();
    String o = "{";
    o += "\"owner\":\"" + (s.owner ? s.owner() : String()) + "\"";
    o += ",\"up\":" + String(s.up ? "true" : "false");
    o += ",\"cachedIp\":\"" + String(s.cachedIp) + "\"";
    o += ",\"beaconIp\":\"" + String(s.beaconIp) + "\"";
    o += ",\"beaconAgeS\":" + String(s.beaconAtMs ? (long)((millis() - s.beaconAtMs) / 1000UL) : -1L);
    o += ",\"attempts\":" + String((unsigned long)s.attempts);
    o += ",\"rounds\":" + String((unsigned)s.rounds);
    o += ",\"backoffMs\":" + String((unsigned long)s.backoffMs);
    o += ",\"nextTryInS\":" + String(s.nextTryMs > millis() ? (long)((s.nextTryMs - millis()) / 1000UL) : 0L);
    o += ",\"stackFree\":" + String(s.task ? (unsigned)uxTaskGetStackHighWaterMark(s.task) : 0u);
    o += ",\"uptimeS\":" + String((unsigned long)(millis() / 1000UL));
    o += ",\"freeHeap\":" + String((unsigned long)ESP.getFreeHeap());
    o += ",\"notes\":[";
    const size_t n = s.noteN < State::kNotes ? s.noteN : State::kNotes;
    for (size_t i = 0; i < n; i++) {
        const size_t idx = (s.noteN - n + i) % State::kNotes;
        String t = s.notes[idx];
        t.replace("\"", "'");
        o += (i ? ",\"" : "\"") + t + "\"";
    }
    o += "]}";
    return o;
}

inline bool connected() { return S().up; }
inline IPAddress remote() { return S().remote; }

// Queue one frame for the link task to write. Safe from any task. Drops the OLDEST
// when full: a stale SENSE is worth less than the newest one. False if not linked.
inline bool send(const char* text) {
    State& s = S();
    if (!s.up || !s.qMutex || !text) return false;
    const size_t n = strlen(text);
    if (n >= kFrameMax) return false;
    xSemaphoreTake(s.qMutex, portMAX_DELAY);
    if (s.qCount == kQueueLen) { s.qHead = (s.qHead + 1) % kQueueLen; s.qCount--; }
    memcpy(s.q[(s.qHead + s.qCount) % kQueueLen], text, n + 1);
    s.qCount++;
    xSemaphoreGive(s.qMutex);
    return true;
}

// The primary says where it is (a WHERE frame). Acted on at the top of the next
// attempt, and it clears any backoff — the primary asked, so go now.
inline void hint(const char* ip, uint16_t port) {
    State& s = S();
    strlcpy(s.hintIp, ip, sizeof(s.hintIp));
    s.hintPort = port;
    s.nextTryMs = 0;
    s.backoffMs = kBackoffMinMs;
}

namespace detail {

inline bool parseIp(const char* str, IPAddress& out) { return out.fromString(str); }

inline bool tcpProbe(const IPAddress& ip, uint16_t port, uint32_t ms) {
    WiFiClient c;
    const bool ok = c.connect(ip, port, ms);
    c.stop();
    return ok;
}

inline void closeUdp() {
    State& s = S();
    if (s.udpOpen) { s.udp.stop(); s.udpOpen = false; }
}

// Beacon: "DGB1|<primaryId>|<ip>|<port>". Anything else on the port is ignored.
inline void pollBeacon(const String& owner) {
    State& s = S();
    if (!s.udpOpen) { s.udpOpen = s.udp.begin(topo::nodelink::kBeaconPort); }
    if (!s.udpOpen) return;
    int n;
    while ((n = s.udp.parsePacket()) > 0) {
        char buf[96];
        const int got = s.udp.read((uint8_t*)buf, sizeof(buf) - 1);
        if (got <= 0) continue;
        buf[got] = 0;
        if (strncmp(buf, "DGB1|", 5) != 0) continue;
        char* id = buf + 5;
        char* ip = strchr(id, '|');   if (!ip) continue;  *ip++ = 0;
        char* pt = strchr(ip, '|');   if (!pt) continue;  *pt++ = 0;
        if (!owner.equalsIgnoreCase(id)) continue;
        strlcpy(s.beaconIp, ip, sizeof(s.beaconIp));
        s.beaconPort = (uint16_t)atoi(pt);
        if (!s.beaconPort) s.beaconPort = 80;
        s.beaconAtMs = millis() ? millis() : 1;
    }
}

// One attempt at one address: probe, dial, wait for the upgrade, send JOIN. The
// handshake that follows (HELLO / WELCOME) is the node's own handler's business.
inline bool tryAddress(const char* ipStr, uint16_t port, const char* how) {
    State& s = S();
    IPAddress ip;
    if (!parseIp(ipStr, ip)) return false;
    if (!tcpProbe(ip, port, kProbeMs)) { note("%s:%u (%s): nothing listening", ipStr, (unsigned)port, how); return false; }
    s.attempts++;
    note("dialling %s:%u (%s)", ipStr, (unsigned)port, how);
    s.ws.disconnect();
    s.ws.begin(ip, port, "/nodelink");
    const uint32_t t0 = millis();
    while (!s.ws.isConnected() && millis() - t0 < kConnectMs) {
        s.ws.loop();
        delay(5);
    }
    if (!s.ws.isConnected()) {
        s.ws.disconnect();
        note("%s:%u did not upgrade within %lu ms", ipStr, (unsigned)port, (unsigned long)kConnectMs);
        return false;
    }
    s.remote = ip;
    s.up = true;
    s.joinedAtMs = millis();
    return true;
}

// Sweep the local /24 for anything that takes a NodeLink socket. The floor under
// every other step: it needs nothing from the network but IP. Slow, so it runs on
// this task only, rarely, and stops at the first hit.
inline bool sweep(const String& owner) {
    State& s = S();
    (void)owner;
    const IPAddress me = WiFi.localIP();
    const uint32_t base = (uint32_t)me & 0x00FFFFFFu;     // little-endian: a.b.c in the low 3 bytes
    Serial.println(F("[BRAIN] sweeping the subnet for the primary"));
    for (int h = 1; h < 255; h++) {
        if ((uint8_t)(((uint32_t)me >> 24) & 0xFF) == h) continue;   // not ourselves
        IPAddress ip((uint32_t)(base | ((uint32_t)h << 24)));
        if (!tcpProbe(ip, 80, 120)) { delay(1); continue; }
        char str[16]; snprintf(str, sizeof(str), "%u.%u.%u.%d", ip[0], ip[1], ip[2], h);
        if (tryAddress(str, 80, "sweep")) return true;
    }
    s.lastSweepMs = millis();
    return false;
}

} // namespace detail

inline void begin(const char* nodeId, FrameFn onFrame, StateFn onState, OwnerFn owner, BusyFn inboundUp);

inline void taskFn(void*) {
    State& s = S();
    // A stable per-board spread so nine nodes do not redial the same second after the
    // primary reboots. The MAC differs between boards and is the same across reboots.
    uint8_t mac[6]; WiFi.macAddress(mac);
    s.jitter = ((uint32_t)mac[4] << 8) | mac[5];

    for (;;) {
        const String owner = s.owner ? s.owner() : String();

        // Unclaimed: never dial. Pairing is the primary's move; a node that dialled
        // whoever it found would be adopted by the first stranger on the network.
        if (!owner.length()) {
            static bool said = false;
            if (!said) { said = true; note("unclaimed - not dialling anyone"); }
            detail::closeUdp(); delay(500); continue;
        }

        // The primary has dialled US and is linked: that link is the one link.
        if (s.inboundUp && s.inboundUp() && !s.up) { detail::closeUdp(); delay(250); continue; }

        if (s.up) {
            detail::closeUdp();
            s.ws.loop();
            // Drain what the node wants to say. Here and nowhere else touches the
            // socket for writing.
            for (;;) {
                char f[kFrameMax];
                xSemaphoreTake(s.qMutex, portMAX_DELAY);
                if (!s.qCount) { xSemaphoreGive(s.qMutex); break; }
                memcpy(f, s.q[s.qHead], kFrameMax);
                s.qHead = (s.qHead + 1) % kQueueLen; s.qCount--;
                xSemaphoreGive(s.qMutex);
                s.ws.sendTXT(f);
            }
            if (!s.ws.isConnected()) {
                s.ws.disconnect();   // stop the library redialling the same address on its own
                s.up = false;
                if (s.onState) s.onState(false);
                note("link to the primary lost after %lu s", (unsigned long)((millis() - s.joinedAtMs) / 1000UL));
                s.nextTryMs = millis() + 600 + (s.jitter % 900);   // a short drop should heal fast
                s.backoffMs = kBackoffMinMs;
                s.rounds = 0;
            }
            delay(5);
            continue;
        }

        // ── looking for the primary ────────────────────────────────────────────
        detail::pollBeacon(owner);
        if (millis() < s.nextTryMs) { delay(20); continue; }

        bool linked = false;
        // 1. WHERE.
        if (s.hintIp[0]) {
            char ip[16]; strlcpy(ip, s.hintIp, sizeof(ip)); const uint16_t port = s.hintPort;
            s.hintIp[0] = 0;
            linked = detail::tryAddress(ip, port, "told where");
        }
        // 2. the beacon.
        if (!linked && s.beaconIp[0] && s.beaconAtMs && millis() - s.beaconAtMs < kBeaconStaleMs) {
            linked = detail::tryAddress(s.beaconIp, s.beaconPort, "beacon");
        }
        // 3. the address it last linked on.
        if (!linked && s.cachedIp[0] && strcmp(s.cachedIp, s.beaconIp) != 0) {
            linked = detail::tryAddress(s.cachedIp, 80, "last address");
        }
        // 4. the name.
        if (!linked) {
            IPAddress ip = MDNS.queryHost(owner.c_str(), 900);
            if (!(uint32_t)ip) note("mDNS: %s.local did not answer", owner.c_str());
            if ((uint32_t)ip) {
                char str[16]; snprintf(str, sizeof(str), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
                if (strcmp(str, s.cachedIp) != 0 && strcmp(str, s.beaconIp) != 0)
                    linked = detail::tryAddress(str, 80, "mDNS");
            }
        }
        // 5. the floor.
        if (!linked && s.rounds >= kRoundsBeforeSweep &&
            (!s.lastSweepMs || millis() - s.lastSweepMs > kSweepEveryMs)) {
            linked = detail::sweep(owner);
        }

        if (linked) {
            note("linked at %u.%u.%u.%u", s.remote[0], s.remote[1], s.remote[2], s.remote[3]);
            detail::closeUdp();
            s.backoffMs = kBackoffMinMs;
            s.rounds = 0;
            // The address that WORKED is the one worth keeping.
            char str[16]; snprintf(str, sizeof(str), "%u.%u.%u.%u", s.remote[0], s.remote[1], s.remote[2], s.remote[3]);
            if (strcmp(str, s.cachedIp) != 0) {
                strlcpy(s.cachedIp, str, sizeof(s.cachedIp));
                s.prefs.putString("ip", s.cachedIp);
            }
            if (s.onState) s.onState(true);
            continue;
        }

        s.rounds = s.rounds < 250 ? s.rounds + 1 : s.rounds;
        // Jittered, doubling, capped: a primary that is simply off for an hour is
        // asked four times a minute at most, and nine nodes do not queue up together.
        s.nextTryMs = millis() + s.backoffMs + (s.jitter % (s.backoffMs / 4 + 1));
        if (s.backoffMs < kBackoffMaxMs) s.backoffMs = s.backoffMs * 2 > kBackoffMaxMs ? kBackoffMaxMs : s.backoffMs * 2;
        note("no primary yet (round %u) - next try in %lu s", (unsigned)s.rounds,
             (unsigned long)((s.nextTryMs - millis()) / 1000UL));
    }
}

inline void begin(const char* nodeId, FrameFn onFrame, StateFn onState, OwnerFn owner, BusyFn inboundUp) {
    State& s = S();
    strlcpy(s.nodeId, nodeId, sizeof(s.nodeId));
    s.onFrame = onFrame; s.onState = onState; s.owner = owner; s.inboundUp = inboundUp;
    s.qMutex = xSemaphoreCreateMutex();
    s.prefs.begin("brainlink", false);
    strlcpy(s.cachedIp, s.prefs.getString("ip", "").c_str(), sizeof(s.cachedIp));

    s.ws.onEvent([](WStype_t type, uint8_t* payload, size_t len) {
        State& st = S();
        if (type == WStype_CONNECTED) {
            StaticJsonDocument<128> d;
            topo::nodelink::buildJoin(d.to<JsonObject>(), st.nodeId);
            String j; serializeJson(d, j);
            st.ws.sendTXT(j);
        } else if (type == WStype_TEXT) {
            if (st.onFrame) st.onFrame(payload, len);
        }
    });
    // OURS to retry — but NOT parked with a huge interval, which is what this said
    // first and what stopped the node ever connecting: loop() refuses to dial until
    // `millis() - _lastConnectionFail >= _reconnectInterval`, begin() zeroes that
    // timestamp, and so an interval of an hour meant "no first connection until the
    // board has been up an hour" (found on the first hardware run, 2026-10-04).
    // 1 ms makes the first dial immediate. The library would then redial the same
    // address after a drop, so taskFn() calls disconnect() the moment it sees one.
    s.ws.setReconnectInterval(1);
    // The node watches the primary, as the primary has always watched its nodes: two
    // missed pongs and the socket is declared dead, which hands control back to the
    // seeking above. (The primary's AsyncWebSocket answers a ping on its own.)
    s.ws.enableHeartbeat(topo::nodelink::kPingIntervalMs, topo::nodelink::kPongTimeoutMs, 2);

    // 10 KB: the frame handler runs on this task and a CONFIG with four sensors left only
    // 2.4 KB free of 8 KB on the first hardware run (2026-10-04).
    xTaskCreate(taskFn, "brainlink", 10240, nullptr, 1, &s.task);
    note("started; owner '%s', cached address '%s'", owner ? owner().c_str() : "", s.cachedIp);
}

} // namespace brainlink

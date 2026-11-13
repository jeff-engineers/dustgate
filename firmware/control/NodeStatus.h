// =============================================================================
// control/NodeStatus.h — one node's entry in GET /api/nodes, built the same way on every platform.
//
// It lived inline in firmware.ino, as the body of the loop that publishes /api/nodes. A native brain
// needs the identical document (the Boards screen reads it), and the way to keep two shells from
// drifting is for neither to own the shape. PURE — ArduinoJson only; templated on the session-like
// thing it reads (NodeSession, or RemoteActuatorBus which wraps one) so it needs neither.
//
// Omitted-when-absent is the rule throughout, as on the wire: `ct` only when the board has a clamp,
// `ota*` only while an update is being reported, `claimedBy` only for a node somebody else owns.
// =============================================================================
#pragma once
#include <cstring>
#include <ArduinoJson.h>
#include "SenseReport.h"

namespace topo {

// Is the staged image worth offering to a node running `have`? Different is not enough: a node
// flashed from the bench after the staged image was built would be offered a DOWNGRADE. The fw
// stamp ends in "MMDD-HHMM" (BuildStamp::fw), which sorts as text within a year, so a node whose
// stamp is newer than the image's is left alone. A stamp that does not parse falls back to "differs".
inline bool updateDue(const char* have, const char* image) {
    if (std::strcmp(have, image) == 0) return false;
    auto stamp = [](const char* f) -> const char* {
        const char* sp = std::strrchr(f, ' ');
        const char* t = sp ? sp + 1 : f;
        return (std::strlen(t) == 9 && t[4] == '-') ? t : nullptr;
    };
    const char* a = stamp(have);
    const char* b = stamp(image);
    if (a && b) return std::strcmp(a, b) < 0;
    return true;
}

// The firmware image this brain would install on a node of this kind, if it holds one.
struct NodeImageView {
    bool        present = false;
    const char* fw      = "";
};

template <typename S>
inline void writeNodeEntry(JsonArray arr, const S& s, const char* id, const char* host,
                           const char* name, const NodeImageView& img) {
    // NOT const: ArduinoJson v6 COPIES a char[] but stores a pointer to a `const char[]`, and `n` dies
    // with this call — a const copy here put garbage in `board` and `fw` (found on the native shell).
    auto n = s.info();
    JsonObject o = arr.createNestedObject();
    o["id"]       = id;
    o["host"]     = host;
    // From the pairing registry, not the layout: the boards screen renders names with none loaded.
    o["name"]     = name;
    o["online"]   = n.connected;
    o["lastSeen"] = n.lastSeenMs;
    o["board"]    = n.board;
    o["fw"]       = n.fw;
    // ONLY ONCE THE BOARD HAS SAID (2026-10-17): before its first WELCOME since this brain started, every cap reads 0,
    // and `bus: 0` is a claim — the app read it as "a servo board" and stripped `drives: bus` off a slider that
    // was merely switched off, then refused its own save ("dustgate-slider is a servo board"). Absent caps are unknown,
    // and the app keeps what the layout already says. `board` is what the WELCOME fills in.
    if (n.board[0]) {
        JsonObject caps = o.createNestedObject("caps");
        caps["servos"] = n.capServos;
        caps["bus"] = n.capBus;
        // Omitted when none, matching the wire: absent already means "no clamp".
        if (n.capClamps > 0) caps["ct"] = n.capClamps;
        // The collector's other two jobs, so the app offers a board only for what it can do (as on the wire: absent = no).
        if (n.capRf)  caps["rf"]  = 1;
        if (n.capBin) caps["bin"] = 1;
    }
    // Is a clamp in the jack: present only once the board has said (CLAMP, 2026-10-09). Absent = not known, which the
    // app must not draw as unplugged.
    if (n.capClamps > 0 && n.clampIn >= 0) o["clampIn"] = n.clampIn == 1;
    addSenseArray(o, s);
    // OTA: which image this brain would install, whether the node already has it, and how an update
    // in progress is going. `update` is only ever true for a board that is up to be told.
    if (img.present) {
        o["image"]  = img.fw;
        o["update"] = n.connected && updateDue(n.fw, img.fw);
    }
    if (n.ota[0]) {
        o["ota"] = n.ota;
        if (n.otaPct >= 0) o["otaPct"] = n.otaPct;
        if (n.otaErr[0])   o["otaErr"] = n.otaErr;
    }
    // A node that belongs to ANOTHER primary is offline to us on purpose. Naming its owner is what
    // tells a dead board from a claimed one, and decides whether you press "take it over".
    if (s.wasRefused()) {
        o["claimedBy"] = s.refusedBy();
        o["takeable"]  = true;
    }
}

// The bus servos and the link, for a session that keeps them (NodeSession; the frozen ESP32 brain's bus does not).
// `bus`: each servo as last reported plus the peak of its last moves (`history`, oldest first) — the load bars and the
// line on a servo's page. `link`: the board's signal and its own count of drops, and the brain's count of link checks
// that went unanswered — developer mode's communication losses. Omitted until there is something to say.
template <typename S>
inline void addBusAndLink(JsonObject o, const S& s) {
    if (s.busCount()) {
        JsonArray a = o.createNestedArray("bus");
        for (size_t i = 0; i < s.busCount(); i++) {
            const auto& b = s.busServo(i);
            JsonObject e = a.createNestedObject();
            e["id"] = b.r.id; e["ok"] = b.r.ok;
            if (b.r.ok) { e["deg"] = b.r.deg; e["load"] = b.r.load; e["v"] = b.r.v; e["temp"] = b.r.temp; }
            e["peak"] = b.r.peak; e["missed"] = b.r.missed; e["reads"] = b.r.reads; e["moves"] = b.r.moves;
            JsonArray h = e.createNestedArray("history");
            for (size_t k = 0; k < b.nHistory; k++) h.add(b.history[k]);
        }
    }
    const auto l = s.link();
    JsonObject k = o.createNestedObject("link");
    if (l.rssi) k["rssi"] = l.rssi;
    if (l.nodeDrops >= 0) k["drops"] = l.nodeDrops;
    k["lost"] = l.pingsLost;
}

}  // namespace topo

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
    JsonObject caps = o.createNestedObject("caps");
    caps["servos"] = n.capServos;
    caps["linear"] = n.capLinear;
    // Omitted when none, matching the wire: absent already means "no clamp".
    if (n.capClamps > 0) caps["ct"] = n.capClamps;
    // The collector's other two jobs, so the app offers a board only for what it can do (as on the wire: absent = no).
    if (n.capRf)  caps["rf"]  = 1;
    if (n.capBin) caps["bin"] = 1;
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

}  // namespace topo

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
    addSenseArray(o, s);
    // OTA: which image this brain would install, whether the node already has it, and how an update
    // in progress is going. `update` is only ever true for a board that is up to be told.
    if (img.present) {
        o["image"]  = img.fw;
        o["update"] = n.connected && std::strcmp(n.fw, img.fw) != 0;
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

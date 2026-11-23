// =============================================================================
// control/LayoutGate.h — the structural check every brain applies to a layout before it will hold it.
//
// ONLY WHAT IS NOT A SHOP AT ALL IS REFUSED (jeff, 2026-11-23, D-88): a save is never blocked by a problem the shop can run
// around, so a person never loses an evening's edits to one stale id. Everything else the spec finds wrong is stored as
// given; the app names the tools, gates, plugs, clamps and boards it affects, and the runtime leaves alone what it cannot drive safely
// (sharedChannelGates below). The JS twin is layoutGate() in shared/device-model/shop.js (layout-gate.test.js ↔
// test_layoutgate.cpp, same cases, same order) — the mock and the demo refuse exactly what the brains do.
//
// NOT THE SCHEMA. The authoritative validator is shared/device-model/topology.js + shop.js, which the app runs to say what
// a saved layout's problems affect; this is the line under which a document is not a shop at all, so a brain never keeps
// one its runtime cannot route. It used to live in TopologyStore.h, the ESP32's LittleFS store, so the native brain never ran it
// and kept a layout with no systems at all that the ESP32 answered 400 to (found 2026-10-18 by running the topology
// contract suite against the native brain). Both brains now call it from TopologyRuntime::adopt().
//
// Pure — ArduinoJson and STL — so the host tests drive it (test_layoutgate.cpp).
// =============================================================================
#pragma once
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <ArduinoJson.h>

namespace topo {

inline bool layoutGate(JsonObjectConst t, std::string& err) {
    if (!t["controllers"].is<JsonArrayConst>()) { err = "controllers not an array"; return false; }
    // Exactly one primary: one brain.
    int primaries = 0;
    for (JsonObjectConst c : t["controllers"].as<JsonArrayConst>()) {
        const char* role = c["role"];
        if (role && std::strcmp(role, "primary") == 0) primaries++;
    }
    if (primaries != 1) { err = "exactly one primary controller required"; return false; }
    if (!t["systems"].is<JsonArrayConst>()) { err = "systems not an array"; return false; }
    JsonArrayConst systems = t["systems"].as<JsonArrayConst>();
    if (systems.size() == 0) { err = "a shop needs at least one system"; return false; }
    if (!t["machines"].is<JsonArrayConst>()) { err = "machines not an array"; return false; }
    for (JsonObjectConst sys : systems) {
        const char* sid = sys["id"];
        if (!sid || !*sid) { err = "system missing id"; return false; }
        if (!sys["elements"].is<JsonArrayConst>()) { err = std::string("system \"") + sid + "\": elements not an array"; return false; }
        // One collector PER SYSTEM — the invariant the container exists to preserve.
        int n = 0;
        for (JsonObjectConst e : sys["elements"].as<JsonArrayConst>()) {
            const char* type = e["type"];
            if (type && std::strcmp(type, "collector") == 0) n++;
        }
        if (n != 1) { err = std::string("system \"") + sid + "\": exactly one collector required"; return false; }
    }
    return true;
}

// Gates that share a PWM channel on one board with another gate. Two servos on one pad move together, so opening one drags
// the other: the runtime never moves any of them and raises gate-shares-channel for each (TopologyRuntime.h). Stored, not
// refused (D-88): the rest of the shop still runs. Matched by sharedChannelGates() in shop.js.
inline std::set<std::string> sharedChannelGates(JsonObjectConst t) {
    std::map<std::string, std::vector<std::string>> byPad;
    for (JsonObjectConst sys : t["systems"].as<JsonArrayConst>())
        for (JsonObjectConst e : sys["elements"].as<JsonArrayConst>()) {
            const char* type = e["type"];
            const char* cid = e["controllerId"];
            const char* id = e["id"];
            if (!type || std::strcmp(type, "selector") != 0 || !cid || !id || !e["servo"]["channel"].is<int>()) continue;
            byPad[std::string(cid) + '\0' + std::to_string(e["servo"]["channel"].as<int>())].push_back(id);
        }
    std::set<std::string> out;
    for (auto& kv : byPad) if (kv.second.size() > 1) out.insert(kv.second.begin(), kv.second.end());
    return out;
}

}  // namespace topo

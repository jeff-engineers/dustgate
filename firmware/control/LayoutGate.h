// =============================================================================
// control/LayoutGate.h — the structural check every brain applies to a layout before it will hold it.
//
// NOT THE SCHEMA. The authoritative validator is shared/device-model/topology.js + shop.js, which the app runs before it
// ever saves; this is the cheap last line of defence against garbage and truncation, so a brain never keeps a document its
// runtime cannot route. It used to live in TopologyStore.h, the ESP32's LittleFS store, so the native brain never ran it
// and kept a layout with no systems at all that the ESP32 answered 400 to (found 2026-10-18 by running the topology
// contract suite against the native brain). Both brains now call it from TopologyRuntime::adopt().
//
// Pure — ArduinoJson and STL — so the host tests drive it (test_layoutgate.cpp).
// =============================================================================
#pragma once
#include <cstring>
#include <set>
#include <string>
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
    // Two servo gates on one board sharing a PWM channel move together: one gate opening drags the other with it. The
    // spec's rule (topology.js), in its words, here because a brain must not route a layout that does that whatever saved it.
    std::set<std::string> held;
    for (JsonObjectConst sys : systems)
        for (JsonObjectConst e : sys["elements"].as<JsonArrayConst>()) {
            const char* type = e["type"];
            const char* cid = e["controllerId"];
            if (!type || std::strcmp(type, "selector") != 0 || !cid || !e["servo"]["channel"].is<int>()) continue;
            const int ch = e["servo"]["channel"].as<int>();
            if (!held.insert(std::string(cid) + '\0' + std::to_string(ch)).second) {
                err = "servo channel " + std::to_string(ch) + " on host \"" + cid + "\" is already used by another gate";
                return false;
            }
        }
    return true;
}

}  // namespace topo

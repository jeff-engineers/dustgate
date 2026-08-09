// =============================================================================
// NodeBus.h — routes each selector to the ActuatorBus that actually drives it.
//
// This is the ONLY place in the firmware that knows a gate might live on another
// board. Every selector carries a `controllerId` (docs/v2-topology-schema.md);
// NodeBus maps that id to a bus:
//
//   controllerId absent, or == this board's own id  → the local bus
//   controllerId of a registered secondary          → that node's remote bus
//   controllerId of an UNregistered controller      → no bus (offline)
//
// Stage 1 registers no remotes, so everything lands on the local bus and the
// dispatch is a no-op — which is exactly the point: the multi-node path is
// exercised by the same code the single-board build runs every day.
//
// busy() is deliberately GLOBAL, not per-bus: the one-servo-at-a-time current
// budget is a per-board rail concern, but serializing across boards too costs
// nothing (moves are ~2s, tool changes are minutes apart) and keeps the
// transition order the sequencer computed strictly observable.
//
// PURE — ArduinoJson + STL only, NO Arduino.h.
// =============================================================================

#pragma once
#include <ArduinoJson.h>
#include "ActuatorBus.h"
#include <cctype>
#include <map>
#include <string>
#include <vector>

namespace topo {

// ONE canonical spelling for a board's address.
//
// The same node legitimately appears as "dustgate-node-1" (what you paired, what
// mDNS advertises) and "dustgate-node-1.local" (what you must actually dial, and
// so what /api/v2/nodes reports and the UI writes back into link.host). Comparing
// those with == silently produced a shop where the link was up and green while
// every gate on it was un-commandable: the remotes map was keyed on one spelling
// and the topology's alias pointed at the other.
//
// Case-insensitive too, since mDNS names are.
inline std::string bareHost(const char* h) {
    if (!h) return std::string();
    std::string s(h);
    // Trailing dot first: a fully-qualified mDNS name is "host.local."
    if (!s.empty() && s.back() == '.') s.pop_back();
    const std::string suffix = ".local";
    if (s.size() > suffix.size()) {
        std::string tail = s.substr(s.size() - suffix.size());
        for (char& c : tail) c = (char)tolower((unsigned char)c);
        if (tail == suffix) s.erase(s.size() - suffix.size());
    }
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

class NodeBus {
public:
    // The local board's own actuators, and the controller id this board answers
    // to. A selector with no controllerId is assumed local (single-board shops
    // and every pre-multi-node topology).
    void setLocal(ActuatorBus* bus, const char* ownControllerId) {
        _local = bus;
        _ownId = ownControllerId ? ownControllerId : "";
    }

    // Register (or replace) the bus for a paired node. The KEY IS THE HOST, not a
    // controllerId: a link's lifetime belongs to pairing (NodeRegistry.h), which
    // outlives any particular layout, while controllerId is a name the UI chose
    // and can rename tomorrow.
    void registerRemote(const std::string& host, ActuatorBus* bus) {
        _remotes[bareHost(host.c_str())] = bus;
    }
    void clearRemotes() { _remotes.clear(); _aliases.clear(); }

    // Point a topology controllerId at a paired host. Set on topology adopt and
    // nowhere else — this is the ONLY thing a layout contributes to routing a
    // move off-board, and re-adopting one can no longer tear a live link down.
    void setAlias(const std::string& controllerId, const std::string& host) {
        _aliases[bareHost(controllerId.c_str())] = bareHost(host.c_str());
    }
    void clearAliases() { _aliases.clear(); }

    // The bus that drives this selector, or nullptr if its controller isn't
    // reachable from here (named a board that was never paired).
    ActuatorBus* busFor(JsonObjectConst sel) const {
        const char* cid = sel["controllerId"].as<const char*>();
        if (!cid || !*cid) return _local;
        const std::string id = bareHost(cid);
        if (bareHost(_ownId.c_str()) == id) return _local;
        // controllerId → host, then host → link. A controllerId that IS a host
        // (the natural case when the picker writes what discovery found) resolves
        // without an alias, so a topology saved before aliases existed still works.
        auto a = _aliases.find(id);
        const std::string key = (a == _aliases.end()) ? id : a->second;
        auto it = _remotes.find(key);
        return it == _remotes.end() ? nullptr : it->second;
    }

    bool setState(const char* selectorId, JsonObjectConst sel, const char* stateId) {
        ActuatorBus* b = busFor(sel);
        if (!b || !b->online()) return false;
        return b->setState(selectorId, sel, stateId);
    }

    // True if ANY bus has a move in flight — the global current mutex.
    bool busy() const {
        if (_local && _local->busy()) return true;
        for (auto& kv : _remotes) if (kv.second && kv.second->busy()) return true;
        return false;
    }

    // Can this selector be commanded at all right now?
    bool onlineFor(JsonObjectConst sel) const {
        ActuatorBus* b = busFor(sel);
        return b && b->online();
    }

    void update() {
        if (_local) _local->update();
        for (auto& kv : _remotes) if (kv.second) kv.second->update();
    }

    const std::string& ownControllerId() const { return _ownId; }

private:
    ActuatorBus*                        _local = nullptr;
    std::string                         _ownId;
    std::map<std::string, ActuatorBus*> _remotes;   // host → link
    std::map<std::string, std::string>  _aliases;   // controllerId → host
};

} // namespace topo

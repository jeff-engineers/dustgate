// =============================================================================
// NodeBus.h — routes each selector to the ActuatorBus that actually drives it.
//
// This is the ONLY place in the firmware that knows a gate might live on another
// board. Every selector carries a `controllerId` (docs/topology-schema.md);
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
#include "BoardId.h"       // bareHost, isOwnBoard: the one rule for "which board is this id"
#include <cctype>
#include <map>
#include <string>
#include <vector>

namespace topo {

class NodeBus {
public:
    // The local board's own actuators, and the controller id this board answers
    // to. A selector with no controllerId is assumed local (single-board shops
    // and every pre-multi-node topology).
    void setLocal(ActuatorBus* bus, const char* ownControllerId) {
        _local = bus;
        _ownId = ownControllerId ? ownControllerId : "";
        if (_local && _servoMinUs) _local->setServoPulseRange(_servoMinUs, _servoMaxUs);
    }

    // The shop's servo pulse range, from the layout (TopologyRuntime::adopt). Kept, so a board paired later gets it too.
    void setServoPulseRange(int minUs, int maxUs) {
        _servoMinUs = minUs; _servoMaxUs = maxUs;
        if (_local) _local->setServoPulseRange(minUs, maxUs);
        for (auto& kv : _remotes) if (kv.second) kv.second->setServoPulseRange(minUs, maxUs);
    }

    // Register (or replace) the bus for a paired node. The KEY IS THE HOST, not a
    // controllerId: a link's lifetime belongs to pairing (NodeRegistry.h), which
    // outlives any particular layout, while controllerId is a name the UI chose
    // and can rename tomorrow.
    void registerRemote(const std::string& host, ActuatorBus* bus) {
        _remotes[bareHost(host.c_str())] = bus;
        if (bus && _servoMinUs) bus->setServoPulseRange(_servoMinUs, _servoMaxUs);
    }
    void clearRemotes() { _remotes.clear(); _aliases.clear(); }

    // Point a topology controllerId at a paired host. Set on topology adopt and
    // nowhere else — this is the ONLY thing a layout contributes to routing a
    // move off-board, and re-adopting one can no longer tear a live link down.
    void setAlias(const std::string& controllerId, const std::string& host) {
        _aliases[bareHost(controllerId.c_str())] = bareHost(host.c_str());
    }
    void clearAliases() { _aliases.clear(); }

    // The bus for a controllerId, or nullptr if that controller isn't reachable
    // from here (a board that was never paired).
    //
    // Split out of busFor() when sensors arrived: a CT is addressed by
    // controllerId with no selector anywhere near it, and the alias/host
    // resolution below is exactly the same problem. Two copies of that lookup is
    // how a sensor ends up working on a board whose gates do not, or vice versa.
    ActuatorBus* busForController(const char* cid) const {
        if (isOwnBoard(cid ? cid : "", _ownId)) return _local;
        const std::string id = bareHost(cid);
        // controllerId → host, then host → link. A controllerId that IS a host
        // (the natural case when the picker writes what discovery found) resolves
        // without an alias, so a topology saved before aliases existed still works.
        auto a = _aliases.find(id);
        const std::string key = (a == _aliases.end()) ? id : a->second;
        auto it = _remotes.find(key);
        return it == _remotes.end() ? nullptr : it->second;
    }

    // The bus that drives this selector.
    ActuatorBus* busFor(JsonObjectConst sel) const {
        return busForController(sel["controllerId"].as<const char*>());
    }

    // Push a board the whole list of sensors the layout says it carries.
    // Silently does nothing for a controller that is not reachable — the same
    // treatment an un-driveable gate gets, and for the same reason: a layout may
    // legitimately name a board that is switched off at the wall (RFC §5.6a).
    void configureSensors(const char* controllerId, JsonArrayConst sensors) {
        ActuatorBus* b = busForController(controllerId);
        if (b) b->configureSensors(sensors);
    }

    // Latest reading for one sensor. False = nothing has ever reported, which
    // includes "that board is not reachable" and must NOT be read as "off" by
    // anything that cares about the difference.
    bool senseOf(const char* controllerId, const char* sensorId,
                 bool& on, uint32_t& atMs) const {
        ActuatorBus* b = busForController(controllerId);
        return b && b->senseOf(sensorId, on, atMs);
    }

    // Plugs polled by a node (see ActuatorBus::pollsPlugs / plugReading).
    bool pollsPlugs(const char* controllerId) const {
        ActuatorBus* b = busForController(controllerId);
        return b && b->pollsPlugs();
    }
    bool plugReading(const char* controllerId, const char* sensorId,
                     float& watts, bool& fault, uint32_t& atMs) const {
        ActuatorBus* b = busForController(controllerId);
        return b && b->plugReading(sensorId, watts, fault, atMs);
    }

    // The collector's jobs on a node — see ActuatorBus::canPressRf / pressRf.
    bool canPressRf(const char* controllerId) const {
        ActuatorBus* b = busForController(controllerId);
        return b && b->online() && b->canPressRf();
    }
    bool watchesBin(const char* controllerId) const {
        ActuatorBus* b = busForController(controllerId);
        return b && b->watchesBin();
    }
    bool pressRf(const char* controllerId, uint8_t address, uint8_t data,
                 uint32_t tickUs, uint32_t repeats) {
        ActuatorBus* b = busForController(controllerId);
        return b && b->online() && b->canPressRf() && b->pressRf(address, data, tickUs, repeats);
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
    int                                 _servoMinUs = 0, _servoMaxUs = 0;   // the layout's servo pulse range; 0 = not said yet
    std::map<std::string, std::string>  _aliases;   // controllerId → host
};

} // namespace topo

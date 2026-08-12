// =============================================================================
// TopologyController.h — control brain (C++ port of topology-device.js + shop.js).
//
// The decision core that ties routing + sequencing to live machine power. It
// owns no hardware: it consumes power readings and emits an ordered
// make-before-break move plan per system. A thin device layer (TopologyRuntime)
// drives the actuator bank from those plans — kept separate so this brain stays
// host-testable.
//
//   setMachinePower(machineId, watts)  → track the OFF→ON edge (recency for
//                                        most-recent-wins) and reconcile()
//   reconcile()                        → routeShop(active) + planShopTransition(),
//                                        adopt each ACTIVE system's routed states,
//                                        HOLD the idle ones
//   machineForOutlet(host, ip)         → map a Shelly plug identity back to its
//                                        machine, so SmartOutletControl's per-plug
//                                        power can be fed in by machineId
//
// WHAT CHANGED FOR N SYSTEMS: everything that used to be one answer is now one
// answer PER SYSTEM — the collector, the idle-hold decision, the dead-head
// verdict. Actuator states stay a single flat map because selector ids are
// unique shop-wide, which is exactly why validateShop enforces that.
//
// A machine is what you switch on; a `tool` element is now a PORT. For a
// schemaVersion-1 document the two coincide (Shop.h::machineIdOf), so the
// existing conformance vectors still hold value-for-value against
// topology-device.js.
//
// PURE — ArduinoJson + STL, NO Arduino.h.
// =============================================================================

#pragma once
#include <ArduinoJson.h>
#include "Shop.h"
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace topo {

static const float kDefaultThresholdW = 5.0f;   // DEFAULT_THRESHOLD_W in topology-device.js

struct ReconcileResult {
  ShopRouting             routing;   // per-selector desired state + per-machine verdict
  std::vector<SystemPlan> plans;     // ordered make-before-break moves, per system
  // systemId → does any active machine have an enabled port in this system.
  // The idle-HOLD policy is applied per system, so the runtime needs to know
  // which blowers are being asked for and which are simply at rest.
  std::map<std::string, bool> systemActive;

  // The plan for one system. planShopTransition omits systems with nothing to
  // do, so "no entry" and "no moves, no risk" are the same answer and this
  // returns an empty plan for both.
  // Built field-by-field rather than brace-initialized: TransitionPlan carries a
  // default member initializer, which at the ESP32 toolchain's gnu++11 makes it
  // a non-aggregate with no such constructor. (The host tests build at C++17 and
  // would not have caught this.)
  TransitionPlan planFor(const std::string& systemId) const {
    TransitionPlan out;
    for (const SystemPlan& p : plans)
      if (p.systemId == systemId) { out.moves = p.moves; out.deadHeadRisk = p.deadHeadRisk; break; }
    return out;
  }
};

class Controller {
public:
  // Adopt a parsed document (v1 topology or v2 shop). Seeds every selector's
  // current state to its closed (idle) state, matching createTopologyDevice().
  // Clears power history.
  void setTopology(JsonObjectConst doc) {
    _doc = doc;
    _watts.clear();
    _activationSeq.clear();
    _seqCounter = 0;
    _collectorOn.clear();
    _actuatorStates.clear();
    for (const SystemView& sys : systemsOf(_doc)) {
      _collectorOn[std::string(sys.id ? sys.id : "")] = false;
      for (JsonObjectConst sel : sys.elements) {
        if (!_eq(sel["type"], "selector")) continue;
        std::string id = sel["id"].as<const char*>();
        const char* cs = _closedState(sel);
        _actuatorStates[id] = cs ? std::string(cs) : std::string();
      }
    }
  }

  // sensor.outlet.thresholdW for a machine, or the default.
  float machineThreshold(const std::string& machineId) const {
    JsonObjectConst m = machineDoc(_doc, machineId);
    if (m.isNull()) return kDefaultThresholdW;
    JsonVariantConst w = m["sensor"]["outlet"]["thresholdW"];
    return (w.is<float>() || w.is<int>()) ? w.as<float>() : kDefaultThresholdW;
  }

  // Last power reading for a machine (0 if never reported) — for the status view.
  float machineWatts(const std::string& machineId) const {
    auto it = _watts.find(machineId);
    return it == _watts.end() ? 0.0f : it->second;
  }

  // Set a machine's live power reading; tracks the OFF→ON edge (recency) and
  // reconciles.
  ReconcileResult setMachinePower(const std::string& machineId, float watts) {
    float th = machineThreshold(machineId);
    auto it = _watts.find(machineId);
    bool wasActive = it != _watts.end() && it->second >= th;
    _watts[machineId] = watts;
    bool nowActive = watts >= th;
    if (nowActive && !wasActive) _activationSeq[machineId] = ++_seqCounter;  // rising edge → newest
    if (!nowActive) _activationSeq.erase(machineId);
    return reconcile();
  }

  // Machines currently active (watts >= threshold), most-recently-activated FIRST.
  std::vector<std::string> activeMachines() const {
    std::vector<std::string> active;
    for (const std::string& id : machineIds(_doc))
      if (machineWatts(id) >= machineThreshold(id)) active.push_back(id);
    // Higher activationSeq = more recent → wins contests (most-recent-wins).
    std::sort(active.begin(), active.end(), [this](const std::string& a, const std::string& b) {
      return seqOf(a) > seqOf(b);
    });
    return active;
  }

  // Recompute desired actuator states from the active machines.
  //
  // Per system: if any active machine has an enabled port here, route to the
  // winners, close the rest of THIS system's selectors, and run this blower.
  // Otherwise hold every selector in this system where it is and stop this
  // blower. A busy 4" system therefore no longer drags the idle 2.5" one along
  // with it — which is the entire point of the container.
  ReconcileResult reconcile() {
    ReconcileResult r;
    std::vector<std::string> active = activeMachines();
    r.routing = routeShop(_doc, active);

    // Which systems are being asked for. A machine counts here even if its port
    // lost a contest: the blower still has to run for whatever DID win, and a
    // stripped machine is a thing to report, not a reason to stop the shop.
    auto ports = portsByMachine(_doc);
    for (const SystemView& sys : systemsOf(_doc))
      r.systemActive[std::string(sys.id ? sys.id : "")] = false;
    for (const std::string& mid : active) {
      auto pit = ports.find(mid);
      if (pit == ports.end()) continue;
      for (const PortRef& pr : pit->second)
        if (portEnabled(pr.port)) r.systemActive[pr.systemId] = true;
    }

    // Planned against the CURRENT collector state, before it is updated below —
    // the dead-head question is "would this destination seal a blower that is
    // turning right now".
    r.plans = planShopTransition(_doc, _actuatorStates, r.routing.states, _collectorOn);

    for (const SystemView& sys : systemsOf(_doc)) {
      const std::string sysId = sys.id ? sys.id : "";
      if (!r.systemActive[sysId]) { _collectorOn[sysId] = false; continue; }   // idle: hold
      for (JsonObjectConst e : sys.elements) {
        if (!_eq(e["type"], "selector")) continue;
        std::string id = e["id"].as<const char*>();
        auto sit = r.routing.states.find(id);
        if (sit != r.routing.states.end()) _actuatorStates[id] = sit->second;
      }
      _collectorOn[sysId] = true;
    }

    _lastRouting = r.routing;
    return r;
  }

  // Map a Shelly plug identity to the machine it powers. Prefer the stable mDNS
  // host; fall back to IP. Returns "" if no machine senses that plug.
  std::string machineForOutlet(const char* host, const char* ip) const {
    for (const std::string& id : machineIds(_doc)) {
      JsonObjectConst o = machineDoc(_doc, id)["sensor"]["outlet"];
      if (o.isNull()) continue;
      const char* oh = o["host"].as<const char*>();
      const char* oi = o["ip"].as<const char*>();
      if (host && *host && oh && strcmp(oh, host) == 0) return id;
      if (ip   && *ip   && oi && strcmp(oi, ip)   == 0) return id;
    }
    return std::string();
  }

  const std::map<std::string, std::string>& actuatorStates() const { return _actuatorStates; }
  const ShopRouting& lastRouting() const { return _lastRouting; }
  const std::map<std::string, bool>& collectorOn() const { return _collectorOn; }
  bool collectorOn(const std::string& systemId) const {
    auto it = _collectorOn.find(systemId);
    return it != _collectorOn.end() && it->second;
  }

  // ---- v1 spellings -------------------------------------------------------
  // A tool WAS the machine before ports existed, and these read better at the
  // call sites that genuinely mean "the thing that's switched on". Kept so the
  // existing conformance vectors keep cross-checking against topology-device.js
  // without being rewritten into a vocabulary the JS sim doesn't use yet.
  ReconcileResult setToolPower(const std::string& id, float w) { return setMachinePower(id, w); }
  float toolThreshold(const std::string& id) const { return machineThreshold(id); }
  float toolWatts(const std::string& id) const     { return machineWatts(id); }
  std::vector<std::string> activeTools() const     { return activeMachines(); }
  std::string toolForOutlet(const char* h, const char* i) const { return machineForOutlet(h, i); }

private:
  long seqOf(const std::string& id) const {
    auto it = _activationSeq.find(id);
    return it == _activationSeq.end() ? 0 : it->second;
  }

  JsonObjectConst _doc;
  std::map<std::string, float>       _watts;         // machineId → last reading
  std::map<std::string, long>        _activationSeq; // machineId → recency
  long                               _seqCounter = 0;
  std::map<std::string, std::string> _actuatorStates;
  std::map<std::string, bool>        _collectorOn;   // systemId → should it run
  ShopRouting                        _lastRouting;
};

} // namespace topo

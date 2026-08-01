// =============================================================================
// TopologyController.h — Phase-2 control brain (C++ port of topology-device.js).
//
// The decision core that ties routing + sequencing to live tool power. It owns
// no hardware: it consumes power readings and emits an ordered make-before-break
// move plan. A thin device layer (the main sketch) drives the stepper + servo
// bank from that plan — kept separate so this brain stays host-testable.
//
//   setToolPower(toolId, watts)  → track the OFF→ON edge (recency for
//                                  most-recent-wins) and reconcile()
//   reconcile()                  → computeRouting(active) + planTransition(),
//                                  adopt the routed states when any tool is
//                                  active, else HOLD (idle policy)
//   toolForOutlet(host, ip)      → map a Shelly plug identity back to its tool,
//                                  so SmartOutletControl's per-plug power can be
//                                  fed in by toolId
//
// Mirrors topology-device.js exactly (activeTools ordering, idle-hold, default
// threshold) so the host conformance test can cross-check them value-for-value.
// PURE — ArduinoJson + STL, NO Arduino.h.
// =============================================================================

#pragma once
#include <ArduinoJson.h>
#include "TopologyRouter.h"
#include "TopologySequencer.h"
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace topo {

static const float kDefaultThresholdW = 5.0f;   // DEFAULT_THRESHOLD_W in topology-device.js

struct ReconcileResult {
  Routing        routing;   // per-selector desired state + reachable map
  TransitionPlan plan;      // ordered make-before-break moves to get there
};

class Controller {
public:
  // Adopt a parsed topology. Seeds every selector's current state to its closed
  // (idle) state, matching createTopologyDevice(). Clears power history.
  void setTopology(JsonObjectConst topology) {
    _topo = topology;
    _toolWatts.clear();
    _activationSeq.clear();
    _seqCounter = 0;
    _collectorOn = false;
    _actuatorStates.clear();
    for (JsonObjectConst sel : _topo["elements"].as<JsonArrayConst>()) {
      if (!_eq(sel["type"], "selector")) continue;
      std::string id = sel["id"].as<const char*>();
      const char* cs = _closedState(sel);
      _actuatorStates[id] = cs ? std::string(cs) : std::string();
    }
  }

  // sensor.outlet.thresholdW for a tool, or the default.
  float toolThreshold(const std::string& toolId) const {
    for (JsonObjectConst e : _topo["elements"].as<JsonArrayConst>()) {
      if (_eq(e["id"], toolId.c_str())) {
        JsonVariantConst w = e["sensor"]["outlet"]["thresholdW"];
        return w.is<float>() || w.is<int>() ? w.as<float>() : kDefaultThresholdW;
      }
    }
    return kDefaultThresholdW;
  }

  // Set a tool's live power reading; tracks the OFF→ON edge (recency) and reconciles.
  ReconcileResult setToolPower(const std::string& toolId, float watts) {
    float th = toolThreshold(toolId);
    auto it = _toolWatts.find(toolId);
    bool wasActive = it != _toolWatts.end() && it->second >= th;
    _toolWatts[toolId] = watts;
    bool nowActive = watts >= th;
    if (nowActive && !wasActive) _activationSeq[toolId] = ++_seqCounter;  // rising edge → newest
    if (!nowActive) _activationSeq.erase(toolId);
    return reconcile();
  }

  // Tools currently active (watts >= threshold), most-recently-activated FIRST.
  std::vector<std::string> activeTools() const {
    std::vector<std::string> active;
    for (JsonObjectConst e : _topo["elements"].as<JsonArrayConst>()) {
      if (!_eq(e["type"], "tool")) continue;
      std::string id = e["id"].as<const char*>();
      auto wit = _toolWatts.find(id);
      float w = wit == _toolWatts.end() ? 0.0f : wit->second;
      if (w >= toolThreshold(id)) active.push_back(id);
    }
    // Higher activationSeq = more recent → wins contests (most-recent-wins).
    std::sort(active.begin(), active.end(), [this](const std::string& a, const std::string& b) {
      return seqOf(a) > seqOf(b);
    });
    return active;
  }

  // Recompute desired actuator states from the active tools. When any tool is
  // active: route to the winners, close the rest, collector on. Idle: HOLD all
  // selectors where they are, collector off.
  ReconcileResult reconcile() {
    ReconcileResult r;
    std::vector<std::string> active = activeTools();
    r.routing = computeRouting(_topo, active);
    r.plan    = planTransition(_topo, _actuatorStates, r.routing.states, _collectorOn);

    if (!active.empty()) {
      for (auto& kv : r.routing.states) _actuatorStates[kv.first] = kv.second;
      _collectorOn = true;
    } else {
      _collectorOn = false;   // idle: hold positions (actuatorStates unchanged)
    }
    _lastRouting = r.routing;
    return r;
  }

  // Map a Shelly plug identity to the tool it powers. Prefer the stable mDNS
  // host; fall back to IP. Returns "" if no tool senses that plug.
  std::string toolForOutlet(const char* host, const char* ip) const {
    for (JsonObjectConst e : _topo["elements"].as<JsonArrayConst>()) {
      if (!_eq(e["type"], "tool")) continue;
      JsonObjectConst o = e["sensor"]["outlet"];
      if (o.isNull()) continue;
      const char* oh = o["host"].as<const char*>();
      const char* oi = o["ip"].as<const char*>();
      if (host && *host && oh && strcmp(oh, host) == 0) return e["id"].as<const char*>();
      if (ip   && *ip   && oi && strcmp(oi, ip)   == 0) return e["id"].as<const char*>();
    }
    return std::string();
  }

  const std::map<std::string, std::string>& actuatorStates() const { return _actuatorStates; }
  const Routing& lastRouting() const { return _lastRouting; }
  bool  collectorOn() const { return _collectorOn; }

private:
  long seqOf(const std::string& id) const {
    auto it = _activationSeq.find(id);
    return it == _activationSeq.end() ? 0 : it->second;
  }

  JsonObjectConst _topo;
  std::map<std::string, float>       _toolWatts;
  std::map<std::string, long>        _activationSeq;
  long                               _seqCounter = 0;
  std::map<std::string, std::string> _actuatorStates;
  bool                               _collectorOn = false;
  Routing                            _lastRouting;
};

} // namespace topo

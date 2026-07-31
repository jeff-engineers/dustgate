// =============================================================================
// TopologyRouter.h — Phase-2 airflow routing (C++ port of routing.js).
//
// Pure logic: given a parsed topology (ArduinoJson) and the set of active tools
// in priority order (most-recently-powered-on first, for most-recent-wins),
// compute the state every SELECTOR should be commanded into to route airflow
// from each active tool to the collector.
//
//   • Junctions (passive tees) are walked through transparently — the gate above
//     governs the whole group.
//   • Greedy priority: a tool wins its whole path only if no selector on it is
//     already committed by an earlier (higher-priority) tool to a DIFFERENT
//     state. Sharing a selector at the SAME state is fine (both flow).
//
// Dependency-free apart from ArduinoJson + the STL, so it host-compiles for the
// conformance test (test/test_topology_router.cpp) that cross-checks it against
// the JS engine — NO Arduino.h. The firmware controller layer feeds it the
// active-tool set and drives actuators from `states`.
// =============================================================================

#pragma once
#include <ArduinoJson.h>
#include <map>
#include <vector>
#include <string>
#include <cstring>

namespace topo {

struct Routing {
  std::map<std::string, std::string> states;    // selectorId -> stateId to command (closed if idle)
  std::map<std::string, bool>        reachable; // toolId -> won a clear open path right now
};

inline bool _eq(JsonVariantConst v, const char* s) {
  const char* p = v.as<const char*>();
  return p && s && strcmp(p, s) == 0;
}

inline std::map<std::string, JsonObjectConst> _byId(JsonObjectConst t) {
  std::map<std::string, JsonObjectConst> m;
  for (JsonObjectConst e : t["elements"].as<JsonArrayConst>()) m[std::string(e["id"].as<const char*>())] = e;
  return m;
}
inline std::map<std::string, JsonObjectConst> _parentDuct(JsonObjectConst t) {
  std::map<std::string, JsonObjectConst> m;
  for (JsonObjectConst d : t["ducts"].as<JsonArrayConst>()) {
    std::string child = d["child"].as<const char*>();
    if (m.find(child) == m.end()) m[child] = d;   // first duct wins (one parent per element)
  }
  return m;
}
inline const char* _closedState(JsonObjectConst sel) {
  for (JsonObjectConst s : sel["states"].as<JsonArrayConst>())
    if (s["isClosed"].as<bool>()) return s["id"].as<const char*>();
  return nullptr;
}

// Walk a tool up to the collector, collecting the (selectorId, stateId) each hop
// needs. Returns false for an orphan/broken chain (never reaches the collector).
inline bool _pathToCollector(const std::string& toolId,
                             std::map<std::string, JsonObjectConst>& byId,
                             std::map<std::string, JsonObjectConst>& parentDuct,
                             std::vector<std::pair<std::string, std::string>>& path) {
  std::string cur = toolId;
  for (int guard = 0; guard < 256; guard++) {
    auto eit = byId.find(cur); if (eit == byId.end()) return false;
    JsonObjectConst el = eit->second;
    if (_eq(el["type"], "collector")) return true;
    auto dit = parentDuct.find(cur); if (dit == parentDuct.end()) return false;   // orphan
    JsonObjectConst d = dit->second;
    std::string parentId = d["parent"].as<const char*>();
    auto pit = byId.find(parentId); if (pit == byId.end()) return false;
    JsonObjectConst parent = pit->second;
    if (_eq(parent["type"], "selector")) {
      const char* pb = d["parentBranch"].as<const char*>();
      for (JsonObjectConst b : parent["branches"].as<JsonArrayConst>()) {
        if (pb && _eq(b["id"], pb)) { path.push_back({parentId, std::string(b["opensState"].as<const char*>())}); break; }
      }
    }
    cur = parentId;   // junctions & the collector fall through with no state
  }
  return false;
}

// `active` is the caller's priority order (most-recent-first for most-recent-wins).
inline Routing computeRouting(JsonObjectConst topology, const std::vector<std::string>& active) {
  Routing out;
  auto byId = _byId(topology);
  auto parentDuct = _parentDuct(topology);

  std::map<std::string, std::vector<std::pair<std::string, std::string>>> paths;
  for (const auto& toolId : active) {
    std::vector<std::pair<std::string, std::string>> p;
    if (_pathToCollector(toolId, byId, parentDuct, p)) paths[toolId] = p;
    else out.reachable[toolId] = false;
  }

  std::map<std::string, std::string> committed;   // selectorId -> stateId (first commit wins)
  for (const auto& toolId : active) {
    auto pit = paths.find(toolId);
    if (pit == paths.end()) continue;             // already unreachable
    auto& path = pit->second;
    bool blocked = false;
    for (auto& hop : path) {
      auto c = committed.find(hop.first);
      if (c != committed.end() && c->second != hop.second) { blocked = true; break; }
    }
    if (blocked) { out.reachable[toolId] = false; continue; }
    for (auto& hop : path) if (committed.find(hop.first) == committed.end()) committed[hop.first] = hop.second;
    out.reachable[toolId] = true;
  }

  // every selector defaults to its closed state; winners override
  for (JsonObjectConst e : topology["elements"].as<JsonArrayConst>()) {
    if (_eq(e["type"], "selector")) {
      const char* cs = _closedState(e);
      out.states[std::string(e["id"].as<const char*>())] = cs ? std::string(cs) : std::string();
    }
  }
  for (auto& kv : committed) out.states[kv.first] = kv.second;
  return out;
}

// Servo command angle for a selector state: referenceAngle + offsetDeg, clamped.
// Returns INT32_MIN if the selector isn't a servo or the state has no offset.
inline int servoCommandAngle(JsonObjectConst sel, const char* stateId) {
  const char* kind = sel["kind"].as<const char*>();
  if (!kind || (strcmp(kind, "servoGate") != 0 && strcmp(kind, "servoManifold") != 0)) return INT32_MIN;
  for (JsonObjectConst s : sel["states"].as<JsonArrayConst>()) {
    if (_eq(s["id"], stateId)) {
      if (!s.containsKey("offsetDeg")) return INT32_MIN;
      JsonObjectConst sv = sel["servo"];
      int ref = sv["referenceAngle"] | 0;
      int lo  = sv["minAngle"] | 0;
      int hi  = sv["maxAngle"] | 180;
      int ang = ref + s["offsetDeg"].as<int>();
      if (ang < lo) ang = lo;
      if (ang > hi) ang = hi;
      return ang;
    }
  }
  return INT32_MIN;
}

} // namespace topo

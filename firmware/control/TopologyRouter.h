// =============================================================================
// TopologyRouter.h — airflow routing (C++ port of routing.js).
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

// A selector that a lower-priority tool had to yield to an earlier one: both
// wanted it, in different states, and only one can have it. Surfaced (never
// resolved by last-writer-wins) so the UI can say WHY a tool isn't pulling —
// see routing.js's identically-shaped conflicts[].
struct Conflict {
  std::string              selectorId;
  std::string              winner;       // toolId that got the selector
  std::string              winnerState;
  std::vector<std::string> losers;       // toolIds blocked by it
};

struct Routing {
  std::map<std::string, std::string> states;    // selectorId -> stateId to command (closed if idle)
  std::map<std::string, bool>        reachable; // toolId -> won a clear open path right now
  std::vector<Conflict>              conflicts; // contested selectors, in selector order
};

inline bool _eq(JsonVariantConst v, const char* s) {
  const char* p = v.as<const char*>();
  return p && s && strcmp(p, s) == 0;
}

// One airflow graph, as three arrays rather than a document.
//
// This is the C++ half of systemView() in shared/device-model/shop.js, and it
// exists for the same reason: a shop holds N systems, each of which is a
// topology in its own right, and every function below this line is a statement
// about ONE blower. The JS version can afford to build a fresh object per system
// because it runs on a laptop. Here it has to be a VIEW — three JsonArrayConst
// handles pointing back into the one parsed document — because materialising a
// copy of every system's elements on an ESP32 would double the largest
// allocation in the firmware for no benefit.
//
// `controllers` is shop-level, not system-level: a board is mounted where the
// cable reaches and may drive selectors in any number of systems, so each view
// carries the same array (RFC §14). `id` is "" for a schemaVersion-1 document,
// which has exactly one implicit system.
struct SystemView {
  JsonArrayConst controllers;
  JsonArrayConst elements;
  JsonArrayConst ducts;
  const char*    id;
};

// A v1 topology, viewed as the single system it always was. The shop layer
// (Shop.h) produces these for a v2 document; this overload is what keeps every
// existing call site, fixture and conformance test working untouched.
inline SystemView viewOf(JsonObjectConst t) {
  return SystemView{ t["controllers"].as<JsonArrayConst>(),
                     t["elements"].as<JsonArrayConst>(),
                     t["ducts"].as<JsonArrayConst>(),
                     t["id"] | "" };
}

inline std::map<std::string, JsonObjectConst> _byId(const SystemView& t) {
  std::map<std::string, JsonObjectConst> m;
  for (JsonObjectConst e : t.elements) m[std::string(e["id"].as<const char*>())] = e;
  return m;
}
inline std::map<std::string, JsonObjectConst> _parentDuct(const SystemView& t) {
  std::map<std::string, JsonObjectConst> m;
  for (JsonObjectConst d : t.ducts) {
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
//
// Under a shop the ids in `active` are PORT ids, not machine ids — one machine
// can hold several ports, and each is a separate leaf of the graph. Shop.h owns
// that expansion; from here down a port is simply what a tool has always been.
inline Routing computeRouting(const SystemView& topology, const std::vector<std::string>& active) {
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
  std::map<std::string, std::string> winnerOf;    // selectorId -> toolId that claimed it
  std::map<std::string, Conflict>    conflictBySel;
  for (const auto& toolId : active) {
    auto pit = paths.find(toolId);
    if (pit == paths.end()) continue;             // already unreachable
    auto& path = pit->second;
    std::string blockedSel;
    bool blocked = false;
    for (auto& hop : path) {
      auto c = committed.find(hop.first);
      if (c != committed.end() && c->second != hop.second) { blocked = true; blockedSel = hop.first; break; }
    }
    if (blocked) {
      out.reachable[toolId] = false;
      auto cit = conflictBySel.find(blockedSel);
      if (cit == conflictBySel.end())
        cit = conflictBySel.insert({blockedSel,
                Conflict{blockedSel, winnerOf[blockedSel], committed[blockedSel], {}}}).first;
      cit->second.losers.push_back(toolId);
      continue;
    }
    for (auto& hop : path)
      if (committed.find(hop.first) == committed.end()) {
        committed[hop.first] = hop.second;
        winnerOf[hop.first]  = toolId;
      }
    out.reachable[toolId] = true;
  }
  for (auto& kv : conflictBySel) out.conflicts.push_back(kv.second);

  // every selector defaults to its closed state; winners override
  for (JsonObjectConst e : topology.elements) {
    if (_eq(e["type"], "selector")) {
      const char* cs = _closedState(e);
      out.states[std::string(e["id"].as<const char*>())] = cs ? std::string(cs) : std::string();
    }
  }
  for (auto& kv : committed) out.states[kv.first] = kv.second;
  return out;
}

// Convenience for a plain (schemaVersion 1) topology document.
inline Routing computeRouting(JsonObjectConst topology, const std::vector<std::string>& active) {
  return computeRouting(viewOf(topology), active);
}

// Has this servo selector actually been calibrated?
//
// servoCommandAngle() below mirrors the JS resolver exactly, and the JS resolver
// DEFAULTS a missing referenceAngle to 0. That's fine for the UI, which greys
// out uncalibrated gates before it ever asks for an angle — but firmware must
// not act on that default: it would drive a real ball valve to a made-up
// position. Anything that COMMANDS hardware checks this first and refuses;
// anything that merely computes (tests, status) can call the resolver directly.
// Kept separate from the resolver so the C++/JS parity contract stays exact.
inline bool servoIsCalibrated(JsonObjectConst sel) {
  JsonObjectConst sv = sel["servo"];
  return !sv.isNull() && sv.containsKey("referenceAngle");
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

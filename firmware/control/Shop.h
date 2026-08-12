// =============================================================================
// Shop.h — the SHOP layer (C++ port of shared/device-model/shop.js).
//
// One brain, N airflow systems. This is the thin container that sits ABOVE
// TopologyRouter.h and TopologySequencer.h; neither of those learns that more
// than one blower exists. What this file owns is exactly what shop.js owns:
//
//   • the container   — systems[], each a topology in its own right
//   • the indirection — machines[] own the plug, the trip point and the name;
//                       `tool` elements are now PORTS carrying a machineId
//   • the merge       — union the per-system results, then roll ports up to
//                       machines
//
// WHY A CONTAINER (RFC §4.2): every airflow invariant below this line is a
// statement about ONE blower — "reach the collector", "what bleeds through",
// "can the blower get sealed". Two collectors in one graph turns each of those
// into a question with two answers, and would accept a 4" tool routed through
// the 2.5" manifold. Lifting the container above them leaves all three correct
// exactly as written.
//
// V1 COMPATIBILITY IS NOT A SEPARATE PATH. A schemaVersion-1 document is simply
// a shop with one anonymous system whose machines are its tool elements: a tool
// carried its own `name` and `sensor.outlet`, which is precisely what a machine
// carries now, so machineDoc() hands back the tool element itself and every
// reader above works unchanged. That is why there is no migrateToShop() here —
// the firmware never rewrites the stored document, it just reads both shapes.
// (The UI does migrate on save; see shop.js.)
//
// PURE — ArduinoJson + STL only, NO Arduino.h, so it host-compiles for the
// conformance test alongside the router and sequencer.
// =============================================================================

#pragma once
#include <ArduinoJson.h>
#include "TopologyRouter.h"
#include "TopologySequencer.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace topo {

// Synthetic id for the single implicit system of a v1 document. Matches the
// default migrateToShop() uses in shop.js, so a layout that gets migrated by the
// UI keeps the same system id the firmware had already been reporting.
static const char* kImplicitSystemId = "system-1";

/** True for a document already in shop shape (has a systems[] array). */
inline bool isShop(JsonObjectConst doc) {
  return doc["systems"].is<JsonArrayConst>();
}

/**
 * Every system in the document, as views into it.
 *
 * A v1 topology yields exactly one view over its own elements/ducts — the
 * whole of the version handling, in one branch.
 */
inline std::vector<SystemView> systemsOf(JsonObjectConst doc) {
  std::vector<SystemView> out;
  JsonArrayConst controllers = doc["controllers"].as<JsonArrayConst>();
  if (!isShop(doc)) {
    out.push_back(SystemView{ controllers,
                              doc["elements"].as<JsonArrayConst>(),
                              doc["ducts"].as<JsonArrayConst>(),
                              kImplicitSystemId });
    return out;
  }
  for (JsonObjectConst sys : doc["systems"].as<JsonArrayConst>()) {
    out.push_back(SystemView{ controllers,
                              sys["elements"].as<JsonArrayConst>(),
                              sys["ducts"].as<JsonArrayConst>(),
                              sys["id"] | "" });
  }
  return out;
}

/**
 * Which machine a port belongs to.
 *
 * v2 ports say so explicitly. A v1 tool element IS its own machine, so it
 * answers with its own id — which is also the id migrateToShop() gives the
 * machine it creates, so ids stay stable across the migration either way.
 */
inline std::string machineIdOf(JsonObjectConst port) {
  const char* m = port["machineId"].as<const char*>();
  if (m && *m) return std::string(m);
  const char* id = port["id"].as<const char*>();
  return id ? std::string(id) : std::string();
}

/**
 * A port counts for routing unless it is EXPLICITLY disabled.
 *
 * Opt-out, not opt-in, so a v1 document — where no port carries the field at
 * all — routes exactly as it did before.
 */
inline bool portEnabled(JsonObjectConst port) {
  JsonVariantConst e = port["enabled"];
  return e.isNull() || e.as<bool>();
}

/**
 * The object carrying a machine's identity: its name, and its `sensor.outlet`.
 *
 * v2: the entry in machines[]. v1: the tool element itself. Returns a null
 * object for an unknown id.
 */
inline JsonObjectConst machineDoc(JsonObjectConst doc, const std::string& machineId) {
  if (isShop(doc)) {
    for (JsonObjectConst m : doc["machines"].as<JsonArrayConst>())
      if (_eq(m["id"], machineId.c_str())) return m;
    return JsonObjectConst();
  }
  for (JsonObjectConst e : doc["elements"].as<JsonArrayConst>())
    if (_eq(e["type"], "tool") && _eq(e["id"], machineId.c_str())) return e;
  return JsonObjectConst();
}

/** Every machine id in the document, in document order. */
inline std::vector<std::string> machineIds(JsonObjectConst doc) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  if (isShop(doc)) {
    for (JsonObjectConst m : doc["machines"].as<JsonArrayConst>()) {
      const char* id = m["id"].as<const char*>();
      if (id && *id && seen.insert(id).second) out.push_back(id);
    }
    return out;
  }
  for (JsonObjectConst e : doc["elements"].as<JsonArrayConst>()) {
    if (!_eq(e["type"], "tool")) continue;
    const char* id = e["id"].as<const char*>();
    if (id && *id && seen.insert(id).second) out.push_back(id);
  }
  return out;
}

struct PortRef {
  std::string     systemId;
  JsonObjectConst port;
};

/**
 * machineId → every port of that machine, in document order.
 *
 * INCLUDES disabled ports: routing filters them, but a caller that wants to say
 * "this machine has a port you turned off" needs to see them.
 */
inline std::map<std::string, std::vector<PortRef>> portsByMachine(JsonObjectConst doc) {
  std::map<std::string, std::vector<PortRef>> out;
  for (const SystemView& sys : systemsOf(doc)) {
    for (JsonObjectConst el : sys.elements) {
      if (!_eq(el["type"], "tool")) continue;
      std::string mid = machineIdOf(el);
      if (mid.empty()) continue;
      out[mid].push_back(PortRef{ std::string(sys.id ? sys.id : ""), el });
    }
  }
  return out;
}

/** The collector element of a system (null object if the system has none). */
inline JsonObjectConst collectorOf(const SystemView& sys) {
  for (JsonObjectConst e : sys.elements)
    if (_eq(e["type"], "collector")) return e;
  return JsonObjectConst();
}

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

// A machine is only as routed as its WORST port. Three answers, not two,
// because a saw whose cabinet gate is shut while its overarm is open is a very
// different situation from one that is simply running (RFC §10.3).
enum MachineStatus {
  MACHINE_ROUTED,     // every enabled port got a clear path
  MACHINE_PARTIAL,    // some ports lost, and every one that lost was supplemental
  MACHINE_STRIPPED    // a PRIMARY port lost — the alarm case: saw running, gate shut
};

inline const char* machineStatusName(MachineStatus s) {
  switch (s) {
    case MACHINE_ROUTED:  return "routed";
    case MACHINE_PARTIAL: return "partial";
    default:              return "stripped";
  }
}

struct MachineRouting {
  std::vector<std::string> routed;    // port ids with a clear path
  std::vector<std::string> blocked;   // port ids that lost
  MachineStatus            status;
};

// A Conflict, tagged with the system it happened in. With one system the tag was
// noise; with several, "gate-2 is contested" doesn't say which duct run to look at.
struct ShopConflict {
  std::string              systemId;
  std::string              selectorId;
  std::string              winner;       // the PORT id that got the selector
  std::string              winnerState;
  std::vector<std::string> losers;       // port ids blocked by it
};

struct ShopRouting {
  std::map<std::string, std::string>    states;     // selectorId → state to command (shop-wide)
  std::map<std::string, bool>           reachable;  // PORT id → won a clear path
  std::vector<ShopConflict>             conflicts;
  std::map<std::string, MachineRouting> machines;   // machineId → rolled-up verdict
};

/**
 * Route the whole shop.
 *
 * Systems share no duct, so routing genuinely is per-system and the merge is a
 * plain union — there is no cross-system arbitration to get wrong. What this
 * adds is the machine indirection: an active MACHINE opens the path to every
 * ENABLED port carrying its id, in whatever system that port lives.
 *
 * @param activeMachineIds highest priority first (most-recent-on first, for
 *        most-recent-wins). The order is preserved into each system, so the
 *        policy keeps working unchanged within a system.
 */
inline ShopRouting routeShop(JsonObjectConst doc,
                             const std::vector<std::string>& activeMachineIds) {
  ShopRouting out;
  auto ports = portsByMachine(doc);

  // portId → the port element, for the supplemental check at the end.
  std::map<std::string, JsonObjectConst> portIndex;

  for (const SystemView& sys : systemsOf(doc)) {
    const std::string sysId = sys.id ? sys.id : "";

    // Machine ids → this system's enabled port ids, priority order preserved.
    // One machine can hold SEVERAL ports in one system (floor gate + overarm on
    // the same collector), so this is a flatten, not a lookup.
    std::vector<std::string> activePorts;
    std::vector<std::pair<std::string, std::vector<std::string>>> portsOfMachineHere;
    for (const std::string& mid : activeMachineIds) {
      auto pit = ports.find(mid);
      if (pit == ports.end()) continue;
      std::vector<std::string> here;
      for (const PortRef& pr : pit->second) {
        if (pr.systemId != sysId || !portEnabled(pr.port)) continue;
        const char* pid = pr.port["id"].as<const char*>();
        if (!pid) continue;
        here.push_back(pid);
        activePorts.push_back(pid);
      }
      if (!here.empty()) portsOfMachineHere.push_back({mid, here});
    }

    for (JsonObjectConst el : sys.elements)
      if (_eq(el["type"], "tool")) {
        const char* pid = el["id"].as<const char*>();
        if (pid) portIndex[pid] = el;
      }

    Routing r = computeRouting(sys, activePorts);

    for (auto& kv : r.states)    out.states[kv.first]    = kv.second;
    for (auto& kv : r.reachable) out.reachable[kv.first] = kv.second;
    for (const Conflict& c : r.conflicts)
      out.conflicts.push_back(ShopConflict{ sysId, c.selectorId, c.winner, c.winnerState, c.losers });

    for (auto& mp : portsOfMachineHere) {
      MachineRouting& mr = out.machines[mp.first];   // accumulates across systems
      for (const std::string& pid : mp.second) {
        auto rit = r.reachable.find(pid);
        bool got = rit != r.reachable.end() && rit->second;
        (got ? mr.routed : mr.blocked).push_back(pid);
      }
    }
  }

  // Final per-machine verdict, once every system has reported.
  //
  // `supplemental: true` marks a port whose loss degrades capture but is not a
  // problem in itself — the overarm. ABSENT MEANS PRIMARY, because a port nobody
  // has thought about is a port whose air you actually need.
  for (auto& kv : out.machines) {
    MachineRouting& m = kv.second;
    if (m.blocked.empty()) { m.status = MACHINE_ROUTED; continue; }
    bool allSupplemental = true;
    for (const std::string& pid : m.blocked) {
      auto it = portIndex.find(pid);
      if (it == portIndex.end() || it->second["supplemental"].as<bool>() != true) {
        allSupplemental = false; break;
      }
    }
    m.status = allSupplemental ? MACHINE_PARTIAL : MACHINE_STRIPPED;
  }
  // An active machine whose every port is disabled reaches nothing at all. It
  // has to be answered for rather than omitted — omission looks like it was
  // never asked, which is the one reading that hides a running tool with no air.
  for (const std::string& mid : activeMachineIds)
    if (out.machines.find(mid) == out.machines.end())
      out.machines[mid] = MachineRouting{ {}, {}, MACHINE_STRIPPED };

  return out;
}

// ---------------------------------------------------------------------------
// Transition planning
// ---------------------------------------------------------------------------

struct SystemPlan {
  std::string       systemId;
  std::vector<Move> moves;
  // PER SYSTEM, deliberately, rather than or-ed into one flag: it asks whether a
  // particular blower ends up sealed, and with two blowers there are genuinely
  // two answers. Collapsing them hides which one is at risk, which is the only
  // thing the caller can act on.
  bool              deadHeadRisk;
};

/**
 * Plan the moves for a whole shop.
 *
 * Make-before-break is a statement about one blower's air, so plans are computed
 * per system — but they come back as an ordered LIST OF PLANS that the caller
 * executes back-to-back, never interleaved. Interleaving would let system B's
 * break land between system A's make and A's break, which is exactly the
 * dead-head the sequencer exists to prevent. Execution stays shop-wide serial
 * anyway: the one-servo-at-a-time current budget is a property of the power
 * supply, not of a duct run (RFC §10.2).
 *
 * @param collectorRunning systemId → is that blower turning right now.
 */
inline std::vector<SystemPlan> planShopTransition(
    JsonObjectConst doc,
    const std::map<std::string, std::string>& currentStates,
    const std::map<std::string, std::string>& desiredStates,
    const std::map<std::string, bool>& collectorRunning) {
  std::vector<SystemPlan> out;
  for (const SystemView& sys : systemsOf(doc)) {
    const std::string sysId = sys.id ? sys.id : "";

    // Narrow the state maps to this system's selectors. planTransition reasons
    // about "every selector in the topology"; handing it the shop-wide map would
    // let a sibling system's open gate answer the dead-head question for this
    // blower, which is the whole failure this layering exists to avoid.
    std::set<std::string> mine;
    for (JsonObjectConst e : sys.elements)
      if (_eq(e["type"], "selector")) {
        const char* id = e["id"].as<const char*>();
        if (id) mine.insert(id);
      }
    std::map<std::string, std::string> cur, des;
    for (auto& kv : currentStates) if (mine.count(kv.first)) cur[kv.first] = kv.second;
    for (auto& kv : desiredStates) if (mine.count(kv.first)) des[kv.first] = kv.second;

    auto rit = collectorRunning.find(sysId);
    TransitionPlan plan = planTransition(sys, cur, des,
                                         rit != collectorRunning.end() && rit->second);
    // Reported even with NO moves when the blower is at risk — an empty plan is
    // exactly how a system arrives at "everything shut while the fan runs".
    if (!plan.moves.empty() || plan.deadHeadRisk)
      out.push_back(SystemPlan{ sysId, plan.moves, plan.deadHeadRisk });
  }
  return out;
}

} // namespace topo

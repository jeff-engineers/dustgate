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
// SCHEMAVERSION 1 IS NOT READ (2026-10-06). It used to be a shop with one anonymous system whose machines were its tool
// elements; nothing can produce one now and TopologyRuntime refuses it with a sentence, so a document without a systems[]
// array reads as having no systems rather than as a guess.
//
// PURE — ArduinoJson + STL only, NO Arduino.h, so it host-compiles for the
// conformance test alongside the router and sequencer.
// =============================================================================

#pragma once
#include <ArduinoJson.h>
#include "TopologyRouter.h"
#include "TopologySequencer.h"
#include "BoardId.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace topo {

/**
 * Every system in the document, as views into it. A document with no systems[] has none.
 */
inline std::vector<SystemView> systemsOf(JsonObjectConst doc) {
  std::vector<SystemView> out;
  JsonArrayConst controllers = doc["controllers"].as<JsonArrayConst>();
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
 * A port says so explicitly (`machineId`); one that does not is its own machine and answers with its own id.
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
 * Opt-out, not opt-in: a port that does not carry the field routes.
 */
inline bool portEnabled(JsonObjectConst port) {
  JsonVariantConst e = port["enabled"];
  return e.isNull() || e.as<bool>();
}

/**
 * The object carrying a machine's identity: its name, and its `sensor.outlet`.
 *
 * The entry in machines[]. Returns a null object for an unknown id.
 */
inline JsonObjectConst machineDoc(JsonObjectConst doc, const std::string& machineId) {
  for (JsonObjectConst m : doc["machines"].as<JsonArrayConst>())
    if (_eq(m["id"], machineId.c_str())) return m;
  return JsonObjectConst();
}

/**
 * The CT clamp watching this element, resolved the way its PLUG already is.
 *
 * A TOOL's clamp lives on its MACHINE — one box, one clamp, however many ports
 * it has — exactly like `sensor.outlet`, and for the same reason: the routing
 * brain only ever senses machines. A COLLECTOR has no machine entry and carries
 * its own.
 *
 * ⚠️ THIS DID NOT EXIST UNTIL 2026-09-16, and its absence is why no clamp paired
 * in the UI ever reached a node. TopologyRuntime read `element.sensor.ct`
 * directly, which is null for every tool the configurator writes — so CONFIG went
 * out empty, the node never sampled, and the whole chain was silent with nothing
 * anywhere reporting a fault. The conformance suite passed throughout because its
 * fixtures put `sensor.ct` on the element by hand, which is a shape the UI cannot
 * produce.
 *
 * The identical machine-vs-element mistake had already been made twice in the
 * front end (clampLeads(), and the demo's sense report). Three times is a rule:
 * anything reading `sensor.*` for a tool goes through the machine.
 */
inline JsonObjectConst clampOf(JsonObjectConst doc, JsonObjectConst el) {
  if (_eq(el["type"], "collector")) return el["sensor"]["ct"].as<JsonObjectConst>();
  if (!_eq(el["type"], "tool"))     return JsonObjectConst();
  return machineDoc(doc, machineIdOf(el))["sensor"]["ct"].as<JsonObjectConst>();
}

/**
 * What a clamp on this element is SENSING, as an id.
 *
 * The MACHINE for a tool, the element for a collector — which is what makes the
 * id round-trip correctly: a node echoes `sensorId` back in its SENSE frame, and
 * the primary feeds that straight into setMachinePower(), which is keyed by
 * machine. Sending a port id instead reaches machineIndex(), finds nothing, and
 * does nothing at all — silently. It also means a two-port machine yields ONE
 * sensor, not one per port.
 */
inline std::string sensedIdOf(JsonObjectConst el) {
  if (_eq(el["type"], "collector")) {
    const char* id = el["id"].as<const char*>();
    return id ? std::string(id) : std::string();
  }
  return machineIdOf(el);
}

/** Every machine id in the document, in document order. */
inline std::vector<std::string> machineIds(JsonObjectConst doc) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  for (JsonObjectConst m : doc["machines"].as<JsonArrayConst>()) {
    const char* id = m["id"].as<const char*>();
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

/**
 * Which BOARD should poll this machine's plug: its controllerId, or "" for the
 * brain. See plugOwners() in shared/device-model/shop.js for the rules and the
 * reason — MATCHED PAIR, same fixture (test/fixtures/plugOwners.json).
 */
inline std::string plugOwnerOf(JsonObjectConst doc, const std::string& machineId) {
  std::vector<std::string> secondary;
  for (JsonObjectConst c : doc["controllers"].as<JsonArrayConst>())
    if (!_eq(c["role"], "primary")) secondary.push_back(std::string(c["id"] | ""));
  auto isSecondary = [&](const char* id) {
    if (!id || !*id) return false;
    for (const std::string& s : secondary) if (s == id) return true;
    return false;
  };
  auto ports = portsByMachine(doc);
  auto it = ports.find(machineId);
  if (it == ports.end()) return "";
  for (const PortRef& pr : it->second) {
    for (const SystemView& sys : systemsOf(doc)) {
      if (std::string(sys.id ? sys.id : "") != pr.systemId) continue;
      const char* portId = pr.port["id"] | "";
      const char* parentId = nullptr;
      for (JsonObjectConst d : sys.ducts)
        if (_eq(d["child"], portId)) { parentId = d["parent"] | ""; break; }
      if (!parentId) break;
      for (JsonObjectConst e : sys.elements)
        if (_eq(e["id"], parentId)) {
          if (_eq(e["type"], "selector") && isSecondary(e["controllerId"] | "")) return std::string(e["controllerId"] | "");
          break;
        }
      break;
    }
  }
  return "";
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
    std::vector<std::string> primaries, supplementals;
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
        (pr.port["supplemental"].as<bool>() == true ? supplementals : primaries).push_back(pid);
      }
      if (!here.empty()) portsOfMachineHere.push_back({mid, here});
    }

    // ARBITRATION ORDER (RFC §11.3). computeRouting is greedy in list order, so
    // the order of this list IS the policy:
    //
    //   1. primary beats supplemental, whatever started more recently
    //   2. among primaries    — most-recent-wins, unchanged
    //   3. among supplementals — most-recent-wins
    //
    // Step 1 matters more than it looks. Without it, starting the table saw
    // AFTER someone's bandsaw hands the manifold to the saw's overarm and leaves
    // the bandsaw cutting into a closed gate: recency beating need.
    // activeMachineIds is already in recency order and both buckets are filled
    // by walking it once, so rules 2 and 3 hold without a second sort.
    std::vector<std::string> activePorts = primaries;
    activePorts.insert(activePorts.end(), supplementals.begin(), supplementals.end());

    // ── ONE MACHINE PER SYSTEM GETS THE AIR ──────────────────────────────────
    //
    // computeRouting only resolves contests over a SELECTOR: two tools on
    // independent gates contest nothing, so both used to win and both gates
    // opened. That is co-open — the same condition airflowIssues() reports as a
    // layout fault — and it halves the velocity at both, which is the exact
    // failure automated gates exist to prevent. It also contradicts the locked
    // scope: one blower, one tool at a time, arbitrated per system.
    //
    // So the order above no longer just breaks ties, it picks a WINNER: the
    // machine owning the first port in arbitration order. Every other machine in
    // this system loses, however free its gate happens to be.
    //
    // Per MACHINE, not per port — a saw with a cabinet gate and an overarm on the
    // same collector wants both open (one box, two pickups; RFC §6.3).
    //
    // ↔ routeShop() in shared/device-model/shop.js. Change one, change both.
    std::map<std::string, std::string> ownerOfPort;
    for (auto& mp : portsOfMachineHere)
      for (const std::string& pid : mp.second) ownerOfPort[pid] = mp.first;

    std::vector<std::string> winningPorts;
    if (!activePorts.empty()) {
      const std::string winner = ownerOfPort[activePorts.front()];
      for (const std::string& pid : activePorts)
        if (ownerOfPort[pid] == winner) winningPorts.push_back(pid);
    }

    for (JsonObjectConst el : sys.elements)
      if (_eq(el["type"], "tool")) {
        const char* pid = el["id"].as<const char*>();
        if (pid) portIndex[pid] = el;
      }

    Routing r = computeRouting(sys, winningPorts);

    // CONFLICTS come from a second pass over every active port, because they
    // answer a different question: not "what opened" but "who wanted the same
    // selector as someone else". Filtering the losers out before the first pass
    // would report a shop where nothing ever contests anything, when contesting
    // a manifold stop is precisely the thing worth telling someone about.
    //
    // Only the conflicts are taken from it — states and reachability come from
    // the authoritative pass above. Cheap: a handful of elements, on tool on/off.
    //
    // A machine that lost the SYSTEM without contesting any selector (its gate
    // was free; it simply was not the winner) is deliberately not a conflict —
    // nothing was contested. machines[id].status == MACHINE_STRIPPED reports it.
    // Held in a NAMED local, not bound straight off the temporary. Lifetime
    // extension does not reach a member subobject through a conditional
    // operator, so `const auto& c = cond ? f().conflicts : r.conflicts;` reads a
    // destroyed vector — and would pass these tests anyway, right up until the
    // allocator reused the block. This file already has one use-after-free in
    // its history (see retire() in SmartOutletControl.cpp); one is enough.
    const bool someoneLost = activePorts.size() > winningPorts.size();
    Routing contestedPass;
    if (someoneLost) contestedPass = computeRouting(sys, activePorts);
    const std::vector<Conflict>& contested = someoneLost ? contestedPass.conflicts
                                                         : r.conflicts;

    // Losers are reported false, not left out. "We considered it and it lost"
    // and "we never heard of it" are different answers, and callers have always
    // been able to read reachable[portId] == false for the first.
    for (const std::string& pid : activePorts)
      if (r.reachable.find(pid) == r.reachable.end()) r.reachable[pid] = false;

    for (auto& kv : r.states)    out.states[kv.first]    = kv.second;
    for (auto& kv : r.reachable) out.reachable[kv.first] = kv.second;
    for (const Conflict& c : contested)
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
 * dead-head the sequencer exists to prevent. Execution is one move per BOARD
 * (TopologyRuntime::update, since 2026-10-08): the current budget belongs to a
 * board's supply, not to a duct run or to the shop, and a closing move still
 * waits for every opening move of its system (RFC §10.2).
 *
 * @param collectorRunning systemId → is that blower turning right now.
 */
inline std::vector<SystemPlan> planShopTransition(
    JsonObjectConst doc,
    const std::map<std::string, std::string>& currentStates,
    const std::map<std::string, std::string>& desiredStates,
    const std::map<std::string, bool>& collectorRunning,
    const std::set<std::string>* reassert = nullptr) {   // see planTransition()
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
                                         rit != collectorRunning.end() && rit->second,
                                         reassert);
    // Reported even with NO moves when the blower is at risk — an empty plan is
    // exactly how a system arrives at "everything shut while the fan runs".
    if (!plan.moves.empty() || plan.deadHeadRisk)
      out.push_back(SystemPlan{ sysId, plan.moves, plan.deadHeadRisk });
  }
  return out;
}

// IS THIS BOARD OPTIONAL — can the shop lose it without losing anything but a reading (2026-10-07, jeff;
// docs/optional-nodes-plan.md)? REQUIRED when the layout gives it any gate, or any of a collector's jobs (transmitter,
// clamp, bin beam); OPTIONAL otherwise, which is a board that only senses tools or one the layout does not use. An optional
// board that is off raises no problem: "off" is its normal state (the planer's board is powered with the planer), and its
// tools read as off meanwhile, the safe way round (RFC §5.6a). Derived, never chosen. The primary is never optional.
// MIRRORS isOptionalBoard() in shop.js — test_shop.cpp ↔ shop.test.js "optional boards", same cases.
// The layout's id for a PAIRED host: the controller whose id is that host, or whose link.host is. A brain knows its boards
// by host; a layout may name one differently and point at it, and missing that would call a board with gates optional.
inline std::string controllerIdForHost(JsonObjectConst doc, const std::string& host) {
  for (JsonObjectConst c : doc["controllers"].as<JsonArrayConst>()) {
    const char* id = c["id"] | "";
    const char* lh = c["link"]["host"] | "";
    if ((*id && sameBoard(id, host, "")) || (*lh && sameBoard(lh, host, ""))) return id;
  }
  return host;
}

inline bool isOptionalBoard(JsonObjectConst doc, const std::string& controllerId) {
  if (controllerId.empty()) return false;
  auto same = [&](JsonVariantConst v) {
    const char* id = v.as<const char*>();
    return id && *id && sameBoard(id, controllerId, "");
  };
  for (JsonObjectConst c : doc["controllers"].as<JsonArrayConst>())
    if (same(c["id"]) && _eq(c["role"], "primary")) return false;
  for (const SystemView& sys : systemsOf(doc))
    for (JsonObjectConst e : sys.elements) {
      if (_eq(e["type"], "selector") && same(e["controllerId"])) return false;
      if (!_eq(e["type"], "collector")) continue;
      if (same(e["control"]["rf"]["controllerId"]) || same(e["sensor"]["ct"]["controllerId"]) ||
          same(e["bin"]["sensor"]["controllerId"])) return false;
    }
  return true;
}

} // namespace topo

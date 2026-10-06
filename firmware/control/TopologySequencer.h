// =============================================================================
// TopologySequencer.h — transition sequencer (C++ port of sequencer.js).
//
// Routing (TopologyRouter.h) says WHERE every selector should end up. This says
// in WHAT ORDER to get there without ever sealing the system while the blower
// runs — never dead-head.
//
// Rule: open new paths BEFORE closing old ones ("make before break"). A servo
// move to a non-closed state is a "make"; to its closed state, a "break". A
// LINEAR selector maintains airflow through any move (sliding aperture), so it's
// never a break — it can move anytime. Moves are a serial list, so the
// one-servo-at-a-time current budget is honored implicitly.
//
// Idle-hold is a CALLER policy: when no tool is active the caller simply doesn't
// ask for a transition. We only sequence a transition the caller wants.
//
// PURE — ArduinoJson + STL only, NO Arduino.h, so it host-compiles for the
// conformance test alongside TopologyRouter.h.
// =============================================================================

#pragma once
#include <ArduinoJson.h>
#include "TopologyRouter.h"   // reuses topo::_eq, _closedState
#include <map>
#include <set>
#include <string>
#include <vector>

namespace topo {

struct Move {
  std::string selectorId;
  std::string toState;
  std::string kind;    // "linear" | "servoGate" | "servoManifold"
  bool        isBreak; // false = make (open first), true = break (close after)
};

struct TransitionPlan {
  std::vector<Move> moves;     // ordered: all makes first, then breaks
  bool deadHeadRisk = false;   // destination seals everything while blower runs
};

// currentStates: selectorId → current stateId (missing = unknown).
// desiredStates: selectorId → target stateId (from computeRouting().states).
//
// Selectors NOT named in `desiredStates` are left alone, which is what makes
// this safe to call per system with a shop-wide state map: the shop layer still
// narrows the maps (see planShopTransition in Shop.h) so `anyOpen` — and
// therefore deadHeadRisk — is judged against this blower's ducts only, but a
// stray foreign selector can't manufacture a move either way.
//
// reassert: selector ids to command even when we BELIEVE they are already at their
// target — planTransition()'s `opts.reassert` in sequencer.js, which says why.
// The caller fills it only on a machine's rising edge (TopologyRuntime.h), never
// on an ordinary tick. Servos only; a slider is never re-sent.
inline TransitionPlan planTransition(const SystemView& topology,
                                     const std::map<std::string, std::string>& currentStates,
                                     const std::map<std::string, std::string>& desiredStates,
                                     bool collectorRunning,
                                     const std::set<std::string>* reassert = nullptr) {
  TransitionPlan out;
  std::vector<Move> makes, breaks;
  bool anyOpen = false;

  for (JsonObjectConst sel : topology.elements) {
    if (!_eq(sel["type"], "selector")) continue;
    std::string id = _str(sel["id"]);

    auto dit = desiredStates.find(id);
    if (dit == desiredStates.end()) continue;      // not addressed → leave as-is
    const std::string& desired = dit->second;

    const char* closed = _closedState(sel);
    bool desiredIsClosed = closed && desired == closed;
    if (!desiredIsClosed) anyOpen = true;          // something ends up routing air

    const char* kind = sel["kind"].as<const char*>();
    bool isLinear = kind && strcmp(kind, "linear") == 0;
    const bool forced = reassert && reassert->count(id) && !isLinear;

    auto cit = currentStates.find(id);
    if (cit != currentStates.end() && cit->second == desired && !forced) continue;  // already there

    // Linear maintains flow through any move → never a break. A servo settling to
    // its closed state is the only "break" (it seals that path).
    bool isBreak = desiredIsClosed && !isLinear;
    Move m{ id, desired, kind ? std::string(kind) : std::string(), isBreak };
    (isBreak ? breaks : makes).push_back(m);
  }

  // Dead-head risk: the destination leaves nothing open while the blower runs.
  out.deadHeadRisk = collectorRunning && !anyOpen;
  out.moves.reserve(makes.size() + breaks.size());
  for (auto& m : makes)  out.moves.push_back(m);
  for (auto& m : breaks) out.moves.push_back(m);
  return out;
}

// Convenience for a plain single-system document.
inline TransitionPlan planTransition(JsonObjectConst topology,
                                     const std::map<std::string, std::string>& currentStates,
                                     const std::map<std::string, std::string>& desiredStates,
                                     bool collectorRunning,
                                     const std::set<std::string>* reassert = nullptr) {
  return planTransition(viewOf(topology), currentStates, desiredStates, collectorRunning, reassert);
}

} // namespace topo

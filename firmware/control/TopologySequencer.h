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
inline TransitionPlan planTransition(JsonObjectConst topology,
                                     const std::map<std::string, std::string>& currentStates,
                                     const std::map<std::string, std::string>& desiredStates,
                                     bool collectorRunning) {
  TransitionPlan out;
  std::vector<Move> makes, breaks;
  bool anyOpen = false;

  for (JsonObjectConst sel : topology["elements"].as<JsonArrayConst>()) {
    if (!_eq(sel["type"], "selector")) continue;
    std::string id = sel["id"].as<const char*>();

    auto dit = desiredStates.find(id);
    if (dit == desiredStates.end()) continue;      // not addressed → leave as-is
    const std::string& desired = dit->second;

    const char* closed = _closedState(sel);
    bool desiredIsClosed = closed && desired == closed;
    if (!desiredIsClosed) anyOpen = true;          // something ends up routing air

    auto cit = currentStates.find(id);
    if (cit != currentStates.end() && cit->second == desired) continue;  // already there

    const char* kind = sel["kind"].as<const char*>();
    bool isLinear = kind && strcmp(kind, "linear") == 0;
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

} // namespace topo

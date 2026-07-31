// =============================================================================
// test_topology_router.cpp — host conformance test for TopologyRouter.h.
//
// Cross-checks the C++ routing port against the JS engine (routing.js): the
// expected values below are the exact output of computeRouting() in Node on the
// same fixtures (see shared/device-model/topology.fixtures.js). Build + run:
//   c++ -std=c++17 -I <libdeps>/ArduinoJson \
//       linear_actuator/test/test_topology_router.cpp -o /tmp/trtest && /tmp/trtest
// (the tools/ script `firmware:router:test` wires the include path automatically.)
// =============================================================================

#include <ArduinoJson.h>
#include "../control/TopologyRouter.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int passed = 0, failed = 0;
static void ok(const char* name, bool cond, const std::string& detail = "") {
  printf("  %s %s%s\n", cond ? "✓" : "✗", name, cond || detail.empty() ? "" : ("  — " + detail).c_str());
  cond ? passed++ : failed++;
}

static std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss; ss << f.rdbuf();
  return ss.str();
}

// helpers to read a routing result
static std::string state(const topo::Routing& r, const std::string& sel) {
  auto it = r.states.find(sel); return it == r.states.end() ? "<none>" : it->second;
}
static bool reach(const topo::Routing& r, const std::string& tool) {
  auto it = r.reachable.find(tool); return it != r.reachable.end() && it->second;
}

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : "linear_actuator/test/fixtures/";

  DynamicJsonDocument star(16384), twoGates(16384), feedChain(16384);
  if (deserializeJson(star,      slurp(dir + "star.json"))      != DeserializationError::Ok) { printf("bad star.json\n"); return 2; }
  if (deserializeJson(twoGates,  slurp(dir + "twoGates.json"))  != DeserializationError::Ok) { printf("bad twoGates.json\n"); return 2; }
  if (deserializeJson(feedChain, slurp(dir + "feedChain.json")) != DeserializationError::Ok) { printf("bad feedChain.json\n"); return 2; }

  // ── star [toolB] → sel:s2, toolB reachable ────────────────────────────────
  {
    auto r = topo::computeRouting(star.as<JsonObjectConst>(), {"toolB"});
    ok("star [toolB]: sel=s2",       state(r, "sel") == "s2");
    ok("star [toolB]: toolB reaches", reach(r, "toolB"));
  }
  // ── star [toolA,toolB] contend on shared linear → A wins (s1), B loses ─────
  {
    auto r = topo::computeRouting(star.as<JsonObjectConst>(), {"toolA", "toolB"});
    ok("star contend: sel=s1 (A wins)", state(r, "sel") == "s1", "got " + state(r, "sel"));
    ok("star contend: toolA reaches",   reach(r, "toolA"));
    ok("star contend: toolB blocked",   !reach(r, "toolB"));
  }
  // ── twoGates [toolX,toolY] independent → both open, both reach ────────────
  {
    auto r = topo::computeRouting(twoGates.as<JsonObjectConst>(), {"toolX", "toolY"});
    ok("twoGates: gate1=open", state(r, "gate1") == "open");
    ok("twoGates: gate2=open", state(r, "gate2") == "open");
    ok("twoGates: both reach", reach(r, "toolX") && reach(r, "toolY"));
  }
  // ── feedChain [toolL] multi-hop linear→feed→manifold → lin:s2, man:left ────
  {
    auto r = topo::computeRouting(feedChain.as<JsonObjectConst>(), {"toolL"});
    ok("feedChain: lin=s2",   state(r, "lin") == "s2", "got " + state(r, "lin"));
    ok("feedChain: man=left", state(r, "man") == "left", "got " + state(r, "man"));
    ok("feedChain: toolL reaches", reach(r, "toolL"));
    // idle selectors default closed
    auto idle = topo::computeRouting(feedChain.as<JsonObjectConst>(), {});
    ok("feedChain idle: lin=home (closed)", state(idle, "lin") == "home", "got " + state(idle, "lin"));
    ok("feedChain idle: man=closed",        state(idle, "man") == "closed", "got " + state(idle, "man"));
  }
  // ── servoCommandAngle: twoGates gate1 open=ref+0, closed=ref+90 ───────────
  {
    JsonObjectConst g1;
    for (JsonObjectConst e : twoGates["elements"].as<JsonArrayConst>()) if (topo::_eq(e["id"], "gate1")) g1 = e;
    ok("servo angle: gate1 open = 10",   topo::servoCommandAngle(g1, "open") == 10, std::to_string(topo::servoCommandAngle(g1, "open")));
    ok("servo angle: gate1 closed = 100", topo::servoCommandAngle(g1, "closed") == 100, std::to_string(topo::servoCommandAngle(g1, "closed")));
  }

  printf("\n%d/%d passed%s\n", passed, passed + failed, failed ? (", " + std::to_string(failed) + " FAILED").c_str() : "");
  return failed ? 1 : 0;
}

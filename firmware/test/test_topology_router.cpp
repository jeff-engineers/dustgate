// =============================================================================
// test_topology_router.cpp — host conformance test for TopologyRouter.h.
//
// Cross-checks the C++ routing port against the JS engine (routing.js): the
// expected values below are the exact output of computeRouting() in Node on the
// same fixtures (see shared/device-model/topology.fixtures.js). Build + run:
//   c++ -std=c++17 -I <libdeps>/ArduinoJson \
//       firmware/test/test_topology_router.cpp -o /tmp/trtest && /tmp/trtest
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
  std::string dir = argc > 1 ? argv[1] : "firmware/test/fixtures/";

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

  // ── a MALFORMED document must not take the board down ─────────────────────
  //
  // TopologyStore::validateMinimal deliberately does not require an element to
  // have an `id` or a duct to have `parent`/`child` — the UI's validateShop() is
  // the authority and the device's gate is a cheap structural one. So a document
  // like this reaches the router, and before _str() every one of these lines was
  // a std::string built from a null const char*: undefined behaviour, and a
  // panic-reboot on the ESP32. Worse, the document is PERSISTED before it is
  // adopted, so the board came up and crashed again on every boot afterwards.
  //
  // These assertions are ordinary, but the test earns its place by not crashing:
  // run this against the old code and the binary dies here rather than failing.
  {
    DynamicJsonDocument bad(4096);
    auto err = deserializeJson(bad, R"({
      "schemaVersion": 1,
      "controllers": [{"id":"primary","role":"primary"}],
      "elements": [
        {"type":"collector","name":"nameless collector"},
        {"id":"gate","type":"selector","kind":"servoGate",
         "states":[{"id":"open","isClosed":false},{"id":"shut","isClosed":true}],
         "branches":[{"id":"b1","role":"tool"}]},
        {"id":"toolZ","type":"tool"}
      ],
      "ducts": [{"child":"toolZ","parentBranch":"b1"}, {"parent":"dc"}]
    })");
    ok("malformed: fixture parses", err == DeserializationError::Ok);
    auto r = topo::computeRouting(bad.as<JsonObjectConst>(), {"toolZ"});
    // The duct names no parent, so the walk is an orphan chain and stops.
    ok("malformed: unreachable, not a crash", !reach(r, "toolZ"));
    // The branch names no opensState, so the selector falls back to closed.
    ok("malformed: selector still closed",    state(r, "gate") == "shut",
       "got " + state(r, "gate"));
    // And an element with no id at all is simply keyed on "" rather than
    // dereferencing null on the way into the map.
    // An element with no id is not addressable at all, rather than being keyed
    // on "" — otherwise it and the parentless duct above both land on "" and
    // connect to each other.
    ok("malformed: nameless collector is not indexed under \"\"",
       topo::computeRouting(bad.as<JsonObjectConst>(), {}).states.count("") == 0);
    ok("malformed: idle pass survives too",
       topo::computeRouting(bad.as<JsonObjectConst>(), {}).states.count("gate") == 1);
  }

  printf("\n%d/%d passed%s\n", passed, passed + failed, failed ? (", " + std::to_string(failed) + " FAILED").c_str() : "");
  return failed ? 1 : 0;
}

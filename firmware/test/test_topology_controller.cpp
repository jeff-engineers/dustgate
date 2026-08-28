// =============================================================================
// test_topology_controller.cpp — host conformance for the Stage-3a control core.
//
// Drives topo::Controller through the same stateful power sequences as the JS
// device sim (shared/device-model/topology-device.js) and asserts identical
// results: active-tool routing, the make-before-break move plan (order + phase),
// dead-head detection, the idle-HOLD policy, and collector on/off. Expected
// values are the exact output of the JS sim on the same fixtures.
//
// Build + run via tools/ script `firmware:controller:test`.
// =============================================================================

#include <ArduinoJson.h>
#include "../control/TopologyController.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int passed = 0, failed = 0;
static void ok(const char* name, bool cond, const std::string& detail = "") {
  printf("  %s %s%s\n", cond ? "✓" : "✗", name,
         cond || detail.empty() ? "" : ("  — " + detail).c_str());
  cond ? passed++ : failed++;
}

static std::string slurp(const std::string& p) {
  std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

// Render a plan's moves as "sel->state(phase)|..." to match the JS ground truth.
static std::string movesStr(const topo::TransitionPlan& p) {
  std::string s;
  for (size_t i = 0; i < p.moves.size(); i++) {
    if (i) s += "|";
    s += p.moves[i].selectorId + "->" + p.moves[i].toState +
         "(" + (p.moves[i].isBreak ? "break" : "make") + ")";
  }
  return s;
}
static std::string stateOf(const topo::Controller& c, const std::string& sel) {
  auto& m = c.actuatorStates(); auto it = m.find(sel);
  return it == m.end() ? "<none>" : it->second;
}

int main(int argc, char** argv) {
  // These fixtures are schemaVersion-1 documents, so the shop layer sees one
  // implicit system under this id (Shop.h::kImplicitSystemId). The vectors below
  // are unchanged from the single-system era on purpose: v1 behaviour is the
  // contract the container must not have altered.
  const std::string kSys = topo::kImplicitSystemId;
  std::string dir = argc > 1 ? argv[1] : "firmware/test/fixtures/";
  DynamicJsonDocument twoGates(16384), star(16384);
  if (deserializeJson(twoGates, slurp(dir + "twoGates.json"))) { printf("bad twoGates.json\n"); return 2; }
  if (deserializeJson(star,     slurp(dir + "star.json")))     { printf("bad star.json\n");     return 2; }

  // ── twoGates: two independent servo gates ────────────────────────────────
  {
    topo::Controller c;
    c.setTopology(twoGates.as<JsonObjectConst>());

    auto r1 = c.setToolPower("toolX", 200);
    ok("twoGates x=200: move gate1 open (make)", movesStr(r1.planFor(kSys)) == "gate1->open(make)", movesStr(r1.planFor(kSys)));
    ok("twoGates x=200: gate1 state open", stateOf(c, "gate1") == "open");
    ok("twoGates x=200: gate2 state closed", stateOf(c, "gate2") == "closed");
    ok("twoGates x=200: collector on", c.collectorOn(kSys));
    ok("twoGates x=200: no dead-head", !r1.planFor(kSys).deadHeadRisk);

    // ONE MACHINE PER SYSTEM. These gates contest no selector, so both used to
    // open — co-open, half the velocity at each, the exact failure automated
    // gates exist to prevent. toolY is newer, so it takes the air and toolX is
    // left waiting even though its own gate was free.
    //
    // And the switchover is make-before-break without being asked: gate2 opens
    // BEFORE gate1 closes, so the blower is never pulling against a sealed
    // system. ↔ topology.test.js "dev X+Y on: only the NEWER tool gets a gate".
    auto r2 = c.setToolPower("toolY", 200);   // toolY newest
    ok("twoGates y=200: gate2 opens before gate1 closes",
       movesStr(r2.planFor(kSys)) == "gate2->open(make)|gate1->closed(break)", movesStr(r2.planFor(kSys)));
    ok("twoGates y=200: only the newer tool has a gate",
       stateOf(c, "gate1") == "closed" && stateOf(c, "gate2") == "open");
    ok("twoGates y=200: no dead-head", !r2.planFor(kSys).deadHeadRisk);

    auto r3 = c.setToolPower("toolX", 0);     // toolX off, toolY still on
    // Nothing to do: toolX already lost its gate when toolY won, so it stopping
    // changes no state at all.
    ok("twoGates x=0: nothing left to move", movesStr(r3.planFor(kSys)).empty(), movesStr(r3.planFor(kSys)));
    ok("twoGates x=0: gate1 closed, gate2 open", stateOf(c, "gate1") == "closed" && stateOf(c, "gate2") == "open");
    ok("twoGates x=0: collector still on", c.collectorOn(kSys));

    auto r4 = c.setToolPower("toolY", 0);     // all off
    ok("twoGates all-off: dead-head risk flagged", r4.planFor(kSys).deadHeadRisk);
    ok("twoGates all-off: collector off", !c.collectorOn(kSys));
    // Idle-HOLD: states are NOT driven closed — gate2 stays where it was.
    ok("twoGates all-off: gate2 HELD open (idle-hold)", stateOf(c, "gate2") == "open", stateOf(c, "gate2"));
  }

  // ── star: two tools contend on one shared LINEAR selector ────────────────
  {
    topo::Controller c;
    c.setTopology(star.as<JsonObjectConst>());

    auto rA = c.setToolPower("toolA", 200);
    ok("star A=200: move sel->s1 (make)", movesStr(rA.planFor(kSys)) == "sel->s1(make)", movesStr(rA.planFor(kSys)));
    ok("star A=200: toolA reachable", rA.routing.reachable.count("toolA") && rA.routing.reachable.at("toolA"));

    auto rB = c.setToolPower("toolB", 200);   // toolB newest → wins the shared linear
    ok("star B=200: move sel->s2 (make, linear never breaks)", movesStr(rB.planFor(kSys)) == "sel->s2(make)", movesStr(rB.planFor(kSys)));
    ok("star B=200: sel now s2", stateOf(c, "sel") == "s2");
    ok("star B=200: toolB reaches, toolA blocked",
       rB.routing.reachable.at("toolB") && !rB.routing.reachable.at("toolA"));

    auto rB0 = c.setToolPower("toolB", 0);    // toolB off → toolA regains the selector
    ok("star B=0: move sel->s1 (make)", movesStr(rB0.planFor(kSys)) == "sel->s1(make)", movesStr(rB0.planFor(kSys)));
    ok("star B=0: sel back to s1", stateOf(c, "sel") == "s1");
  }

  // ── plug identity → tool mapping ─────────────────────────────────────────
  {
    // twoGates fixtures have no outlet sensors; craft a tiny topology inline.
    DynamicJsonDocument d(2048);
    deserializeJson(d, R"({"schemaVersion":1,"controllers":[{"id":"p","role":"primary"}],
      "elements":[{"id":"dc","type":"collector"},
        {"id":"saw","type":"tool","sensor":{"outlet":{"host":"shelly-saw","ip":"10.0.0.5"}}}],
      "ducts":[{"child":"saw","parent":"dc"}]})");
    topo::Controller c; c.setTopology(d.as<JsonObjectConst>());
    ok("outlet map: host match", c.toolForOutlet("shelly-saw", "0.0.0.0") == "saw");
    ok("outlet map: ip fallback", c.toolForOutlet("", "10.0.0.5") == "saw");
    ok("outlet map: no match → empty", c.toolForOutlet("nope", "1.2.3.4").empty());
  }

  printf("\n%d/%d passed%s\n", passed, passed + failed,
         failed ? (", " + std::to_string(failed) + " FAILED").c_str() : "");
  return failed ? 1 : 0;
}

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
#include "../control/TopologyRuntime.h"   // the re-assert queue lives here
#include <set>
#include <vector>
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
// A bus that records what it was told and moves only when the test says so.
struct StubBus : public topo::ActuatorBus {
  bool moving = false;
  std::vector<std::string> log;
  bool online() const override { return true; }
  bool busy()   const override { return moving; }
  bool setState(const char* selectorId, JsonObjectConst, const char* stateId) override {
    log.push_back(std::string(selectorId) + "->" + stateId);
    moving = true;
    return true;
  }
};
static void drainRt(topo::TopologyRuntime& rt, StubBus& bus) {
  for (int i = 0; i < 50; i++) {
    rt.update(0);
    if (bus.moving) { bus.moving = false; continue; }
    if (!rt.transitioning()) { rt.update(0); break; }
  }
}
static bool logged(const StubBus& b, const std::string& m, size_t from = 0) {
  for (size_t i = from; i < b.log.size(); i++) if (b.log[i] == m) return true;
  return false;
}

static std::string stateOf(const topo::Controller& c, const std::string& sel) {
  auto& m = c.actuatorStates(); auto it = m.find(sel);
  return it == m.end() ? "<none>" : it->second;
}

int main(int argc, char** argv) {
  // The fixtures are shops whose one system is called this. The vectors below are unchanged from the single-system era on
  // purpose: that behaviour is the contract the container must not have altered.
  const std::string kSys = "system-1";
  std::string dir = argc > 1 ? argv[1] : "firmware/test/fixtures/";
  DynamicJsonDocument twoGates(16384), star(16384);
  if (deserializeJson(twoGates, slurp(dir + "twoGates.json"))) { printf("bad twoGates.json\n"); return 2; }
  if (deserializeJson(star,     slurp(dir + "starShop.json"))) { printf("bad starShop.json\n"); return 2; }

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
    deserializeJson(d, R"({"schemaVersion":2,"controllers":[{"id":"p","role":"primary"}],
      "systems":[{"id":"system-1","elements":[{"id":"dc","type":"collector"},
        {"id":"saw","type":"tool","machineId":"saw"}],
      "ducts":[{"child":"saw","parent":"dc"}]}],
      "machines":[{"id":"saw","name":"Saw","sensor":{"outlet":{"host":"shelly-saw","ip":"10.0.0.5"}}}]})");
    topo::Controller c; c.setTopology(d.as<JsonObjectConst>());
    ok("outlet map: host match", c.toolForOutlet("shelly-saw", "0.0.0.0") == "saw");
    ok("outlet map: ip fallback", c.toolForOutlet("", "10.0.0.5") == "saw");
    ok("outlet map: no match → empty", c.toolForOutlet("nope", "1.2.3.4").empty());
  }

  // ── sequencer: RE-ASSERT on a machine switching on (2026-09-28) ─────────
  // PAIR: topology.test.js's "re-assert" block — same cases, same order.
  {
    DynamicJsonDocument feed(16384);
    if (deserializeJson(feed, slurp(dir + "feedChainShop.json"))) { printf("bad feedChainShop.json\n"); return 2; }
    // Plan against the SYSTEM VIEW: both fixtures are v2 shops, and the
    // whole-document overload finds no selectors in one — which made the first
    // version of this block pass its "no move" case for the wrong reason.
    const topo::SystemView tg = topo::systemsOf(twoGates.as<JsonObjectConst>())[0];
    const topo::SystemView fc = topo::systemsOf(feed.as<JsonObjectConst>())[0];
    std::map<std::string, std::string> cur{{"gate1", "open"}, {"gate2", "closed"}};
    std::map<std::string, std::string> want = cur;
    ok("reassert: without it, a believed-closed gate gets no move",
       topo::planTransition(tg, cur, want, false).moves.empty());

    std::set<std::string> both{"gate1", "gate2"};
    topo::TransitionPlan p = topo::planTransition(tg, cur, want, false, &both);
    ok("reassert: both gates are commanded anyway", p.moves.size() == 2, movesStr(p));
    ok("reassert: the open is a make",  movesStr(p).find("gate1->open(make)") != std::string::npos);
    ok("reassert: the close is a break", movesStr(p).find("gate2->closed(break)") != std::string::npos);
    ok("reassert: still make-before-break", movesStr(p) == "gate1->open(make)|gate2->closed(break)", movesStr(p));

    std::set<std::string> one{"gate2"};
    topo::TransitionPlan q = topo::planTransition(tg, cur, want, false, &one);
    ok("reassert: only the named selectors", movesStr(q) == "gate2->closed(break)", movesStr(q));

    std::map<std::string, std::string> fcur{{"lin", "s1"}, {"man", "closed"}};
    std::set<std::string> fall{"lin", "man"};
    topo::TransitionPlan l = topo::planTransition(fc, fcur, fcur, false, &fall);
    ok("reassert: a slider is never re-sent", movesStr(l).find("lin->") == std::string::npos, movesStr(l));
  }

  // ── runtime: the re-assert QUEUE (C++ only — the JS sim has no queue) ───
  // The scenario that asked for it: the jointer runs, stops (idle HOLDS the
  // gates), someone opens the table-saw gate by hand, the jointer starts again.
  {
    std::string shopJson = slurp(dir + "twoSystemShop.json");
    StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    rt.begin(&nb);
    std::string err;
    ok("runtime reassert: adopt twoSystemShop", rt.adopt(shopJson.c_str(), shopJson.size(), err), err);

    rt.setMachinePower("jointer", 500); drainRt(rt, local);
    rt.setMachinePower("jointer", 0);   drainRt(rt, local);     // idle: held
    size_t mark = local.log.size();
    rt.setMachinePower("jointer", 500);                        // on again
    // A second, ordinary reading before anything has run — ingest() REBUILDS
    // the queue on every tick, and the re-asserts must survive that.
    rt.setMachinePower("jointer", 510);
    drainRt(rt, local);
    ok("runtime reassert: the table-saw gate is re-closed on switch-on",
       logged(local, "bv-cab->closed", mark));
    ok("runtime reassert: the jointer's own gate is re-sent open",
       logged(local, "bv-jnt->open", mark));

    size_t mark2 = local.log.size();
    rt.setMachinePower("jointer", 520); drainRt(rt, local);    // not an edge
    ok("runtime reassert: a repeat reading moves nothing", local.log.size() == mark2);

  }
  // THE BLOWER STARTS ONCE THE OPENS HAVE LANDED, not once every close has.
  // A FRESH runtime: after a run the blower is coasting and reads as on.
  {
    std::string shopJson = slurp(dir + "twoSystemShop.json");
    StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    rt.begin(&nb);
    std::string err;
    rt.adopt(shopJson.c_str(), shopJson.size(), err);
    rt.setMachinePower("jointer", 500);
    rt.update(0);                                  // issues the make (bv-jnt open)
    ok("runtime reassert: blower waits while the open is moving", !rt.collectorOn("big"));
    local.moving = false; rt.update(0);            // make landed; the break is issued
    ok("runtime reassert: blower starts with a close still in flight",
       rt.collectorOn("big") && rt.transitioning());
  }

  printf("\n%d/%d passed%s\n", passed, passed + failed,
         failed ? (", " + std::to_string(failed) + " FAILED").c_str() : "");
  return failed ? 1 : 0;
}

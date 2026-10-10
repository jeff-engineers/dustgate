#include <cstring>
// test_deviceproblems.cpp — host tests for control/DeviceProblems.h: when a dead board or a silent plug becomes a problem
// a person sees, and when it stops being one. Run from the repo root.
#include "../control/DeviceProblems.h"
#include <cstdio>
#include <fstream>
#include <sstream>
using namespace topo;
static int passed = 0, failed = 0;
static void ok(const char* what, bool c) { if (c) { printf("  ok   %s\n", what); passed++; } else { printf("  FAIL %s\n", what); failed++; } }
struct NullBus : ActuatorBus { bool online() const override { return true; } bool busy() const override { return false; }
  bool setState(const char*, JsonObjectConst, const char*) override { return true; } };
// Records every command it is given, so a test can see a gate being sent again.
struct RecBus : ActuatorBus { std::vector<std::string> log;
  bool online() const override { return true; } bool busy() const override { return false; }
  bool setState(const char* id, JsonObjectConst, const char* st) override { log.push_back(std::string(id) + "->" + st); return true; } };
static int count(const RecBus& b, const std::string& m) { int n = 0; for (auto& l : b.log) if (l == m) n++; return n; }
static TopologyRuntime* load(NodeBus& nb) {
  static NullBus nul; nb.setLocal(&nul, "primary");
  std::ifstream f("firmware/test/fixtures/twoGates.json"); std::stringstream b; b << f.rdbuf();
  TopologyRuntime* rt = new TopologyRuntime(); rt->begin(&nb);
  std::string err; if (!rt->adopt(b.str().data(), b.str().size(), err)) { printf("fixture: %s\n", err.c_str()); exit(2); }
  return rt;
}
int main() {
  printf("\nP1 boards\n");
  { NodeBus nb; TopologyRuntime* rt = load(nb); DeviceProblems dp;
    BoardView b; b.host = "n1"; b.linked = false; b.downForMs = 5000;
    dp.update(*rt, {b}, {}, 100000);
    ok("a board down for a few seconds is not yet a problem", !rt->hasProblem("board:n1"));
    b.downForMs = kBoardOfflineAfterMs;
    dp.update(*rt, {b}, {}, 100000);
    ok("past the grace it is board-offline", rt->hasProblem("board:n1"));
    b.linked = true; b.downForMs = 0;
    dp.update(*rt, {b}, {}, 102000);
    ok("it clears the moment the board is back", !rt->hasProblem("board:n1"));
    BoardView r; r.host = "n2"; r.refused = true;
    dp.update(*rt, {r}, {}, 104000);
    ok("a board that refused us is a problem at once, even though it answers", rt->hasProblem("board:n2"));
    BoardView m; m.host = "n3"; m.linked = true; m.moveFault = "The link dropped mid-move";
    dp.update(*rt, {m}, {}, 106000);
    ok("a move that did not finish is its own problem on a linked board", rt->hasProblem("move:n3"));
    m.moveFault = nullptr;
    dp.update(*rt, {m}, {}, 108000);
    ok("...cleared by the next move", !rt->hasProblem("move:n3"));
    delete rt; }
  printf("\nP1a an OPTIONAL board that is off is not a problem (docs/optional-nodes-plan.md)\n");
  { NodeBus nb; TopologyRuntime* rt = load(nb); DeviceProblems dp;
    BoardView b; b.host = "planer"; b.linked = false; b.downForMs = 10 * kBoardOfflineAfterMs; b.optional = true;
    dp.update(*rt, {b}, {}, 100000);
    ok("an optional board down for minutes raises nothing", !rt->hasProblem("board:planer"));
    b.optional = false;
    dp.update(*rt, {b}, {}, 102000);
    ok("the same board, required, is board-offline", rt->hasProblem("board:planer"));
    b.optional = true;
    dp.update(*rt, {b}, {}, 104000);
    ok("...and the problem goes when the layout makes it optional", !rt->hasProblem("board:planer"));
    BoardView r; r.host = "planer2"; r.refused = true; r.optional = true;
    dp.update(*rt, {r}, {}, 106000);
    ok("an optional board that REFUSED us is still a problem: it answered", rt->hasProblem("board:planer2"));
    delete rt; }
  printf("\nP1b an interrupted move is sent again\n");
  { NodeBus nb; RecBus bus; nb.setLocal(&bus, "primary");
    std::ifstream f("firmware/test/fixtures/twoGates.json"); std::stringstream b; b << f.rdbuf();
    TopologyRuntime rt; rt.begin(&nb);
    std::string err; if (!rt.adopt(b.str().data(), b.str().size(), err)) { printf("fixture: %s\n", err.c_str()); return 2; }
    DeviceProblems dp;
    rt.setMachinePower("toolX", 200);
    for (int i = 0; i < 5; i++) rt.update(1000 * (i + 1));
    ok("toolX switching on opens gate1, then closes gate2 (the last move issued)",
       count(bus, "gate1->open") == 1 && count(bus, "gate2->closed") == 1);

    BoardView m; m.host = "primary"; m.linked = true; m.moveFault = "The board restarted mid-move";
    dp.update(rt, {m}, {}, 10000);
    for (int i = 0; i < 5; i++) rt.update(10000 + 1000 * (i + 1));
    ok("a move that did not finish: the gate it was moving is commanded again", count(bus, "gate2->closed") == 2);
    ok("...and only that gate", count(bus, "gate1->open") == 1);

    dp.update(rt, {m}, {}, 20000);
    for (int i = 0; i < 5; i++) rt.update(20000 + 1000 * (i + 1));
    ok("the same fault, still up, is NOT sent again (one fault, one re-send)", count(bus, "gate2->closed") == 2);

    m.moveFault = nullptr; dp.update(rt, {m}, {}, 30000);
    m.moveFault = "The link dropped mid-move"; dp.update(rt, {m}, {}, 31000);
    for (int i = 0; i < 5; i++) rt.update(31000 + 1000 * (i + 1));
    ok("a NEW fault after it cleared is sent again", count(bus, "gate2->closed") == 3); }
  printf("\nP2 plugs\n");
  { NodeBus nb; TopologyRuntime* rt = load(nb); DeviceProblems dp;
    PlugView p; p.key = "plug:saw"; p.name = "Table Saw"; p.ip = "10.0.0.5"; p.reachable = false;
    dp.update(*rt, {}, {p}, 100000);
    ok("a plug just seen silent is not a problem", !rt->hasProblem("plug:saw"));
    dp.update(*rt, {}, {p}, 100000 + kPlugDownAfterMs - 1);
    ok("...nor just inside the window", !rt->hasProblem("plug:saw"));
    dp.update(*rt, {}, {p}, 100000 + kPlugDownAfterMs);
    ok("...but is at two minutes", rt->hasProblem("plug:saw"));
    p.reachable = true;
    dp.update(*rt, {}, {p}, 100000 + kPlugDownAfterMs + 500);
    ok("it clears when the plug answers", !rt->hasProblem("plug:saw"));
    p.reachable = false;
    dp.update(*rt, {}, {p}, 400000);
    dp.update(*rt, {}, {p}, 400000 + kPlugDownAfterMs - 1);
    ok("a plug that dropped out again gets a FRESH window, not the old clock", !rt->hasProblem("plug:saw"));
    delete rt; }
  printf("\nP3 a clamp the layout uses, unplugged from its jack (2026-10-09)\n");
  { static const char* kShop = R"({"schemaVersion":2,
      "controllers":[{"id":"primary","role":"primary"},{"id":"n2","role":"secondary"},{"id":"n3","role":"secondary"}],
      "systems":[{"id":"s1","elements":[
        {"id":"dc","type":"collector","sensor":{"ct":{"controllerId":"primary","channel":0}}},
        {"id":"gate1","type":"selector","controllerId":"n3","kind":"servoGate",
         "states":[{"id":"open","isClosed":false,"offsetDeg":0},{"id":"closed","isClosed":true,"offsetDeg":90}],
         "branches":[{"id":"g1","opensState":"open","role":"tool"}],"servo":{"channel":0,"referenceAngle":10}},
        {"id":"p1","type":"tool","machineId":"planer"}],
       "ducts":[{"child":"gate1","parent":"dc"},{"child":"p1","parent":"gate1","parentBranch":"g1"}]}],
      "machines":[{"id":"planer","name":"Planer","sensor":{"ct":{"controllerId":"n2","channel":0}}}]})";
    NodeBus nb; static NullBus nul; nb.setLocal(&nul, "primary");
    TopologyRuntime rt; rt.begin(&nb);
    std::string err; if (!rt.adopt(kShop, strlen(kShop), err)) { printf("fixture: %s\n", err.c_str()); return 2; }
    DeviceProblems dp;
    BoardView n2; n2.host = "n2"; n2.linked = true;
    dp.update(rt, {n2}, {}, 1000);
    ok("a board that never said (old firmware) raises nothing", !rt.hasProblem("clamp:n2"));
    n2.clampIn = 1; dp.update(rt, {n2}, {}, 2000);
    ok("a clamp plugged in raises nothing", !rt.hasProblem("clamp:n2"));
    n2.clampIn = 0; dp.update(rt, {n2}, {}, 3000);
    ok("the planer's clamp unplugged is clamp-unplugged", rt.hasProblem("clamp:n2"));
    n2.linked = false; dp.update(rt, {n2}, {}, 4000);
    ok("a board that is off says nothing about its jack", !rt.hasProblem("clamp:n2"));
    n2.linked = true; n2.clampIn = 1; dp.update(rt, {n2}, {}, 5000);
    ok("plugged back in, it clears", !rt.hasProblem("clamp:n2"));
    BoardView n3; n3.host = "n3"; n3.linked = true; n3.clampIn = 0;
    dp.update(rt, {n3}, {}, 6000);
    ok("an empty jack on a board the layout uses no clamp on is nobody's problem", !rt.hasProblem("clamp:n3"));
    BoardView me; me.host = "dustgate"; me.self = true; me.linked = true; me.clampIn = 0;
    dp.update(rt, {me}, {}, 7000);
    ok("the brain's OWN jack: the collector's clamp unplugged", rt.hasProblem("clamp:dustgate"));
  }
  printf("\n%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}

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
  printf("\n%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}

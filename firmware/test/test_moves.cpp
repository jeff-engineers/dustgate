// =============================================================================
// test_moves.cpp — how the runtime schedules gate moves and starts a blower (TopologyRuntime::update, 2026-10-08).
//
//   • a boot settle opens ONE gate per system and commands every other one closed, once its boards are linked
//   • a tool switched on before the settle has run takes the system over: no settle moves follow
//   • a blower starts AT ONCE when its system already has an open gate, while the new one is still opening
//   • ...and still waits for the opening move when the system is sealed (never dead-head)
//
// C++ only: the JS model applies a plan at once and has no boards to wait for. One move per board and
// make-before-break across boards are pinned in test_nodebus.cpp ("a busy board does not hold up...").
// Build + run via tools/ script `firmware:moves:test`.
// =============================================================================
#include <ArduinoJson.h>
#include "../control/TopologyRuntime.h"
#include <cstdio>
#include <string>
#include <vector>

static int passed = 0, failed = 0;
static void ok(const char* name, bool cond, const std::string& detail = "") {
  printf("  %s %s%s\n", cond ? "✓" : "✗", name, cond || detail.empty() ? "" : ("  — " + detail).c_str());
  cond ? passed++ : failed++;
}

struct StubBus : public topo::ActuatorBus {
  bool up = true, moving = false;
  std::vector<std::string> log;
  bool online() const override { return up; }
  bool busy()   const override { return moving; }
  bool setState(const char* selectorId, JsonObjectConst, const char* stateId) override {
    log.push_back(std::string(selectorId) + "->" + stateId);
    moving = true;
    return true;
  }
  void settle() { moving = false; }
};
static std::string joined(const std::vector<std::string>& v) { std::string s; for (auto& x : v) s += (s.empty() ? "" : ",") + x; return s; }

// One system, two gates on two boards: gate1 on the brain (toolX), gate2 on node2 (toolY). A collector with no
// control block, so collectorOn() is the runtime's decision alone.
static const char* kShop = R"({"schemaVersion":2,
  "controllers":[{"id":"primary","role":"primary"},{"id":"node2","role":"secondary"}],
  "systems":[{"id":"s1","elements":[
    {"id":"dc","type":"collector"},
    {"id":"gate1","type":"selector","controllerId":"primary","kind":"servoGate",
     "states":[{"id":"open","isClosed":false,"offsetDeg":0},{"id":"closed","isClosed":true,"offsetDeg":90}],
     "branches":[{"id":"g1","opensState":"open","role":"tool"}],"servo":{"channel":0,"referenceAngle":10}},
    {"id":"gate2","type":"selector","controllerId":"node2","kind":"servoGate",
     "states":[{"id":"open","isClosed":false,"offsetDeg":0},{"id":"closed","isClosed":true,"offsetDeg":90}],
     "branches":[{"id":"g2","opensState":"open","role":"tool"}],"servo":{"channel":0,"referenceAngle":10}},
    {"id":"toolX","type":"tool","machineId":"toolX"},{"id":"toolY","type":"tool","machineId":"toolY"}],
   "ducts":[{"child":"gate1","parent":"dc"},{"child":"gate2","parent":"dc"},
            {"child":"toolX","parent":"gate1","parentBranch":"g1"},{"child":"toolY","parent":"gate2","parentBranch":"g2"}]}],
  "machines":[{"id":"toolX","name":"X"},{"id":"toolY","name":"Y"}]})";

struct Rig {
  StubBus local, node2; topo::NodeBus nb; topo::TopologyRuntime rt;
  Rig() {
    nb.setLocal(&local, "primary"); nb.registerRemote("node2", &node2); rt.begin(&nb);
    std::string err; rt.adopt(kShop, strlen(kShop), err);
  }
  void pump(int n = 4) { for (int i = 0; i < n; i++) rt.update(1000); }
  void land() { local.settle(); node2.settle(); }
  std::string gate(const char* id) const {
    auto it = rt.actuatorStates().find(id); return it == rt.actuatorStates().end() ? "?" : it->second;
  }
};

int main() {
  // ── settle at boot ─────────────────────────────────────────────────────────
  {
    Rig r; r.node2.up = false;
    r.rt.settleAtBoot();
    r.pump();
    ok("a settle waits for every board of its system to be linked", r.local.log.empty() && r.node2.log.empty(),
       joined(r.local.log) + " | " + joined(r.node2.log));
    r.node2.up = true;
    r.pump();
    ok("then it opens the path to the system's first machine", joined(r.local.log) == "gate1->open", joined(r.local.log));
    ok("...and holds the closes until that open has landed", r.node2.log.empty(), joined(r.node2.log));
    r.local.settle(); r.pump();
    ok("then every other gate is COMMANDED closed, whatever we believed", joined(r.node2.log) == "gate2->closed", joined(r.node2.log));
    r.land(); r.pump();
    ok("and nothing more moves", r.local.log.size() == 1 && r.node2.log.size() == 1);
    ok("the blower stays off: a settle is not a run", !r.rt.collectorOn("s1"));
  }
  {
    Rig r; r.node2.up = false;
    r.rt.settleAtBoot();
    r.pump();
    r.rt.setMachinePower("toolY", 200);    // a tool before the settle could run: the system is routing's now
    r.node2.up = true;
    for (int i = 0; i < 6; i++) { r.pump(1); r.land(); }
    ok("a tool switched on first takes the system: its gate opens", r.gate("gate2") == "open");
    bool settleOpenedGate1 = false;
    for (auto& l : r.local.log) if (l == "gate1->open") settleOpenedGate1 = true;
    ok("...and the settle never runs after it", !settleOpenedGate1, joined(r.local.log));
  }

  // ── when the blower starts ─────────────────────────────────────────────────
  {
    Rig r;
    r.rt.settleAtBoot();
    for (int i = 0; i < 6; i++) { r.pump(1); r.land(); }   // gate1 open, gate2 closed
    r.local.log.clear(); r.node2.log.clear();
    r.rt.setMachinePower("toolY", 200);                     // wants gate2 open (node2), then gate1 closed
    r.rt.update(1000);
    ok("the new gate's open is under way", joined(r.node2.log) == "gate2->open", joined(r.node2.log));
    ok("with gate1 already open, the blower starts AT ONCE", r.rt.collectorOn("s1"));
    ok("...and gate1 stays open until gate2 has landed", r.local.log.empty(), joined(r.local.log));
    r.node2.settle(); r.pump();
    ok("then gate1 closes", joined(r.local.log) == "gate1->closed", joined(r.local.log));
  }
  {
    Rig r;                                                  // no settle: every gate believed closed — SEALED
    r.rt.setMachinePower("toolX", 200);
    r.rt.update(1000);
    ok("a sealed system's blower waits for its open", !r.rt.collectorOn("s1") && joined(r.local.log) == "gate1->open",
       joined(r.local.log));
    r.local.settle(); r.pump();
    ok("...and starts once it has landed", r.rt.collectorOn("s1"));
  }

  // ── the [TOOL] line ────────────────────────────────────────────────────────
  {
    static std::vector<std::string> said;
    Rig r; r.rt.setSay([](const std::string& l) { said.push_back(l); });
    r.rt.setMachinePower("toolX", 812);
    r.rt.setMachinePower("toolX", 815);                     // still on: no second line
    r.rt.setMachinePower("toolX", 0);
    r.rt.setMachineManual("toolY", true);
    ok("a tool crossing its trip point says so, with its watts", said.size() >= 1 && said[0] == "[TOOL] X on (812 W)",
       said.empty() ? "" : said[0]);
    ok("...once, not on every reading", said.size() >= 2 && said[1] == "[TOOL] X off", said.size() >= 2 ? said[1] : "");
    ok("a hand switch says it was by hand", said.size() >= 3 && said[2] == "[TOOL] Y on (by hand)", said.size() >= 3 ? said[2] : "");
  }

  printf("%d/%d passed\n", passed, passed + failed);
  return failed ? 1 : 0;
}

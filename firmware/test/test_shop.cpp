// =============================================================================
// test_shop.cpp — host conformance test for Shop.h.
//
// Cross-checks the C++ shop layer against the JS one (shared/device-model/shop.js):
// the expected values below are the exact output of routeShop() and
// planShopTransition() in Node on the same fixture — firmware/test/fixtures/
// twoSystemShop.json IS topology.fixtures.js's `twoSystemShop`, serialized, so
// the two engines are genuinely answering the same question.
//
// The fixture is the case the whole container exists for: a 4" system ("big",
// two servo ball valves) and a 2.5" system ("small", one linear manifold) under
// one brain, with a table saw whose cabinet port lives in one and whose overarm
// pickup lives in the other. The overarm is marked `supplemental`, which is what
// makes losing it "partial" rather than the alarm.
//
// Build + run:
//   c++ -std=c++17 -I <libdeps>/ArduinoJson \
//       firmware/test/test_shop.cpp -o /tmp/shoptest && /tmp/shoptest
// (the tools/ script `firmware:shop:test` wires the include path automatically.)
// =============================================================================

#include <ArduinoJson.h>
#include "../control/Shop.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int passed = 0, failed = 0;
static void ok(const char* name, bool cond, const std::string& detail = "") {
  printf("  %s %s%s\n", cond ? "✓" : "✗", name,
         cond || detail.empty() ? "" : ("  — " + detail).c_str());
  cond ? passed++ : failed++;
}

static std::string slurp(const std::string& p) {
  std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

static std::string state(const topo::ShopRouting& r, const std::string& sel) {
  auto it = r.states.find(sel); return it == r.states.end() ? "<none>" : it->second;
}
static bool reach(const topo::ShopRouting& r, const std::string& port) {
  auto it = r.reachable.find(port); return it != r.reachable.end() && it->second;
}
static std::string status(const topo::ShopRouting& r, const std::string& machine) {
  auto it = r.machines.find(machine);
  return it == r.machines.end() ? "<absent>" : topo::machineStatusName(it->second.status);
}
static std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); i++) { if (i) s += ","; s += v[i]; }
  return s;
}
// Render one system's plan as "sel->state(phase)|..." to match the JS ground truth.
static std::string movesStr(const std::vector<topo::SystemPlan>& plans, const std::string& sysId) {
  for (const topo::SystemPlan& p : plans) {
    if (p.systemId != sysId) continue;
    std::string s;
    for (size_t i = 0; i < p.moves.size(); i++) {
      if (i) s += "|";
      s += p.moves[i].selectorId + "->" + p.moves[i].toState +
           "(" + (p.moves[i].isBreak ? "break" : "make") + ")";
    }
    return s;
  }
  return "<no plan>";
}
static bool risk(const std::vector<topo::SystemPlan>& plans, const std::string& sysId) {
  for (const topo::SystemPlan& p : plans) if (p.systemId == sysId) return p.deadHeadRisk;
  return false;
}

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : "firmware/test/fixtures/";

  DynamicJsonDocument shopDoc(32768), twoGates(16384);
  if (deserializeJson(shopDoc, slurp(dir + "twoSystemShop.json"))) { printf("bad twoSystemShop.json\n"); return 2; }
  if (deserializeJson(twoGates, slurp(dir + "twoGates.json")))     { printf("bad twoGates.json\n");     return 2; }
  JsonObjectConst shop = shopDoc.as<JsonObjectConst>();
  JsonObjectConst v1   = twoGates.as<JsonObjectConst>();

  // ── shape detection + the v1 lens ─────────────────────────────────────────
  {
    ok("shop detected as a shop", topo::isShop(shop));
    ok("v1 topology is NOT a shop", !topo::isShop(v1));

    auto sysShop = topo::systemsOf(shop);
    ok("shop has two systems", sysShop.size() == 2);
    ok("systems in document order", sysShop.size() == 2 &&
       std::string(sysShop[0].id) == "big" && std::string(sysShop[1].id) == "small");

    auto sysV1 = topo::systemsOf(v1);
    ok("v1 yields one implicit system", sysV1.size() == 1);
    ok("implicit system id", sysV1.size() == 1 &&
       std::string(sysV1[0].id) == topo::kImplicitSystemId);
    // The whole of the v1 compatibility story: a tool element IS its machine, so
    // nothing above this layer needs a second code path.
    ok("v1 machines are its tool elements", join(topo::machineIds(v1)) == "toolX,toolY",
       join(topo::machineIds(v1)));
    ok("v1 machineDoc is the tool element itself",
       topo::_eq(topo::machineDoc(v1, "toolX")["type"], "tool"));
  }

  // ── machines and ports ────────────────────────────────────────────────────
  {
    auto ports = topo::portsByMachine(shop);
    // The reason machines exist: one thing you switch on, two ports, two systems.
    ok("table saw has two ports", ports["table-saw"].size() == 2);
    ok("saw port 1 is in the 4\" system", ports["table-saw"][0].systemId == "big");
    ok("saw port 2 is in the 2.5\" system", ports["table-saw"][1].systemId == "small");
    ok("machineIdOf reads the explicit link",
       topo::machineIdOf(ports["table-saw"][0].port) == "table-saw");
    ok("enabled is opt-out", topo::portEnabled(ports["jointer"][0].port));
  }

  // ── routing: one machine, two systems ─────────────────────────────────────
  // A single machineId opens a ball valve on one blower's ducts and moves a
  // manifold on the other's. This is the case a flat topology cannot express.
  {
    auto r = topo::routeShop(shop, {"table-saw"});
    ok("saw alone: cabinet valve open",   state(r, "bv-cab") == "open",   state(r, "bv-cab"));
    ok("saw alone: jointer valve closed", state(r, "bv-jnt") == "closed", state(r, "bv-jnt"));
    ok("saw alone: manifold at m1",       state(r, "man")    == "m1",     state(r, "man"));
    ok("saw alone: cabinet port reaches", reach(r, "ts-cabinet"));
    ok("saw alone: overarm port reaches", reach(r, "ts-overarm"));
    ok("saw alone: machine routed",       status(r, "table-saw") == "routed", status(r, "table-saw"));
    ok("saw alone: no conflicts",         r.conflicts.empty());
  }

  // ── contention across systems → PARTIAL, not stripped ─────────────────────
  // The drill press takes the manifold the saw's overarm wanted. The saw keeps
  // its cabinet air, so this degrades capture rather than being the alarm.
  {
    auto r = topo::routeShop(shop, {"drill-press", "table-saw"});
    ok("contend: manifold to the drill press", state(r, "man") == "m2", state(r, "man"));
    ok("contend: cabinet still open",          state(r, "bv-cab") == "open");
    ok("contend: cabinet port reaches",        reach(r, "ts-cabinet"));
    ok("contend: overarm port blocked",        !reach(r, "ts-overarm"));
    ok("contend: drill port reaches",          reach(r, "drill-port"));
    // The three-answer verdict. `partial` is only reachable because the overarm
    // is marked supplemental — see the stripped case below.
    ok("contend: saw is PARTIAL",              status(r, "table-saw") == "partial", status(r, "table-saw"));
    ok("contend: drill press is routed",       status(r, "drill-press") == "routed");
    ok("contend: saw kept the cabinet",        join(r.machines["table-saw"].routed) == "ts-cabinet",
       join(r.machines["table-saw"].routed));
    ok("contend: saw lost the overarm",        join(r.machines["table-saw"].blocked) == "ts-overarm",
       join(r.machines["table-saw"].blocked));
    ok("contend: one conflict",                r.conflicts.size() == 1);
    // Tagged with the system, because "man is contested" doesn't say which duct
    // run to walk over to once there is more than one.
    ok("conflict names its system",            r.conflicts.size() == 1 && r.conflicts[0].systemId == "small",
       r.conflicts.empty() ? "" : r.conflicts[0].systemId);
    ok("conflict names the winning PORT",      r.conflicts.size() == 1 && r.conflicts[0].winner == "drill-port");
    ok("conflict names the loser",             r.conflicts.size() == 1 && join(r.conflicts[0].losers) == "ts-overarm");
  }

  // ── arbitration: primary beats supplemental (RFC §11.3) ───────────────────
  // The saw is listed FIRST — it started more recently — so plain
  // most-recent-wins would hand the manifold to its overarm and leave the drill
  // press, whose ONLY port that manifold feeds, running into a closed gate.
  // Rule 1: never trade someone's only collection for someone else's bonus.
  {
    auto r = topo::routeShop(shop, {"table-saw", "drill-press"});
    ok("primary holds the manifold despite starting earlier", state(r, "man") == "m2", state(r, "man"));
    ok("the drill press keeps its air",        reach(r, "drill-port"));
    ok("the newer machine yields its bonus",   !reach(r, "ts-overarm"));
    ok("the yielding machine is only partial", status(r, "table-saw") == "partial", status(r, "table-saw"));
    ok("nobody is stripped",                   status(r, "drill-press") == "routed");
    ok("conflict names the primary as winner", r.conflicts.size() == 1 && r.conflicts[0].winner == "drill-port");
  }

  // ── the other system is untouched by the first ────────────────────────────
  {
    auto r = topo::routeShop(shop, {"jointer"});
    ok("jointer: its own valve opens",   state(r, "bv-jnt") == "open");
    ok("jointer: cabinet valve closes",  state(r, "bv-cab") == "closed");
    // The 2.5" manifold goes home rather than being dragged somewhere by a
    // machine on the other blower — the container earning its keep.
    ok("jointer: manifold idles home",   state(r, "man") == "home", state(r, "man"));
    ok("jointer: routed",                status(r, "jointer") == "routed");
  }

  // ── planning: concatenated per system, dead-head judged per blower ─────────
  {
    std::map<std::string, std::string> cur = {{"bv-cab","closed"},{"bv-jnt","closed"},{"man","home"}};
    auto des = topo::routeShop(shop, {"drill-press", "table-saw"}).states;
    auto plans = topo::planShopTransition(shop, cur, des, {{"big",true},{"small",false}});
    ok("plan: one entry per system with work", plans.size() == 2);
    ok("plan: big system order",   plans.size() == 2 && plans[0].systemId == "big");
    ok("plan: small system second", plans.size() == 2 && plans[1].systemId == "small");
    ok("plan: big opens the cabinet valve", movesStr(plans, "big") == "bv-cab->open(make)",
       movesStr(plans, "big"));
    ok("plan: small moves the manifold",    movesStr(plans, "small") == "man->m2(make)",
       movesStr(plans, "small"));
    ok("plan: no dead-head on big",   !risk(plans, "big"));
    ok("plan: no dead-head on small", !risk(plans, "small"));
  }

  // Both blowers running, everything asked to close: each is at risk on its own
  // account. Two answers, not one — collapsing them would hide which blower to
  // shut down.
  {
    std::map<std::string, std::string> cur = {{"bv-cab","open"},{"bv-jnt","closed"},{"man","m1"}};
    auto des = topo::routeShop(shop, {}).states;
    auto plans = topo::planShopTransition(shop, cur, des, {{"big",true},{"small",true}});
    ok("idle: dead-head flagged on big",   risk(plans, "big"));
    ok("idle: dead-head flagged on small", risk(plans, "small"));
    ok("idle: big closes its valve", movesStr(plans, "big") == "bv-cab->closed(break)",
       movesStr(plans, "big"));
    // Linear maintains flow through any move, so returning home is never a break.
    ok("idle: linear home is a make, not a break", movesStr(plans, "small") == "man->home(make)",
       movesStr(plans, "small"));
  }

  // Only the running blower is at risk. Same destination as above; the small
  // system is simply off, so it has nothing to seal.
  {
    std::map<std::string, std::string> cur = {{"bv-cab","open"},{"bv-jnt","closed"},{"man","m1"}};
    auto des = topo::routeShop(shop, {}).states;
    auto plans = topo::planShopTransition(shop, cur, des, {{"big",true},{"small",false}});
    ok("idle: risk only on the running blower", risk(plans, "big") && !risk(plans, "small"));
  }

  // ── disabled ports ────────────────────────────────────────────────────────
  // Turning the overarm off is how a woodworker says "not today". It must drop
  // out of routing entirely rather than lose a contest — a lost contest is a
  // problem to report, and this isn't one.
  {
    JsonObject overarm;
    for (JsonObject sys : shopDoc["systems"].as<JsonArray>())
      for (JsonObject el : sys["elements"].as<JsonArray>())
        if (el["id"] == "ts-overarm") overarm = el;
    ok("found the overarm port", !overarm.isNull());
    overarm["enabled"] = false;

    auto r = topo::routeShop(shop, {"table-saw"});
    ok("disabled: manifold not moved for it", state(r, "man") == "home", state(r, "man"));
    ok("disabled: saw still routed",          status(r, "table-saw") == "routed", status(r, "table-saw"));
    ok("disabled: overarm not in the verdict", join(r.machines["table-saw"].routed) == "ts-cabinet",
       join(r.machines["table-saw"].routed));

    // Every port off: the machine reaches nothing. It is ANSWERED FOR rather
    // than omitted — omission reads as "never asked", which is the one reading
    // that hides a running tool with no air.
    //
    // validateShop rejects this document — a primary port cannot be disabled
    // (RFC §6.6) — which is exactly why the runtime is tested on it. The device
    // adopts what it is handed; refusing to answer for a machine because its
    // document was wrong is how a running tool goes quiet.
    JsonObject cabinet;
    for (JsonObject sys : shopDoc["systems"].as<JsonArray>())
      for (JsonObject el : sys["elements"].as<JsonArray>())
        if (el["id"] == "ts-cabinet") cabinet = el;
    cabinet["enabled"] = false;
    auto r2 = topo::routeShop(shop, {"table-saw"});
    ok("all ports off: machine present in the result", r2.machines.count("table-saw") == 1);
    ok("all ports off: STRIPPED", status(r2, "table-saw") == "stripped", status(r2, "table-saw"));

    // Restore, then SWAP which port is primary — overarm required, cabinet the
    // bonus — to prove `supplemental` is what separates partial from the alarm:
    // identical contention, different verdict. Swapping rather than just
    // dropping the flag keeps the fixture legal: a machine has exactly one
    // primary port (RFC §6.3), and this is the JS test's mutation too.
    overarm.remove("enabled");
    cabinet.remove("enabled");
    overarm.remove("supplemental");
    cabinet["supplemental"] = true;
    auto r3 = topo::routeShop(shop, {"drill-press", "table-saw"});
    ok("without `supplemental`: the same loss is STRIPPED",
       status(r3, "table-saw") == "stripped", status(r3, "table-saw"));

    // And rule 2 is untouched: among PRIMARIES, recency still decides. Same
    // order as the arbitration case above, but now the manifold is contended by
    // two primaries — one per machine — so the saw's later start does win, which
    // is exactly what makes the drill press stripped rather than merely degraded.
    auto r4 = topo::routeShop(shop, {"table-saw", "drill-press"});
    ok("among primaries, most-recent still wins", state(r4, "man") == "m1", state(r4, "man"));
    ok("so the older machine is stripped", status(r4, "drill-press") == "stripped",
       status(r4, "drill-press"));
  }

  printf("\n%d/%d passed\n", passed, passed + failed);
  return failed ? 1 : 0;
}

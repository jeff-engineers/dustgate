// =============================================================================
// test_manual_blower.cpp — host conformance for running a blower BY HAND.
//
// The paired half of shared/device-model/manual-blower.test.js. The two engines
// can't share code, so the matched assertions ARE the anti-drift mechanism —
// same arrangement as nodelink.test.js ↔ test_nodebus.cpp. Change one, change
// both.
//
// What has to hold, in both engines:
//   • a hand start runs ONE system's blower, and only that one
//   • it opens a path FIRST — a sealed system is exactly what must never be
//     started into, and the moves go through the ordinary queue so ON still
//     waits for them (never dead-head)
//   • it HOLDS: no coast, no timeout, and a machine coming and going on the same
//     system doesn't end it
//   • switching it off is immediate, with no coast on the way out
//   • switching it off while a machine is running leaves the blower to the
//     machine, and the ordinary coast-down applies again afterwards
//
// Build + run via tools/ script `firmware:blower:test`.
// =============================================================================

#include <ArduinoJson.h>
#include "../control/TopologyRuntime.h"
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

/** Pump the runtime the way loop() does until it goes quiet. */
static void drain(topo::TopologyRuntime& rt, StubBus& bus, uint32_t nowMs = 0) {
  for (int i = 0; i < 50; i++) {
    rt.update(nowMs);
    if (bus.moving) { bus.settle(); continue; }
    if (!rt.transitioning()) { rt.update(nowMs); break; }
  }
}

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : "firmware/test/fixtures/";
  std::string shopJson = slurp(dir + "twoSystemShop.json");
  if (shopJson.empty()) { printf("bad twoSystemShop.json\n"); return 2; }

  // ── it runs, and only the system asked for ─────────────────────────────
  {
    StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    rt.begin(&nb);
    std::string err;
    ok("adopt twoSystemShop", rt.adopt(shopJson.c_str(), shopJson.size(), err), err);

    ok("a fresh shop has both blowers idle", !rt.collectorOn("big") && !rt.collectorOn("small"));
    ok("an unknown system is refused", !rt.setCollectorManual("no-such-system", true));

    ok("switching one on is accepted", rt.setCollectorManual("big", true));
    ok("and it says so", rt.collectorIsManual("big"));
    // ON is deferred until the path is open — the same rule a tool start obeys.
    ok("the blower waits for its path", !rt.collectorOn("big"));
    drain(rt, local, 1000);
    ok("then it runs", rt.collectorOn("big"));
    ok("the OTHER system is untouched", !rt.collectorOn("small") && !rt.collectorIsManual("small"));
  }

  // ── never dead-head: the path is opened first ──────────────────────────
  {
    StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    rt.begin(&nb);
    std::string err; rt.adopt(shopJson.c_str(), shopJson.size(), err);

    rt.setCollectorManual("big", true);
    ok("a hand start queues moves", rt.transitioning());
    drain(rt, local, 1000);
    ok("something was actually opened", !local.log.empty(), "no moves issued");
    // Whatever it opened, it must not be a close: a blower starting into a
    // sealed system is the one failure this project has a hard rule about.
    bool anyOpen = false;
    for (const std::string& m : local.log) if (m.find("->open") != std::string::npos) anyOpen = true;
    ok("...and what it opened was a gate", anyOpen);
    ok("only then did the blower start", rt.collectorOn("big"));
  }

  {
    // Already open: hold, don't re-route. Idle-hold means the shop rests where
    // it was, and a hand start that reshuffled gates would move a valve nobody
    // asked about.
    StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    rt.begin(&nb);
    std::string err; rt.adopt(shopJson.c_str(), shopJson.size(), err);

    rt.setToolPower("jointer", 800);
    drain(rt, local, 1000);
    rt.setToolPower("jointer", 0);
    drain(rt, local, 2000);
    size_t movesBefore = local.log.size();

    rt.setCollectorManual("big", true);
    drain(rt, local, 3000);
    ok("an open system is not re-routed", local.log.size() == movesBefore,
       std::to_string(local.log.size()) + " vs " + std::to_string(movesBefore));
    ok("and the blower runs", rt.collectorOn("big"));
  }

  // ── it HOLDS ───────────────────────────────────────────────────────────
  {
    StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    rt.begin(&nb);
    std::string err; rt.adopt(shopJson.c_str(), shopJson.size(), err);

    rt.setCollectorManual("big", true);
    drain(rt, local, 1000);
    ok("a hand run is not coasting", !rt.collectorCoasting("big"));
    // Time is what ends a coast; it must not end this.
    rt.update(61000);
    ok("and it does not time out", rt.collectorOn("big"), "off a minute later");

    rt.setToolPower("jointer", 800);
    drain(rt, local, 70000);
    ok("a machine can start while it runs", rt.collectorOn("big"));
    ok("...and the hand switch is still set", rt.collectorIsManual("big"));

    rt.setToolPower("jointer", 0);
    drain(rt, local, 80000);
    rt.update(86000);
    // Without the flag this is exactly where the coast-down would begin.
    ok("the blower stays on after that machine stops", rt.collectorOn("big"));
    ok("and it is not coasting down", !rt.collectorCoasting("big"));
  }

  // ── switching it off ───────────────────────────────────────────────────
  {
    StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    rt.begin(&nb);
    std::string err; rt.adopt(shopJson.c_str(), shopJson.size(), err);

    rt.setCollectorManual("big", true);
    drain(rt, local, 1000);
    rt.setCollectorManual("big", false);
    ok("off means off", !rt.collectorOn("big"));
    ok("and it stops claiming to be manual", !rt.collectorIsManual("big"));
    // No coast on the way out: the coast catches dust still in the pipe after a
    // cut, and nothing was being cut.
    ok("it does not coast down afterwards", !rt.collectorCoasting("big"));
  }

  {
    // Off while a machine is running must not stop the blower — the machine
    // still needs it, and this switch was never about the machine.
    StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    rt.begin(&nb);
    std::string err; rt.adopt(shopJson.c_str(), shopJson.size(), err);

    rt.setCollectorManual("big", true);
    drain(rt, local, 1000);
    rt.setToolPower("jointer", 800);
    drain(rt, local, 2000);
    rt.setCollectorManual("big", false);
    drain(rt, local, 3000);
    ok("a machine still holds the blower on", rt.collectorOn("big"));
    ok("but the hand run is over", !rt.collectorIsManual("big"));

    // ...and the ordinary coast-down applies again.
    rt.setToolPower("jointer", 0);
    rt.update(4000);
    ok("so the next idle coasts as usual", rt.collectorCoasting("big"));
  }

  printf("\n%d/%d passed%s\n", passed, passed + failed, failed ? "  — FAILURES" : "");
  return failed ? 1 : 0;
}

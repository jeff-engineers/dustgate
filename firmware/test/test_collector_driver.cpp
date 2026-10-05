// test_collector_driver.cpp — host tests for control/CollectorDriver.h: the glue around the press policy
// (read the plug, ask the policy, press, record it, raise or clear the problems a person must see).
// The policy itself is test_collector_press.cpp; this guards the wiring, which used to live inline in the
// sketch's loop() with no test at all. Run from the repo root.
#include "../control/CollectorDriver.h"
#include <cstdio>
#include <fstream>
#include <sstream>
using namespace topo;
static int passed = 0, failed = 0;
static void ok(const char* what, bool c) { if (c) { printf("  ok   %s\n", what); passed++; } else { printf("  FAIL %s\n", what); failed++; } }

struct FakePresser : CollectorPresser {
  int presses = 0; bool result = true;
  bool press() override { presses++; return result; }
  const char* kind() const override { return "fake"; }
};
struct Hooks : DriverHooks {
  std::vector<std::string> lines; int pets = 0;
  void say(const std::string& l) override { lines.push_back(l); }
  void pet() override { pets++; }
  bool saw(const char* n) const { for (auto& l : lines) if (l.find(n) != std::string::npos) return true; return false; }
};
struct NullBus : ActuatorBus {
  bool online() const override { return true; } bool busy() const override { return false; }
  bool setState(const char*, JsonObjectConst, const char*) override { return true; }
};

static TopologyRuntime* load(NodeBus& nb, bool withSensor = true) {
  std::ifstream f("firmware/test/fixtures/twoGates.json"); std::stringstream b; b << f.rdbuf();
  std::string text = b.str();
  // A sense-only plug watching the blower: the feedback every closed-loop case below needs.
  const std::string dc = "{\"id\": \"dc\", \"type\": \"collector\", \"name\": \"Dust Collector\"";
  const size_t at = text.find(dc);
  if (withSensor && at != std::string::npos) text.insert(at + dc.size(), ", \"sensor\": {\"outlet\": {\"gen\": 2, \"ip\": \"192.168.87.50\"}}");
  static NullBus nul; nb.setLocal(&nul, "primary");
  TopologyRuntime* rt = new TopologyRuntime(); rt->begin(&nb);
  std::string err; if (!rt->adopt(text.data(), text.size(), err)) { printf("fixture: %s\n", err.c_str()); exit(2); }
  return rt;
}

int main() {
  const std::string sys = "system-1";
  printf("\nD1 a blower that will not start: pressed, waited on, pressed again, then given up on\n");
  {
    NodeBus nb; TopologyRuntime* rt = load(nb); FakePresser p; Hooks h; PressState ps;
    rt->setCollectorPlug(sys, 0.0f, true, 0);
    uint32_t t = 100000;
    ok("the first disagreement presses", driveCollectorPress(*rt, sys, p, ps, true, t, h) && p.presses == 1);
    ok("...says so on the console, with the attempt number", h.saw("press #1 wanting ON"));
    ok("...and pets the watchdog around the slow transmit", h.pets >= 2);
    ok("inside the cooldown it does not press again", !driveCollectorPress(*rt, sys, p, ps, true, t + 2000, h) && p.presses == 1);
    t += kPressCooldownMs + 10;
    ok("after it, a blower still off is pressed again", driveCollectorPress(*rt, sys, p, ps, true, t, h) && p.presses == 2);
    t += kPressCooldownMs + 10;
    ok("and a third time", driveCollectorPress(*rt, sys, p, ps, true, t, h) && p.presses == 3);
    t += kPressCooldownMs + 10;
    ok("then it stops pressing", !driveCollectorPress(*rt, sys, p, ps, true, t, h) && p.presses == 3);
    ok("...and raises the problem a person has to see", rt->hasProblem("rf-gave-up:" + sys));
    ok("...saying it on the console exactly once", [&] { driveCollectorPress(*rt, sys, p, ps, true, t + 1000, h);
        int n = 0; for (auto& l : h.lines) if (l.find("did not respond") != std::string::npos) n++; return n == 1; }());
    delete rt;
  }

  printf("\nD2 a blower that agrees is left alone, and the budget resets\n");
  {
    NodeBus nb; TopologyRuntime* rt = load(nb); FakePresser p; Hooks h; PressState ps;
    rt->setCollectorPlug(sys, 0.0f, true, 0);
    uint32_t t = 100000;
    driveCollectorPress(*rt, sys, p, ps, true, t, h);
    rt->setCollectorPlug(sys, 600.0f, true, 6000);
    t += kPressCooldownMs + 10;
    ok("drawing power, nothing more is pressed", !driveCollectorPress(*rt, sys, p, ps, true, t, h) && p.presses == 1);
    ok("...and the attempt budget is back to full", ps.attempts == 0);
  }

  printf("\nD3 a transmitter that could not send is a problem, and a later success clears it\n");
  {
    NodeBus nb; TopologyRuntime* rt = load(nb); FakePresser p; Hooks h; PressState ps;
    rt->setCollectorPlug(sys, 0.0f, true, 0);
    p.result = false;
    driveCollectorPress(*rt, sys, p, ps, true, 100000, h);
    ok("a failed send raises rf-send", rt->hasProblem("rf-send:" + sys) && h.saw("TRANSMIT FAILED"));
    p.result = true;
    driveCollectorPress(*rt, sys, p, ps, true, 100000 + kPressCooldownMs + 10, h);
    ok("the next successful one clears it", !rt->hasProblem("rf-send:" + sys));
  }

  printf("\nD4 told to stop, still drawing: pressed again within the budget, then given up on\n");
  {
    NodeBus nb; TopologyRuntime* rt = load(nb); FakePresser p; Hooks h; PressState ps;
    rt->setCollectorPlug(sys, 600.0f, true, 6000);
    uint32_t t = 100000;
    ok("a blower still drawing after we want it OFF is pressed", driveCollectorPress(*rt, sys, p, ps, false, t, h) && p.presses == 1);
    ok("...saying OFF, and that it saw it running", h.saw("wanting OFF (saw running"));
    for (int i = 0; i < 2; i++) { t += kPressCooldownMs + 10; driveCollectorPress(*rt, sys, p, ps, false, t, h); }
    t += kPressCooldownMs + 10;
    ok("three presses and no more", !driveCollectorPress(*rt, sys, p, ps, false, t, h) && p.presses == 3);
    ok("...and it is reported, not retried forever", rt->hasProblem("rf-gave-up:" + sys));
  }
  printf("\nD5 an open-loop collector (no plug, sensor or clamp): ON once, OFF once, nothing at boot\n");
  {
    NodeBus nb; TopologyRuntime* rt = load(nb, false); FakePresser p; Hooks h; PressState ps;
    uint32_t t = 100000;
    ok("wanting OFF at boot presses nothing (a toggle would start an idle blower)", !driveCollectorPress(*rt, sys, p, ps, false, t, h) && p.presses == 0);
    ok("wanting ON presses once", driveCollectorPress(*rt, sys, p, ps, true, t + 1000, h) && p.presses == 1);
    ok("...and only once, however long it runs", !driveCollectorPress(*rt, sys, p, ps, true, t + 600000, h) && p.presses == 1);
    ok("wanting OFF again presses once", driveCollectorPress(*rt, sys, p, ps, false, t + 700000, h) && p.presses == 2);
    ok("...and only once", !driveCollectorPress(*rt, sys, p, ps, false, t + 1300000, h) && p.presses == 2);
  }
  printf("\n%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}

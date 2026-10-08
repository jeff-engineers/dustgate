// test_slider_setup.cpp — setting up a sliding gate by hand (TopologyRuntime::driveLinearTo), the move behind the slider
// page's jog, capture and test when the slider lives on a node. C++ only: the JS model has no bus to move anything on.
//
//   • a setup move sends ONE SET at the asked position, and the node echoes a made-up state
//   • it is refused while a tool runs, while anything is moving, for a gate that is not a slider, and off the rail
//   • afterwards routing does not trust where it last sent the gate: the next switch-on moves it again
//
// Build + run via tools/ script `firmware:slidersetup:test`.
#include <ArduinoJson.h>
#include "../control/TopologyRuntime.h"
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
static std::string slurp(const std::string& p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

struct StubBus : public topo::ActuatorBus {
  bool up = true, moving = false;
  std::vector<std::string> log;
  std::string lastMm;
  bool lastHome = false;
  bool online() const override { return up; }
  bool busy()   const override { return moving; }
  bool setState(const char* selectorId, JsonObjectConst sel, const char* stateId) override {
    log.push_back(std::string(selectorId) + "->" + stateId);
    for (JsonObjectConst s : sel["states"].as<JsonArrayConst>())
      if (std::string(s["id"] | "") == stateId && s.containsKey("positionMm")) lastMm = std::to_string((int)(s["positionMm"].as<float>() * 10));
    lastHome = sel["homeFirst"] | false;   // what buildSetFrame turns into SET.home
    moving = true;
    return true;
  }
  void settle() { moving = false; }
};
static void drain(topo::TopologyRuntime& rt, StubBus& bus, uint32_t t) {
  for (int i = 0; i < 50; i++) { rt.update(t); if (bus.moving) { bus.settle(); continue; } if (!rt.transitioning()) { rt.update(t); break; } }
}

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : "firmware/test/fixtures/";
  const std::string shop = slurp(dir + "twoSystemShop.json");
  if (shop.empty()) { printf("bad twoSystemShop.json\n"); return 2; }

  StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
  nb.setLocal(&local, "primary"); rt.begin(&nb);
  std::string err; rt.adopt(shop.c_str(), shop.size(), err);
  std::string why;

  // ── a setup move ───────────────────────────────────────────────────────────
  ok("a slider is driven to the asked distance", rt.driveLinearTo("man", 83.5f, why), why);
  ok("...as one SET with a made-up state", local.log.size() == 1 && local.log.back() == "man->setup");
  ok("...carrying the position", local.lastMm == "835", local.lastMm);
  ok("...and no home unless asked", !local.lastHome);
  ok("while it moves, a gate is moving", rt.anyGateMoving());
  ok("a second move is refused until it lands", !rt.driveLinearTo("man", 90.0f, why) && why.find("moving") != std::string::npos, why);
  local.settle();
  ok("...and taken once it has", rt.driveLinearTo("man", 90.0f, why), why);
  local.settle();
  ok("the first move of a setup can ask to find home again", rt.driveLinearTo("man", 1.0f, why, true) && local.lastHome, why);
  local.settle();

  // ── what is refused ────────────────────────────────────────────────────────
  ok("a servo gate is not a slider", !rt.driveLinearTo("bv-cab", 10.0f, why) && why.find("sliding") != std::string::npos, why);
  ok("an unknown gate is refused", !rt.driveLinearTo("nope", 10.0f, why));
  ok("a negative distance is refused", !rt.driveLinearTo("man", -1.0f, why));
  rt.update(1000); rt.setMachinePower("drill-press", 200); drain(rt, local, 1000);
  ok("nothing moves for setup while a tool runs", !rt.driveLinearTo("man", 10.0f, why) && why.find("tool") != std::string::npos, why);

  // ── routing does not trust a gate that setup moved ─────────────────────────
  // The drill press routed the slider to its stop; a setup move then takes it elsewhere. Routing still wants the same
  // stop, and must send it there again rather than believe it never left.
  rt.setMachinePower("drill-press", 0); rt.update(1000 + topo::kDefaultCollectorOffDelayMs + 1);
  const auto at = rt.actuatorStates().find("man");
  const std::string routedTo = at == rt.actuatorStates().end() ? "" : at->second;
  ok("the drill press had the slider at a stop", !routedTo.empty() && routedTo != "home", routedTo);
  ok("a setup move after it", rt.driveLinearTo("man", 5.0f, why), why);
  local.settle();
  const size_t before = local.log.size();
  rt.update(9000); rt.setMachinePower("drill-press", 200); drain(rt, local, 9000);
  bool resent = false;
  for (size_t i = before; i < local.log.size(); i++) if (local.log[i] == "man->" + routedTo) resent = true;
  ok("the next switch-on drives the slider back to its stop", resent);

  printf("%d/%d passed\n", passed, passed + failed);
  return failed ? 1 : 0;
}

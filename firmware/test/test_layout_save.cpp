// =============================================================================
// test_layout_save.cpp — a layout SAVE is not a reboot (TopologyRuntime::adopt).
//
// The paired half of shared/device-model/layout-save.test.js: same cases, same order. The two engines can't share code,
// so the matched assertions ARE the anti-drift mechanism. Where this engine has something the JS model does not (a bus
// that moves gates one at a time), a case adds a check about it, marked "(the bus)".
//
// What has to hold, in both engines:
//   • a save while a tool runs leaves its blower running and its gate where it is
//   • ...and the blower still coasts down and stops when the tool stops (it used to read as started by a person, and
//     nothing pressed it off)
//   • a blower switched on by hand stays on, and stays "by hand"
//   • a gate the save changed is forgotten (seeded closed); one it only renamed is kept
//   • a new trip point takes effect at once, and a tool now under it coasts down like one switched off
//   • a FIRST layout, or one after a reset, still starts from nothing
//   • sameHardware(): everything but the name, with key order and 90 vs 90.0 not counting as changes
//
// Build + run via tools/ script `firmware:layoutsave:test`.
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
  printf("  %s %s%s\n", cond ? "✓" : "✗", name, cond || detail.empty() ? "" : ("  — " + detail).c_str());
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
static void drain(topo::TopologyRuntime& rt, StubBus& bus, uint32_t nowMs) {
  for (int i = 0; i < 50; i++) {
    rt.update(nowMs);
    if (bus.moving) { bus.settle(); continue; }
    if (!rt.transitioning()) { rt.update(nowMs); break; }
  }
}

struct Rig {
  StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
  Rig() { nb.setLocal(&local, "primary"); rt.begin(&nb); }
  bool adopt(const std::string& json) { std::string err; return rt.adopt(json.c_str(), json.size(), err); }
  void power(const char* id, float w, uint32_t t) { rt.update(t); rt.setMachinePower(id, w); drain(rt, local, t); }
  std::string gate(const char* id) const {
    auto it = rt.actuatorStates().find(id);
    return it == rt.actuatorStates().end() ? std::string("?") : it->second;
  }
  bool running(const char* id) const {
    for (const std::string& m : rt.activeMachines()) if (m == id) return true;
    return false;
  }
};

// An element by id, anywhere in the shop, for editing a copy of the fixture.
static JsonObject element(DynamicJsonDocument& d, const char* id) {
  for (JsonObject sys : d["systems"].as<JsonArray>())
    for (JsonObject e : sys["elements"].as<JsonArray>())
      if (std::string(e["id"] | "") == id) return e;
  return JsonObject();
}
static JsonObject machine(DynamicJsonDocument& d, const char* id) {
  for (JsonObject m : d["machines"].as<JsonArray>()) if (std::string(m["id"] | "") == id) return m;
  return JsonObject();
}
static std::string dump(const DynamicJsonDocument& d) { std::string s; serializeJson(d, s); return s; }

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : "firmware/test/fixtures/";
  const std::string shopJson = slurp(dir + "twoSystemShop.json");
  if (shopJson.empty()) { printf("bad twoSystemShop.json\n"); return 2; }
  const uint32_t OFF_DELAY = topo::kDefaultCollectorOffDelayMs;

  // ── a save while a tool runs ───────────────────────────────────────────────
  {
    Rig r; r.adopt(shopJson);
    r.power("jointer", 800, 1000);
    ok("the jointer runs its blower", r.rt.collectorOn("big"));
    ok("...through its own gate", r.gate("bv-jnt") == "open");

    const size_t movesBefore = r.local.log.size();
    r.rt.update(2000);
    ok("the same layout is saved again", r.adopt(shopJson));
    drain(r.rt, r.local, 2000);
    ok("after a save the blower is still running", r.rt.collectorOn("big"));
    ok("...and not coasting", !r.rt.collectorCoasting("big"));
    ok("the gate is still open", r.gate("bv-jnt") == "open");
    ok("the jointer still reads as running", r.running("jointer"));
    ok("(the bus) the save moved nothing", r.local.log.size() == movesBefore,
       std::to_string(r.local.log.size() - movesBefore) + " moves");

    // The bug this exists for: the blower read as started by a person after a save, and nothing turned it off.
    r.power("jointer", 0, 3000);
    ok("when the tool stops after the save, the blower coasts", r.rt.collectorOn("big") && r.rt.collectorCoasting("big"));
    r.rt.update(3000 + OFF_DELAY + 1);
    ok("...and stops when the coast is over", !r.rt.collectorOn("big"));
  }

  // ── a blower switched on by hand ───────────────────────────────────────────
  {
    Rig r; r.adopt(shopJson);
    r.rt.update(1000);
    r.rt.setCollectorManual("big", true);
    drain(r.rt, r.local, 1000);
    r.rt.update(2000);
    r.adopt(shopJson);
    drain(r.rt, r.local, 2000);
    ok("a hand-run blower is still running after a save", r.rt.collectorOn("big"));
    ok("...and still by hand", r.rt.collectorIsManual("big"));
    r.rt.update(2000 + 61000);
    ok("...and it does not time out", r.rt.collectorOn("big"));
  }

  // ── a gate the save changed, and one it only renamed ───────────────────────
  {
    Rig r; r.adopt(shopJson);
    r.power("table-saw", 800, 1000);   // the cabinet port on 'big', the overarm on the slider in 'small'
    r.power("table-saw", 0, 2000);     // idle HOLDS the gates where they are
    ok("the cabinet gate was left open", r.gate("bv-cab") == "open");
    ok("the slider was left at a stop", r.gate("man") != "home", r.gate("man"));
    r.rt.update(2000 + OFF_DELAY + 1);
    ok("both blowers are off once the coast is over", !r.rt.collectorOn("big") && !r.rt.collectorOn("small"));

    DynamicJsonDocument edited(shopJson.size() * 4 + 2048);
    deserializeJson(edited, shopJson);
    element(edited, "bv-cab")["name"] = "Cabinet saw valve";              // a label: the same gate
    element(edited, "man")["states"][1]["positionMm"] = 14.0;             // a stop moved: maybe not where it was
    r.rt.update(2000 + OFF_DELAY + 2);
    r.adopt(dump(edited));
    drain(r.rt, r.local, 2000 + OFF_DELAY + 2);
    ok("a gate that was only renamed keeps its position", r.gate("bv-cab") == "open", r.gate("bv-cab"));
    ok("a gate whose stops changed is forgotten (closed)", r.gate("man") == "home", r.gate("man"));
    ok("an untouched closed gate stays closed", r.gate("bv-jnt") == "closed");
  }

  // ── a new trip point takes effect at once ──────────────────────────────────
  {
    Rig r; r.adopt(shopJson);
    r.power("jointer", 800, 1000);
    DynamicJsonDocument raised(shopJson.size() * 4 + 2048);
    deserializeJson(raised, shopJson);
    machine(raised, "jointer")["sensor"]["outlet"]["thresholdW"] = 1000;   // tuned from the phone, with the jointer running
    r.rt.update(2000);
    r.adopt(dump(raised));
    ok("a tool now under its trip point is no longer running", !r.running("jointer"));
    ok("...so its blower coasts", r.rt.collectorOn("big") && r.rt.collectorCoasting("big"));
    r.rt.update(2000 + OFF_DELAY + 1);
    ok("...and stops when the coast is over", !r.rt.collectorOn("big"));
  }

  // ── a FIRST layout, or one after a reset, starts from nothing ──────────────
  {
    Rig r0; r0.adopt(shopJson);
    ok("a first layout has every blower off", !r0.rt.collectorOn("big") && !r0.rt.collectorOn("small"));
    ok("...and every gate closed", r0.gate("bv-cab") == "closed" && r0.gate("bv-jnt") == "closed" && r0.gate("man") == "home");

    Rig r; r.adopt(shopJson);
    r.power("jointer", 800, 1000);
    r.rt.clear();                      // a reset hands nothing on
    r.adopt(shopJson);
    ok("a layout after a reset has the blower off", !r.rt.collectorOn("big"));
    ok("...and the gate closed", r.gate("bv-jnt") == "closed");
    ok("...and no tool running", r.rt.activeMachines().empty());
  }

  // ── sameHardware ───────────────────────────────────────────────────────────
  {
    auto gate = [&](DynamicJsonDocument& d) { deserializeJson(d, shopJson); return element(d, "bv-cab"); };
    DynamicJsonDocument a(8192), b(8192);
    ok("a gate is the same gate as itself", topo::sameHardware(gate(a), gate(b)));
    DynamicJsonDocument c(8192); JsonObject renamed = gate(c); renamed["name"] = "Something else";
    ok("a new name is the same gate", topo::sameHardware(gate(a), renamed));
    DynamicJsonDocument e(8192); JsonObject angle = gate(e); angle["states"][1]["offsetDeg"] = 85;
    ok("a new closed angle is not", !topo::sameHardware(gate(a), angle));
    DynamicJsonDocument f(8192); JsonObject channel = gate(f); channel["servo"]["channel"] = 1;
    ok("a new channel is not", !topo::sameHardware(gate(a), channel));
    DynamicJsonDocument g(8192); JsonObject board = gate(g); board["controllerId"] = "dustgate-planer";
    ok("a new board is not", !topo::sameHardware(gate(a), board));
    // Key order: the same gate written back to front.
    DynamicJsonDocument h(8192); JsonObject fwd = gate(h);
    std::vector<std::pair<std::string, std::string>> kv;
    for (JsonPair p : fwd) { std::string v; serializeJson(p.value(), v); kv.push_back({p.key().c_str(), v}); }
    std::string rev = "{";
    for (size_t i = kv.size(); i-- > 0;) rev += "\"" + kv[i].first + "\":" + kv[i].second + (i ? "," : "");
    rev += "}";
    DynamicJsonDocument reversed(8192); deserializeJson(reversed, rev);
    ok("key order does not count", topo::sameHardware(gate(a), reversed.as<JsonObjectConst>()));
    DynamicJsonDocument n1(256), n2(256);
    deserializeJson(n1, "{\"a\":90}"); deserializeJson(n2, "{\"a\":90.0}");
    ok("90 and 90.0 are the same number", topo::sameHardware(n1.as<JsonObjectConst>(), n2.as<JsonObjectConst>()));
  }

  printf("%d/%d passed\n", passed, passed + failed);
  return failed ? 1 : 0;
}

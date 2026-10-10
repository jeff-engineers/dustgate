// test_sensorplan.cpp — host tests for control/SensorPlan.h: the layout resolved into what it
// wants watched, once, for both the push and the poll.
#include "../control/SensorPlan.h"
#include <cstdio>
#include <cstring>
using namespace topo;
static int passed = 0, failed = 0;
static void ok(const char* what, bool c) { if (c) { printf("  ok   %s\n", what); passed++; } else { printf("  FAIL %s\n", what); failed++; } }

static const char* kShop = R"({"schemaVersion":2,
  "controllers":[{"id":"primary","role":"primary"},{"id":"nodeA","role":"secondary"},{"id":"nodeC","role":"secondary"}],
  "systems":[{"id":"s1","name":"s","elements":[
    {"id":"dc","type":"collector","controllerId":"nodeC","sensor":{"ct":{"channel":2}},
      "bin":{"sensor":{"kind":"threshold","invert":false}}},
    {"id":"gA","type":"selector","controllerId":"nodeA","kind":"servoGate"},
    {"id":"gP","type":"selector","controllerId":"primary","kind":"servoGate"},
    {"id":"tA","type":"tool","machineId":"mA"},
    {"id":"tA2","type":"tool","machineId":"mA"},
    {"id":"tP","type":"tool","machineId":"mP"},
    {"id":"tC","type":"tool","machineId":"mC"}],
    "ducts":[{"child":"gA","parent":"dc"},{"child":"gP","parent":"dc"},
             {"child":"tA","parent":"gA","parentBranch":"b1"},{"child":"tA2","parent":"gA","parentBranch":"b2"},
             {"child":"tP","parent":"gP","parentBranch":"b1"},{"child":"tC","parent":"gP","parentBranch":"b2"}]}],
  "machines":[
    {"id":"mA","sensor":{"outlet":{"ip":"192.168.86.40","kind":"tasmota","thresholdW":25}}},
    {"id":"mP","sensor":{"outlet":{"ip":"192.168.86.41","kind":"shelly","thresholdW":10}}},
    {"id":"mC","sensor":{"ct":{"controllerId":"nodeA","channel":1},"outlet":{"ip":"192.168.86.42","kind":"shelly"}}}]})";

int main() {
  DynamicJsonDocument doc(8192);
  deserializeJson(doc, kShop);
  const auto plan = planSensors(doc.as<JsonObjectConst>(), [](const std::string& m) { return m == "mA" ? 25.0f : 7.0f; });

  auto find = [&](PlannedSensor::Kind k, const std::string& id) -> const PlannedSensor* {
    for (const PlannedSensor& p : plan) if (p.kind == k && p.id == id) return &p;
    return nullptr;
  };

  printf("\nP1 clamps: one per machine, through the machine\n");
  {
    const PlannedSensor* mc = find(PlannedSensor::Kind::Clamp, "mC");
    ok("a tool's clamp is found through its machine", mc && mc->board == "nodeA" && mc->channel == 1 && !mc->onCollector);
    const PlannedSensor* dc = find(PlannedSensor::Kind::Clamp, "dc");
    ok("a collector's clamp is keyed by the element and says so", dc && dc->board == "nodeC" && dc->channel == 2 && dc->onCollector && dc->systemId == "s1");
    int clamps = 0; for (const PlannedSensor& p : plan) if (p.kind == PlannedSensor::Kind::Clamp) clamps++;
    ok("exactly the two clamps", clamps == 2);
  }

  printf("\nP2 plugs: only for a tool without a clamp, one per machine, owned by its gate's board\n");
  {
    int mAplugs = 0; for (const PlannedSensor& p : plan) if (p.kind == PlannedSensor::Kind::Plug && p.id == "mA") mAplugs++;
    ok("a two-port machine yields ONE plug", mAplugs == 1);
    const PlannedSensor* ma = find(PlannedSensor::Kind::Plug, "mA");
    ok("the plug carries address, protocol and threshold", ma && ma->ip == "192.168.86.40" && ma->tasmota && ma->thresholdW == 25.0f);
    ok("a plug behind a node's gate is that node's", ma && ma->board == "nodeA");
    const PlannedSensor* mp = find(PlannedSensor::Kind::Plug, "mP");
    ok("a plug behind the primary's own gate is the brain's (empty board)", mp && mp->board.empty() && !mp->tasmota && mp->thresholdW == 7.0f);
    ok("a machine WITH a clamp has no plug entry", find(PlannedSensor::Kind::Plug, "mC") == nullptr);
  }

  printf("\nP3 bins: one per collector, keyed by its system\n");
  {
    const PlannedSensor* b = find(PlannedSensor::Kind::Bin, "bin:s1");
    ok("the bin is found with its board, system and invert", b && b->board == "nodeC" && b->systemId == "s1" && !b->invert);
  }

  printf("\nP4 order: clamps, then plugs, then bins\n");
  {
    bool seenPlug = false, seenBin = false, orderOk = true;
    for (const PlannedSensor& p : plan) {
      if (p.kind == PlannedSensor::Kind::Plug) seenPlug = true;
      if (p.kind == PlannedSensor::Kind::Bin) seenBin = true;
      if (p.kind == PlannedSensor::Kind::Clamp && (seenPlug || seenBin)) orderOk = false;
      if (p.kind == PlannedSensor::Kind::Plug && seenBin) orderOk = false;
    }
    ok("the order each board's CONFIG is written in", orderOk);
  }

  printf("\nP5 a layout that watches nothing plans nothing\n");
  {
    DynamicJsonDocument d2(1024);
    deserializeJson(d2, R"({"schemaVersion":2,"controllers":[{"id":"primary","role":"primary"}],
      "systems":[{"id":"s","name":"s","elements":[{"id":"dc","type":"collector"}],"ducts":[]}],"machines":[]})");
    ok("an empty plan", planSensors(d2.as<JsonObjectConst>(), nullptr).empty());
  }

  printf("\n%d/%d passed%s\n", passed, passed + failed, failed ? " — FAILED" : "");
  return failed ? 1 : 0;
}

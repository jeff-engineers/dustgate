// test_layoutgate.cpp — host tests for control/LayoutGate.h, the structural check both brains apply to a layout.
#include "../control/LayoutGate.h"
#include <cstdio>
#include <string>
using namespace topo;
static int passed = 0, failed = 0;
static void ok(const char* what, bool c) { if (c) { printf("  ok   %s\n", what); passed++; } else { printf("  FAIL %s\n", what); failed++; } }
static bool gate(const char* json, std::string* why = nullptr) {
  DynamicJsonDocument d(4096); deserializeJson(d, json);
  std::string e; const bool r = layoutGate(d.as<JsonObjectConst>(), e);
  if (why) *why = e;
  return r;
}

int main() {
  printf("\nG the structural gate\n");
  const char* good = R"({"controllers":[{"id":"primary","role":"primary"}],"machines":[],
    "systems":[{"id":"s1","elements":[{"id":"dc","type":"collector"}]}]})";
  ok("a shop with one primary, one system, one collector passes", gate(good));
  std::string why;
  ok("no systems: refused", !gate(R"({"controllers":[{"id":"p","role":"primary"}],"machines":[],"systems":[]})", &why) && why == "a shop needs at least one system");
  ok("systems missing: refused", !gate(R"({"controllers":[{"id":"p","role":"primary"}],"machines":[]})", &why) && why == "systems not an array");
  ok("two primaries: refused", !gate(R"({"controllers":[{"id":"a","role":"primary"},{"id":"b","role":"primary"}],"machines":[],
    "systems":[{"id":"s1","elements":[{"id":"dc","type":"collector"}]}]})", &why) && why == "exactly one primary controller required");
  ok("no controllers: refused", !gate(R"({"machines":[],"systems":[{"id":"s1","elements":[{"id":"dc","type":"collector"}]}]})", &why) && why == "controllers not an array");
  ok("no machines array: refused", !gate(R"({"controllers":[{"id":"p","role":"primary"}],"systems":[{"id":"s1","elements":[{"id":"dc","type":"collector"}]}]})", &why) && why == "machines not an array");
  ok("a system with no id: refused", !gate(R"({"controllers":[{"id":"p","role":"primary"}],"machines":[],"systems":[{"elements":[{"id":"dc","type":"collector"}]}]})", &why) && why == "system missing id");
  ok("a system with two collectors: refused", !gate(R"({"controllers":[{"id":"p","role":"primary"}],"machines":[],
    "systems":[{"id":"s1","elements":[{"id":"a","type":"collector"},{"id":"b","type":"collector"}]}]})", &why) && why == "system \"s1\": exactly one collector required");
  ok("a system with none: refused", !gate(R"({"controllers":[{"id":"p","role":"primary"}],"machines":[],"systems":[{"id":"s1","elements":[]}]})"));
  ok("two gates on one board's channel 0: refused", !gate(R"({"controllers":[{"id":"p","role":"primary"}],"machines":[],
    "systems":[{"id":"s1","elements":[{"id":"dc","type":"collector"},
      {"id":"g1","type":"selector","controllerId":"n1","servo":{"channel":0}},
      {"id":"g2","type":"selector","controllerId":"n1","servo":{"channel":0}}]}]})", &why) && why == "servo channel 0 on host \"n1\" is already used by another gate");
  ok("...the same channel on two boards is fine", gate(R"({"controllers":[{"id":"p","role":"primary"}],"machines":[],
    "systems":[{"id":"s1","elements":[{"id":"dc","type":"collector"},
      {"id":"g1","type":"selector","controllerId":"n1","servo":{"channel":0}},
      {"id":"g2","type":"selector","controllerId":"n2","servo":{"channel":0}}]}]})"));
  printf("\n%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}

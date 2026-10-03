// test_plugowner.cpp — which board polls a machine's plug (Shop.h plugOwnerOf).
// PAIRED with shared/device-model/plug-owner.test.js: same fixture, same cases, same order.
#include <ArduinoJson.h>
#include <fstream>
#include <sstream>
#include <cstdio>
#include "../control/TopologyRouter.h"
#include "../control/Shop.h"
using namespace topo;

static int passed = 0, failed = 0;
static void ok(const char* what, bool c, const std::string& got = "") {
  if (c) { printf("  ok   %s\n", what); passed++; }
  else   { printf("  FAIL %s  got: %s\n", what, got.c_str()); failed++; }
}

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : "firmware/test/fixtures/";
  std::ifstream f(dir + "plugOwners.json");
  std::stringstream ss; ss << f.rdbuf();
  DynamicJsonDocument doc(16384);
  if (deserializeJson(doc, ss.str())) { printf("bad plugOwners.json\n"); return 2; }
  JsonObjectConst d = doc.as<JsonObjectConst>();

  ok("a tool behind a node's gate is polled by that node", plugOwnerOf(d, "mA") == "nodeA", plugOwnerOf(d, "mA"));
  ok("every tool behind a manifold node is polled by it (1)", plugOwnerOf(d, "mB1") == "nodeB", plugOwnerOf(d, "mB1"));
  ok("every tool behind a manifold node is polled by it (2)", plugOwnerOf(d, "mB2") == "nodeB", plugOwnerOf(d, "mB2"));
  ok("a tool behind the primary's own gate stays with the brain", plugOwnerOf(d, "mP") == "", plugOwnerOf(d, "mP"));
  ok("a tool plumbed straight to a junction has no board of its own", plugOwnerOf(d, "mD") == "", plugOwnerOf(d, "mD"));
  ok("a two-port machine takes the first port that has a node", plugOwnerOf(d, "mTwo") == "nodeA", plugOwnerOf(d, "mTwo"));
  ok("every machine has an answer (an unknown one is the brain)", plugOwnerOf(d, "nope") == "");

  printf("\n%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}

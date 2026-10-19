// test_braincadence.cpp — host tests for control/BrainCadence.h, the clocks both brains keep.
#include "../control/BrainCadence.h"
#include <cstdio>
using namespace topo;
static int passed = 0, failed = 0;
static void ok(const char* what, bool c) { if (c) { printf("  ok   %s\n", what); passed++; } else { printf("  FAIL %s\n", what); failed++; } }
int main() {
  printf("\nC the beacon and the hourly line\n");
  ok("a required board down: every 5 s", beaconEveryMs(true) == 5000);
  ok("otherwise: every 60 s", beaconEveryMs(false) == 60000);
  ok("the hourly line is hourly", kHourlyLogMs == 3600000);
  ok("boards and their links, in order", hourlyNodesField({{"dustgate-planer", true}, {"dustgate-cyclone", false}}) == "dustgate-planer:up dustgate-cyclone:down");
  ok("no boards: empty", hourlyNodesField({}).empty());
  printf("\n%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}

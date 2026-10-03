// test_relocate.cpp — host tests for control/OutletRelocate.h.
#include "../control/OutletRelocate.h"
#include <cstdio>
#include <string>
using namespace relocate;
static int passed = 0, failed = 0;
static void ok(const char* what, bool c) { if (c) { printf("  ok   %s\n", what); passed++; } else { printf("  FAIL %s\n", what); failed++; } }

int main() {
  printf("\nM1 normalising a MAC\n");
  ok("colons, upper", normMac("AA:BB:CC:DD:EE:FF") == "AA:BB:CC:DD:EE:FF");
  ok("lower and dashes", normMac("aa-bb-cc-dd-ee-ff") == "AA:BB:CC:DD:EE:FF");
  ok("bare", normMac("aabbccddeeff") == "AA:BB:CC:DD:EE:FF");
  ok("garbage is empty", normMac("not a mac").empty() && normMac("AA:BB").empty() && normMac("").empty());

  printf("\nM2 matched by MAC\n");
  {
    std::vector<Outlet> lost = {{"saw", "192.168.86.57", "AA:AA:AA:AA:AA:01"}};
    std::vector<Found> f = {{"192.168.86.90", "AA:AA:AA:AA:AA:01", true}, {"192.168.86.91", "AA:AA:AA:AA:AA:02", true}};
    auto r = match(lost, lost, f);
    ok("one result, the right address", r.size() == 1 && r[0].id == "saw" && r[0].ip == "192.168.86.90" && !r[0].byElimination);
    f[0].pickable = false;
    ok("never someone else's plug", match(lost, lost, f).empty());
    std::vector<Found> same = {{"192.168.86.57", "AA:AA:AA:AA:AA:01", true}};
    ok("found at the address it already has: nothing to change", match(lost, lost, same).empty());
    ok("not found at all: nothing", match(lost, lost, {}).empty());
  }

  printf("\nM3 no MAC on file: pair by elimination, and only when certain\n");
  {
    std::vector<Outlet> lost = {{"saw", "192.168.86.57", ""}};
    std::vector<Found> one = {{"192.168.86.90", "AA:AA:AA:AA:AA:01", true}};
    auto r = match(lost, lost, one);
    ok("one lost, one stranger: paired", r.size() == 1 && r[0].ip == "192.168.86.90" && r[0].mac == "AA:AA:AA:AA:AA:01" && r[0].byElimination);
    std::vector<Found> two = {one[0], {"192.168.86.91", "AA:AA:AA:AA:AA:02", true}};
    ok("two strangers: refuse to guess", match(lost, lost, two).empty());
    std::vector<Outlet> twoLost = {{"saw", "10.0.0.1", ""}, {"jointer", "10.0.0.2", ""}};
    ok("two lost, no MACs: refuse to guess", match(twoLost, twoLost, one).empty());
    std::vector<Found> notours = {{"192.168.86.90", "AA:AA:AA:AA:AA:01", false}};
    ok("a plug that belongs to someone else is not a candidate", match(lost, lost, notours).empty());
    std::vector<Outlet> all = {{"saw", "192.168.86.57", ""}, {"planer", "192.168.86.90", ""}};
    ok("a plug the layout already names is not a stranger", match(lost, all, one).empty());
    std::vector<Outlet> allMac = {{"saw", "192.168.86.57", ""}, {"planer", "192.168.86.77", "AA:AA:AA:AA:AA:01"}};
    ok("...even if it moved: its MAC says whose it is", match(lost, allMac, one).empty());
  }

  printf("\nM4 a MAC match uses up the plug\n");
  {
    std::vector<Outlet> lost = {{"saw", "10.0.0.1", "AA:AA:AA:AA:AA:01"}, {"jointer", "10.0.0.2", ""}};
    std::vector<Found> f = {{"10.0.0.9", "AA:AA:AA:AA:AA:01", true}};
    auto r = match(lost, lost, f);
    ok("the matched plug is not also offered to the one without a MAC", r.size() == 1 && r[0].id == "saw");
  }

  printf("\n%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}

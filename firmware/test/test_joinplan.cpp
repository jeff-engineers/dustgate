// test_joinplan.cpp — host tests for utils/JoinPlan.h: what a board does while it waits to join the WiFi.
#include "../utils/JoinPlan.h"
#include <cstdio>
using namespace joinplan;
static int passed = 0, failed = 0;
static void ok(const char* what, bool c) { if (c) { printf("  ok   %s\n", what); passed++; } else { printf("  FAIL %s\n", what); failed++; } }

int main() {
  printf("\nJ1 never associated\n");
  Attempt a; a.restart(1000);
  ok("waits at first", a.next(1000) == Act::Wait);
  ok("still waits just short of the timeout", a.next(1000 + kAssocTimeoutMs - 1) == Act::Wait);
  ok("opens the portal at the timeout", a.next(1000 + kAssocTimeoutMs) == Act::Portal);

  printf("\nJ2 associated, the router keeps offering a taken address\n");
  a.restart(0); a.associated = true;
  ok("one decline is not yet the router insisting", (a.declines = 1, a.next(11000)) == Act::Wait);
  ok("two declines switch the MAC", (a.declines = 2, a.next(21000)) == Act::SwitchMac);
  ok("declines win over the quiet timeout", a.next(kQuietDhcpMs + 5000) == Act::SwitchMac);

  printf("\nJ3 associated, DHCP silent\n");
  a.restart(500); a.associated = true;
  ok("waits past the association timeout", a.next(500 + kAssocTimeoutMs + 1) == Act::Wait);
  ok("never opens the portal once associated", a.next(500 + kQuietDhcpMs - 1) == Act::Wait);
  ok("rejoins when nothing answers", a.next(500 + kQuietDhcpMs) == Act::Rejoin);

  printf("\nJ4 a restart starts a fresh attempt\n");
  a.declines = 5; a.restart(90000);
  ok("not associated", !a.associated);
  ok("no declines carried over", a.declines == 0);
  ok("the clock starts again", a.next(90000 + 1000) == Act::Wait);

  printf("\nJ5 the board's twin MAC\n");
  const unsigned char mine[6] = { 0x38, 0x44, 0xBE, 0x1F, 0xFC, 0xB8 };
  unsigned char twin[6]; twinMac(mine, twin);
  ok("locally administered bit set (38 → 3A)", twin[0] == 0x3A);
  ok("still unicast", (twin[0] & 0x01) == 0);
  ok("the rest is the board's own", twin[1] == 0x44 && twin[2] == 0xBE && twin[3] == 0x1F && twin[4] == 0xFC && twin[5] == 0xB8);
  unsigned char again[6]; twinMac(mine, again);
  bool same = true; for (int i = 0; i < 6; i++) same = same && again[i] == twin[i];
  ok("the same twin every time", same);
  const unsigned char odd[6] = { 0x39, 0, 0, 0, 0, 1 };
  twinMac(odd, twin);
  ok("a multicast bit is cleared, never passed on", twin[0] == 0x3A);

  printf("\n%d/%d passed%s\n", passed, passed + failed, failed ? " — FAILED" : "");
  return failed ? 1 : 0;
}

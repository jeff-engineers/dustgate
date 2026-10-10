// =============================================================================
// test_clampjack.cpp — sensing/ClampJack.h: is a clamp plugged into the board's jack (2026-10-09).
// Build + run via tools/ script `firmware:clampjack:test`.
// =============================================================================
#include "../sensing/ClampJack.h"
#include <cstdio>

static int passed = 0, failed = 0;
static void ok(const char* name, bool cond) { printf("  %s %s\n", cond ? "✓" : "✗", name); cond ? passed++ : failed++; }

int main() {
  using sensing::ClampJack;
  ok("an empty jack (pulled to 3V3) is no clamp", !ClampJack::looksPlugged(3300));
  ok("the bias midpoint is a clamp", ClampJack::looksPlugged(1650));
  ok("railed LOW is no clamp either", !ClampJack::looksPlugged(50));
  ok("the thresholds are CtSensor::isRailed()'s", ClampJack::kLowRailMv == 200 && ClampJack::kHighRailMv == 3100);

  {
    int i = 0, delays = 0;
    const uint32_t m = ClampJack::probeMeanMv([&] { return (i++ % 2) ? 2650 : 650; }, [&](uint32_t) { delays++; });
    ok("a running tool swinging around the midpoint averages to it", m == 1650);
    ok("one read per sample, a gap between each", i == ClampJack::kSamples && delays == ClampJack::kSamples - 1);
  }

  {
    ClampJack j;
    ok("unknown before the first probe", !j.known() && j.due(0));
    ok("the first probe decides at once", j.update(1650, 0) && j.known() && j.plugged());
    ok("not due again for a second", !j.due(500) && j.due(1000));
    ok("one empty read does not flip it", !j.update(3300, 1000) && j.plugged());
    ok("a clean read in between cancels it", !j.update(1650, 2000) && j.plugged());
    ok("two empty reads in a row do", !j.update(3300, 3000) && j.update(3300, 4000) && !j.plugged());
    ok("and plugging it back in takes two as well", !j.update(1640, 5000) && j.update(1660, 6000) && j.plugged());
  }

  printf("%d/%d passed\n", passed, passed + failed);
  return failed ? 1 : 0;
}

// test_failsafestop.cpp — host tests for control/FailsafeStop.h: the collector's board stopping its blower with the brain gone.
#include "../control/FailsafeStop.h"
#include <cstdio>
using namespace topo;
static int passed = 0, failed = 0;
static void ok(const char* what, bool c) { if (c) { printf("  ok   %s\n", what); passed++; } else { printf("  FAIL %s\n", what); failed++; } }

int main() {
  printf("\nS stopping the blower\n");
  { CollectorStopper s;
    ok("S1 not armed: nothing", s.tick(1000, true, true) == StopAction::Nothing);
    s.arm();
    ok("S2 armed, no clamp reading yet: never press blind (a press is a toggle)", s.tick(1000, false, true) == StopAction::Nothing);
    ok("S3 the clamp says it runs: press", s.tick(2000, true, true) == StopAction::Press);
    ok("S4 inside the off-press cooldown: wait, whatever the reading", s.tick(2000 + kPressCooldownOffMs - 1, true, true) == StopAction::Nothing);
    ok("S5 after it, still running: press again", s.tick(2000 + kPressCooldownOffMs, true, true) == StopAction::Press);
    ok("S6 after the next, off: stopped, said once", s.tick(2000 + 2 * kPressCooldownOffMs, true, false) == StopAction::Stopped &&
                                                     s.tick(2000 + 3 * kPressCooldownOffMs, true, true) == StopAction::Nothing); }
  { CollectorStopper s; s.arm();
    ok("S7 already off when armed: stopped, no press", s.tick(1000, true, false) == StopAction::Stopped && s.presses() == 0); }
  { CollectorStopper s; s.arm();
    uint32_t t = 1000;
    for (int i = 0; i < kMaxPressAttempts; i++) { s.tick(t, true, true); t += kPressCooldownOffMs; }
    ok("S8 the brain's attempt limit, then it gives up (said once)", s.presses() == kMaxPressAttempts &&
       s.tick(t, true, true) == StopAction::GaveUp && s.tick(t + kPressCooldownOffMs, true, true) == StopAction::Nothing); }
  { CollectorStopper s; s.arm(); s.tick(1000, true, true); s.disarm();
    ok("S9 the brain is back: disarmed, nothing more", s.tick(1000 + kPressCooldownOffMs, true, true) == StopAction::Nothing); }
  printf("\n%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}

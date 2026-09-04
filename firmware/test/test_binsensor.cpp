// =============================================================================
// test_binsensor.cpp — host tests for utils/BinSensor.h.
//
// NOT half of an anti-drift pair, and that is worth stating (CLAUDE.md's rule
// about saying so). The debounce here has no JS partner because no JS model
// simulates a flickering beam — the mock and demo STAGE bin state directly, the
// way they stage a plug fault, so `kBinDebounceMs` exists once and has nothing
// to drift against. The same shape as kMoveTimeoutMs.
//
// What DOES have a partner is the reported SHAPE — `systems[].bin` present or
// omitted — and that is asserted from the JS side in bin-sensor.test.js against
// statusView(), and from here against writeStatus(). Same rule, two engines,
// which is the arrangement collector-plug.test.js already uses.
//
// Build + run via tools/ script `firmware:bin:test`.
// =============================================================================

#include <ArduinoJson.h>
#include "../utils/BinSensor.h"
#include <cstdio>
#include <string>

static int passed = 0, failed = 0;
static void ok(const char* what, bool cond, const std::string& got = "") {
    if (cond) { printf("  ok   %s\n", what); passed++; }
    else      { printf("  FAIL %s%s%s\n", what,
                       got.empty() ? "" : "  got: ", got.c_str()); failed++; }
}

using namespace topo;

int main() {
    printf("\nB1 the first reading is believed immediately\n");
    {
        // A board that boots with a full bin must say so, not claim OK for two
        // seconds. Seeding is the whole reason BinDebounce has a `seeded` flag.
        BinDebounce d;
        ok("nothing sampled yet → not seeded", !d.seeded());
        d.sample(true, 1000);
        ok("first sample is taken at face value", d.full());
        ok("...and it is now seeded",            d.seeded());
    }
    {
        BinDebounce d;
        d.sample(false, 1000);
        ok("boots empty → reports empty", !d.full());
    }

    printf("\nB2 a change has to hold for kBinDebounceMs\n");
    {
        BinDebounce d;
        d.sample(false, 0);
        d.sample(true, 100);
        ok("a new reading is not believed at once",      !d.full());
        d.sample(true, 100 + kBinDebounceMs - 1);
        ok("...nor one millisecond early",               !d.full());
        d.sample(true, 100 + kBinDebounceMs);
        ok("...and is believed exactly on time",          d.full());
    }

    printf("\nB3 chatter never gets through\n");
    {
        // The case the debounce exists for: chips swirling past the beam. Each
        // flip restarts the clock, so a bin that is not really full never reads
        // full however long the flickering goes on.
        BinDebounce d;
        d.sample(false, 0);
        uint32_t t = 0;
        for (int i = 0; i < 50; i++) {           // 50 flips, 100 ms apart
            t += 100;
            d.sample((i % 2) == 0, t);
        }
        ok("5 seconds of chatter, still not full", !d.full());
        // ...and once it settles, it lands.
        for (uint32_t s = t; s <= t + kBinDebounceMs; s += 100) d.sample(true, s);
        ok("...but a steady reading after it does", d.full());
    }

    printf("\nB4 it follows the bin back down — no latch\n");
    {
        // DELIBERATE: emptying the bin clears the trip without anyone
        // acknowledging it, because there is nowhere to acknowledge from yet.
        // If this test ever needs changing, §7.5's open question got answered.
        BinDebounce d;
        d.sample(true, 0);
        ok("full", d.full());
        d.sample(false, 100);
        d.sample(false, 100 + kBinDebounceMs);
        ok("emptied, and it clears on its own", !d.full());
    }

    printf("\nB5 whose bin is it\n");
    {
        StaticJsonDocument<2048> doc;
        deserializeJson(doc, R"({"systems":[
          {"id":"sysA","elements":[{"id":"dcA","type":"collector",
             "bin":{"sensor":{"kind":"threshold","controllerId":"node-dc"}}}]},
          {"id":"sysB","elements":[{"id":"dcB","type":"collector"}]}
        ]})");
        JsonObjectConst t = doc.as<JsonObjectConst>();

        ok("the board named on the sensor owns it",
           localBinSystemId(t, "node-dc") == "sysA",
           localBinSystemId(t, "node-dc"));
        ok("another board owns nothing",
           localBinSystemId(t, "primary").empty(),
           localBinSystemId(t, "primary"));
        ok("a collector with no bin is absent, not empty",
           localBinSystemId(t, "dcB").empty());
    }
    {
        // Mirrors NodeBus: no controllerId means local. Single-board shops never
        // name a controller, and every pre-multi-node topology predates the idea.
        StaticJsonDocument<1024> doc;
        deserializeJson(doc, R"({"systems":[
          {"id":"only","elements":[{"id":"dc","type":"collector",
             "bin":{"sensor":{"kind":"threshold"}}}]}
        ]})");
        ok("no controllerId → this board's bin",
           localBinSystemId(doc.as<JsonObjectConst>(), "primary") == "only");
    }
    {
        StaticJsonDocument<512> doc;
        deserializeJson(doc, R"({"elements":[{"id":"dc","type":"collector"}]})");
        ok("a v1 document has no systems array and no bin",
           localBinSystemId(doc.as<JsonObjectConst>(), "primary").empty());
    }

    printf("\n%d/%d passed\n", passed, passed + failed);
    return failed == 0 ? 0 : 1;
}

// test_relocate.cpp — host tests for control/OutletRelocate.h.
#include "../control/OutletRelocate.h"
#include <cstdio>
#include <map>
#include <string>
#include <vector>
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

  printf("\nM5 elimination never crosses kinds\n");
  {
    std::vector<Outlet> lost = {{"saw", "192.168.86.64", "", ""}};                         // a Shelly, no MAC
    std::vector<Found> tas = {{"192.168.86.80", "B8:F0:09:10:BD:28", true, "tasmota"}};
    ok("one lost Shelly, one stranger Tasmota: not paired", match(lost, lost, tas).empty());
    std::vector<Found> both = {tas[0], {"192.168.86.70", "AC:EB:E6:F3:CB:34", true, "shelly"}};
    auto r = match(lost, lost, both);
    ok("...a Shelly stranger beside it is the one", r.size() == 1 && r[0].ip == "192.168.86.70" && r[0].kind == "shelly");
  }

  printf("\nM6 a MAC match that changed kind (reflashed) is reported even at the same address\n");
  {
    std::vector<Outlet> lost = {{"saw", "10.0.0.5", "AA:AA:AA:AA:AA:01", ""}};
    std::vector<Found> f = {{"10.0.0.5", "AA:AA:AA:AA:AA:01", true, "tasmota"}};
    auto r = match(lost, lost, f);
    ok("rewritten to a Tasmota", r.size() == 1 && r[0].ip == "10.0.0.5" && r[0].kind == "tasmota");
  }

  // ── S: samePlug — the matched pair with shared/device-model/plug-identity.test.js. Same cases, same order. ──
  printf("\nS samePlug\n");
  ok("S1 nothing answered: unknown", samePlug("tasmota", "AA:AA:AA:AA:AA:01", false, "", "") == Same::Unknown);
  ok("S2 both MACs, equal: yes", samePlug("tasmota", "aa:aa:aa:aa:aa:01", true, "tasmota", "AAAAAAAAAA01") == Same::Yes);
  ok("S3 both MACs, different: no", samePlug("tasmota", "AA:AA:AA:AA:AA:01", true, "tasmota", "AA:AA:AA:AA:AA:02") == Same::No);
  ok("S4 both MACs equal, kinds differ (reflashed): yes", samePlug("", "AA:AA:AA:AA:AA:01", true, "tasmota", "AA:AA:AA:AA:AA:01") == Same::Yes);
  ok("S5 no MAC on file, a Tasmota where a Shelly was saved: no", samePlug("", "", true, "tasmota", "B8:F0:09:10:BD:28") == Same::No);
  ok("S6 no MAC on file, same kind: unknown", samePlug("tasmota", "", true, "tasmota", "B8:F0:09:10:BD:28") == Same::Unknown);
  ok("S7 a MAC on file, the plug would not say one, same kind: unknown", samePlug("tasmota", "AA:AA:AA:AA:AA:01", true, "tasmota", "") == Same::Unknown);
  ok("S8 a MAC on file, the plug would not say one, other kind: no", samePlug("shelly", "AA:AA:AA:AA:AA:01", true, "tasmota", "") == Same::No);
  ok("S9 an absent kind is a Shelly", samePlug("", "", true, "shelly", "") == Same::Unknown);

  // ── P: the policy both brains run ──
  struct Fake : Probe {
    std::map<std::string, Seen> at; std::vector<Found> rows; uint32_t done = 0; bool running = false; int starts = 0, asks = 0;
    Seen describe(const std::string& ip) override { asks++; auto it = at.find(ip); return it == at.end() ? Seen() : it->second; }
    bool sweepRunning() override { return running; }
    bool startSweep() override { starts++; running = true; return true; }
    uint32_t sweepsFinished() override { return done; }
    std::vector<Found> sweepRows() override { return rows; }
    void finish(std::vector<Found> r) { rows = r; running = false; done++; }
  };
  const std::string M1 = "B8:F0:09:10:BD:76", M2 = "B8:F0:09:10:BD:28", SAW = "AC:EB:E6:F3:CB:34";

  printf("\nP1 a plug with a MAC on file is checked before it is trusted\n");
  {
    Fake f; f.at["10.0.0.63"] = {true, "tasmota", M1};
    PlugRelocator r(8);
    std::vector<Plug> plugs = {{"m:miter", "10.0.0.63", "tasmota", M1, false}};
    ok("not ticked yet, a MAC on file: not trusted", !r.trusted("m:miter", M1));
    ok("not ticked yet, no MAC to check: trusted", r.trusted("m:other", ""));
    auto w = r.tick(plugs, f, 1000);
    ok("checked on the first tick, no rewrite", w.empty() && f.asks == 1);
    ok("trusted once its MAC answered", r.trusted("m:miter"));
    plugs[0].reachable = true;
    r.tick(plugs, f, 2000);
    ok("not asked again straight away", f.asks == 1);
    r.tick(plugs, f, 1000 + kReverifyMs);
    ok("asked again after kReverifyMs", f.asks == 2 && r.trusted("m:miter"));
  }

  printf("\nP2 2026-10-18: the Table Saw's Shelly left .64 and a new Tasmota was leased it\n");
  {
    Fake f; f.at["10.0.0.64"] = {true, "tasmota", M2};
    PlugRelocator r(8);
    std::vector<Plug> plugs = {{"m:saw", "10.0.0.64", "", "", false}, {"m:miter", "10.0.0.63", "tasmota", M1, true}};
    f.at["10.0.0.63"] = {true, "tasmota", M1};
    r.tick(plugs, f, 1000);
    ok("a plug with no MAC is trusted until shown otherwise", r.trusted("m:saw"));
    r.tick(plugs, f, 1000 + kProbeSilentAfterMs);
    ok("silent a minute: the address is asked, and a Tasmota is not the saved Shelly", !r.trusted("m:saw") && r.replacedBy("m:saw").find("tasmota") != std::string::npos);
    ok("the Miter Saw is untouched", r.trusted("m:miter") && r.replacedBy("m:miter").empty());
    ok("a replaced plug is lost at once: a sweep starts", f.starts == 1);

    printf("\nP3 ...the sweep finds the Shelly somewhere else, and the Tasmota at .64 is not handed to the saw\n");
    f.finish({{"10.0.0.64", M2, true, "tasmota"}, {"10.0.0.63", M1, true, "tasmota"}, {"10.0.0.70", SAW, true, "shelly"}});
    auto w = r.tick(plugs, f, 2000 + kProbeSilentAfterMs);
    ok("the saw is pointed at its Shelly, by elimination", w.size() == 1 && w[0].key == "m:saw" && w[0].ip == "10.0.0.70" && w[0].kind == "shelly" && w[0].mac == SAW);
    plugs[0].ip = "10.0.0.70"; plugs[0].mac = SAW; plugs[0].reachable = true;
    r.tick(plugs, f, 3000 + kProbeSilentAfterMs);
    ok("once the layout takes it, the saw is trusted without waiting for a check", r.trusted("m:saw") && r.replacedBy("m:saw").empty());
  }

  printf("\nP4 two Tasmotas swap leases overnight; both answer, each as the other\n");
  {
    Fake f; f.at["10.0.0.63"] = {true, "tasmota", M2}; f.at["10.0.0.64"] = {true, "tasmota", M1};
    PlugRelocator r(8);
    std::vector<Plug> plugs = {{"m:miter", "10.0.0.63", "tasmota", M1, true}, {"m:jointer", "10.0.0.64", "tasmota", M2, true}};
    r.tick(plugs, f, 1000);
    ok("neither is trusted: each reads the other's tool", !r.trusted("m:miter") && !r.trusted("m:jointer"));
    f.finish({{"10.0.0.63", M2, true, "tasmota"}, {"10.0.0.64", M1, true, "tasmota"}});
    auto w = r.tick(plugs, f, 2000);
    bool a = false, b = false;
    for (auto& x : w) { if (x.key == "m:miter" && x.ip == "10.0.0.64") a = true; if (x.key == "m:jointer" && x.ip == "10.0.0.63") b = true; }
    ok("both are pointed at their own plug by MAC", w.size() == 2 && a && b);
  }

  printf("\nP4a the replaced plug's own plug comes back to its address\n");
  {
    Fake f; f.at["10.0.0.63"] = {true, "tasmota", M2};
    PlugRelocator r(8);
    std::vector<Plug> plugs = {{"m:miter", "10.0.0.63", "tasmota", M1, false}};
    r.tick(plugs, f, 1000);
    ok("replaced", !r.trusted("m:miter") && !r.replacedBy("m:miter").empty());
    f.running = true;   // a sweep is busy: no rows to settle it
    f.at["10.0.0.63"] = {true, "tasmota", M1};
    r.tick(plugs, f, 1000 + kProbeSilentAfterMs / 2);
    ok("not asked again inside a minute", !r.trusted("m:miter"));
    r.tick(plugs, f, 1000 + kProbeSilentAfterMs);
    ok("asked again a minute on: its own MAC, so trusted and no longer replaced", r.trusted("m:miter") && r.replacedBy("m:miter").empty());
  }

  printf("\nP5 a plug paired with no MAC gets one recorded\n");
  {
    Fake f; f.at["10.0.0.64"] = {true, "tasmota", M2};
    PlugRelocator r(8);
    std::vector<Plug> plugs = {{"m:jointer", "10.0.0.64", "tasmota", "", true}};
    auto w = r.tick(plugs, f, 1000);
    ok("backfilled", w.size() == 1 && w[0].mac == M2 && w[0].ip == "10.0.0.64" && w[0].kind == "tasmota");
    plugs[0].mac = M2;
    r.tick(plugs, f, 2000);
    ok("still trusted once the MAC lands", r.trusted("m:jointer") && f.asks == 1);
  }

  printf("\nP6 the same plug, reflashed from Shelly to Tasmota\n");
  {
    Fake f; f.at["10.0.0.65"] = {true, "tasmota", SAW};
    PlugRelocator r(8);
    std::vector<Plug> plugs = {{"m:drum", "10.0.0.65", "", SAW, false}};
    auto w = r.tick(plugs, f, 1000);
    ok("the layout is told it is a Tasmota now", w.size() == 1 && w[0].kind == "tasmota" && w[0].mac == SAW && w[0].ip == "10.0.0.65");
  }

  printf("\nP7 a plug switched off is not replaced, and is swept for at most every kRelocateMinGapMs\n");
  {
    Fake f;
    PlugRelocator r(8);
    std::vector<Plug> plugs = {{"m:planer", "10.0.0.66", "tasmota", M1, false}};
    r.tick(plugs, f, 1000);
    r.tick(plugs, f, 1000 + kProbeSilentAfterMs);
    ok("nothing answered: not replaced", r.replacedBy("m:planer").empty() && f.starts == 0);
    r.tick(plugs, f, 1000 + kRelocateAfterMs);
    ok("silent five minutes: one sweep", f.starts == 1);
    f.finish({});
    r.tick(plugs, f, 2000 + kRelocateAfterMs);
    r.tick(plugs, f, 3000 + kRelocateAfterMs);
    ok("not again straight away", f.starts == 1);
    r.tick(plugs, f, 1000 + kRelocateAfterMs + kRelocateMinGapMs);
    ok("again after the gap", f.starts == 2);
  }

  printf("\nP8 a sweep a person started is used too\n");
  {
    Fake f;
    PlugRelocator r(8);
    std::vector<Plug> plugs = {{"m:planer", "10.0.0.66", "tasmota", M1, false}};
    r.tick(plugs, f, 1000);
    f.finish({{"10.0.0.90", M1, true, "tasmota"}});
    auto w = r.tick(plugs, f, 1000 + kRelocateAfterMs);
    ok("found without a sweep of our own", w.size() == 1 && w[0].ip == "10.0.0.90" && f.starts == 0);
  }

  printf("\nR applyRewrite\n");
  {
    DynamicJsonDocument d(2048);
    deserializeJson(d, R"({"machines":[{"id":"saw","sensor":{"outlet":{"ip":"10.0.0.64","gen":2,"host":"Sawstop"}}}],
      "systems":[{"id":"system-1","elements":[{"id":"dc","type":"collector","sensor":{"outlet":{"ip":"10.0.0.1","kind":"tasmota","gen":0}},
      "control":{"outlet":{"ip":"10.0.0.2","gen":2}}}]}]})");
    JsonObject root = d.as<JsonObject>();
    ok("a machine's plug to a Tasmota", applyRewrite(root, {"m:saw", "10.0.0.80", "tasmota", M2, ""}) &&
       std::string(root["machines"][0]["sensor"]["outlet"]["ip"] | "") == "10.0.0.80" &&
       std::string(root["machines"][0]["sensor"]["outlet"]["kind"] | "") == "tasmota" &&
       (root["machines"][0]["sensor"]["outlet"]["gen"] | -1) == 0 &&
       std::string(root["machines"][0]["sensor"]["outlet"]["mac"] | "") == M2);
    JsonObject dc = root["systems"][0]["elements"][0];
    ok("a collector's sense plug to a Shelly drops the kind and gets a generation", applyRewrite(root, {"c:system-1", "10.0.0.3", "shelly", SAW, ""}) &&
       dc["sensor"]["outlet"]["kind"].isNull() && (dc["sensor"]["outlet"]["gen"] | 0) == 2 && std::string(dc["sensor"]["outlet"]["ip"] | "") == "10.0.0.3");
    ok("a collector's switch plug", applyRewrite(root, {"s:system-1", "10.0.0.4", "", "", ""}) && std::string(dc["control"]["outlet"]["ip"] | "") == "10.0.0.4");
    ok("an unknown key changes nothing", !applyRewrite(root, {"m:nobody", "10.0.0.9", "", "", ""}));
  }

  printf("\n%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}

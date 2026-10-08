// test_apicore.cpp — host tests for api/ApiCore.h: what each shared route accepts, refuses and asks of the backend.
#include "../api/ApiCore.h"
#include <cstdio>
#include <vector>
using namespace api;
static int passed = 0, failed = 0;
static void ok_(const char* what, bool c) { if (c) { printf("  ok   %s\n", what); passed++; } else { printf("  FAIL %s\n", what); failed++; } }

struct Fake : Backend {
  std::vector<std::string> calls; bool known = true; bool jogOk = true; std::string jogWhy;
  bool setToolManual(const std::string& id, bool on) override { calls.push_back("tool:" + id + (on ? ":1" : ":0")); return known; }
  bool setCollectorManual(const std::string& s, bool on) override { calls.push_back("coll:" + s + (on ? ":1" : ":0")); return known; }
  bool jog(const std::string& c, int ch, int a, bool d, std::string& why) override {
    calls.push_back("jog:" + c + ":" + std::to_string(ch) + ":" + std::to_string(a) + (d ? ":detach" : "")); why = jogWhy; return jogOk; }
  void resetAll() override { calls.push_back("reset"); }
  void pairNode(const std::string& h, const std::string& n, bool r, bool t) override { calls.push_back("pair:" + h + ":" + n + (r ? ":remove" : "") + (t ? ":takeover" : "")); }
  void pauseLinks(bool p) override { calls.push_back(p ? "pause" : "resume"); }
  bool linOk = true; bool moving = false;
  bool linearGoto(const std::string& id, float mm, bool home, std::string& why) override {
    calls.push_back("lin:" + id + ":" + std::to_string((int)(mm * 10)) + (home ? ":home" : "")); why = "a tool is running"; return linOk; }
  bool gateMoving() override { return moving; }
};
static Response run(Fake& f, const char* m, const char* path, const char* body = "", bool* mine = nullptr) {
  Request r; r.method = m; r.path = path; r.body = body; Response out; const bool h = handle(r, f, out); if (mine) *mine = h; return out;
}

int main() {
  printf("\nA1 manual switches\n");
  { Fake f;
    ok_("a tool on is passed to the backend", run(f, "POST", "/api/tool", R"({"toolId":"t1","on":true})").status == 200 && f.calls.back() == "tool:t1:1");
    ok_("a missing toolId is a 400", run(f, "POST", "/api/tool", R"({"on":true})").status == 400);
    ok_("a missing 'on' is a 400", run(f, "POST", "/api/tool", R"({"toolId":"t1"})").status == 400);
    ok_("bad JSON is a 400", run(f, "POST", "/api/tool", "{").status == 400);
    f.known = false;
    ok_("an unknown tool is a 404, not a silent ok", run(f, "POST", "/api/tool", R"({"toolId":"zz","on":false})").status == 404);
    f.known = true; f.calls.clear();
    ok_("the collector takes an optional system", run(f, "POST", "/api/collector", R"({"systemId":"s2","on":true})").status == 200 && f.calls.back() == "coll:s2:1");
    ok_("...and none means the first", (run(f, "POST", "/api/collector", R"({"on":false})"), f.calls.back() == "coll::0"));
    ok_("the old collector switch still works", (run(f, "POST", "/api/dustcollector/switch", R"({"on":true})"), f.calls.back() == "coll::1"));
  }
  printf("\nA2 a servo jog\n");
  { Fake f;
    ok_("a jog carries board, channel and angle", run(f, "POST", "/api/servo/jog", R"({"controllerId":"n1","channel":1,"angle":90})").status == 200 && f.calls.back() == "jog:n1:1:90");
    ok_("a detach needs no angle", run(f, "POST", "/api/servo/jog", R"({"controllerId":"n1","channel":0,"detach":true})").status == 200 && f.calls.back() == "jog:n1:0:0:detach");
    ok_("an angle outside 0-180 is refused before it reaches a servo", run(f, "POST", "/api/servo/jog", R"({"channel":0,"angle":181})").status == 400);
    ok_("a negative channel is refused", run(f, "POST", "/api/servo/jog", R"({"channel":-1,"angle":10})").status == 400);
    ok_("a missing angle is refused", run(f, "POST", "/api/servo/jog", R"({"channel":0})").status == 400);
    f.jogOk = false; f.jogWhy = "no servo support on this brain";
    ok_("a brain with no servos of its own says 501", run(f, "POST", "/api/servo/jog", R"({"channel":0,"angle":10})").status == 501);
    f.jogWhy = "that board is not linked";
    ok_("a board that is down is a 502", run(f, "POST", "/api/servo/jog", R"({"controllerId":"n9","channel":0,"angle":10})").status == 502);
  }
  printf("\nA3 boards and the rest\n");
  { Fake f;
    ok_("pairing passes name, remove and takeover", (run(f, "POST", "/api/nodes/pair", R"({"host":"n1","name":"Back wall","takeover":true})"), f.calls.back() == "pair:n1:Back wall:takeover"));
    ok_("an unpair is a pair with remove", (run(f, "POST", "/api/nodes/pair", R"({"host":"n1","remove":true})"), f.calls.back() == "pair:n1::remove"));
    ok_("pairing needs a host", run(f, "POST", "/api/nodes/pair", R"({"name":"x"})").status == 400);
    ok_("pause takes a boolean", (run(f, "POST", "/api/nodes/pause", R"({"paused":true})"), f.calls.back() == "pause"));
    ok_("...and refuses anything else", run(f, "POST", "/api/nodes/pause", R"({"paused":"yes"})").status == 400);
    ok_("reset-all reaches the backend", (run(f, "POST", "/api/reset-all"), f.calls.back() == "reset"));
    ok_("discovery with nothing found is an empty list", run(f, "GET", "/api/nodes/discover").body == "[]");
    ok_("an update the backend does not offer is a 409 with the reason", run(f, "POST", "/api/nodes/update", R"({"id":"n1"})").status == 409);
    ok_("slider routes are a 501 on a brain with no rack", run(f, "POST", "/api/home").status == 501 && run(f, "GET", "/api/motion").status == 501);
    ok_("...but Settings' reset-calibration is a harmless no-op, not an error toast", run(f, "POST", "/api/clearcal").status == 200);
    ok_("forgetting Wi-Fi is refused: there is none to forget", run(f, "POST", "/api/wifi/reset").status == 501);
    bool mine = true; run(f, "GET", "/api/topology", "", &mine);
    ok_("a route it does not own is left to the shell", !mine);
  }
  printf("\nA4 a slider on a node, set up by hand\n");
  { Fake f;
    ok_("a goto carries the gate and the distance", run(f, "POST", "/api/linear/goto", R"({"selectorId":"man","mm":83.5})").status == 200 && f.calls.back() == "lin:man:835");
    ok_("a whole number of mm is fine", run(f, "POST", "/api/linear/goto", R"({"selectorId":"man","mm":0})").status == 200 && f.calls.back() == "lin:man:0");
    ok_("home first is passed on", run(f, "POST", "/api/linear/goto", R"({"selectorId":"man","mm":1,"home":true})").status == 200 && f.calls.back() == "lin:man:10:home");
    ok_("a goto needs a gate", run(f, "POST", "/api/linear/goto", R"({"mm":10})").status == 400);
    ok_("...and a distance", run(f, "POST", "/api/linear/goto", R"({"selectorId":"man"})").status == 400);
    f.linOk = false;
    { Response r = run(f, "POST", "/api/linear/goto", R"({"selectorId":"man","mm":10})");
      ok_("a refusal is a 409 that says why", r.status == 409 && r.body.find("a tool is running") != std::string::npos); }
    ok_("still moving reads true", (f.moving = true, run(f, "GET", "/api/linear/state").body == R"({"moving":true})"));
    ok_("...and done reads false", (f.moving = false, run(f, "GET", "/api/linear/state").body == R"({"moving":false})"));
    Backend plain; Request r; r.method = "POST"; r.path = "/api/linear/goto"; r.body = R"({"selectorId":"man","mm":10})"; Response out;
    ok_("a brain that cannot drive one refuses rather than pretends", handle(r, plain, out) && out.status == 409);
  }
  printf("\nA5 query parameters\n");
  { Request r; r.query = "kind=pwm&from=12";
    ok_("a parameter is found", r.param("kind") == "pwm" && r.param("from") == "12");
    ok_("an absent one is empty", r.param("old").empty());
  }
  printf("\n%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}

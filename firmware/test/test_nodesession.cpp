// test_nodesession.cpp — host tests for control/NodeSession.h: the primary's side of one node link,
// with no socket in it. This logic used to live inside RemoteActuatorBus and had NO host tests at all
// (only a hardware soak); every case here was a thing that had to be learnt on a bench.
#include "../control/NodeSession.h"
#include "../control/NodeStatus.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace topo;
static int passed = 0, failed = 0;
static void ok(const char* what, bool c) { if (c) { printf("  ok   %s\n", what); passed++; } else { printf("  FAIL %s\n", what); failed++; } }

static uint32_t g_now = 1000;
static uint32_t clk() { return g_now; }
struct RecSink : SessionSink {
  std::vector<std::string> lines, events;
  void say(const char* l) override { lines.push_back(l); }
  void linkEvent(const char* e, const char* x) override { events.push_back(std::string(e) + "|" + (x ? x : "")); }
  bool saw(const char* needle) const { for (auto& l : lines) if (l.find(needle) != std::string::npos) return true; return false; }
  bool event(const char* ev) const { for (auto& e : events) if (e.rfind(ev, 0) == 0) return true; return false; }
};

static const char* kWelcome = R"({"t":"WELCOME","v":1,"nodeId":"n1","board":"xiao_c5","fw":"abc 1004-1200","caps":{"servos":2,"linear":0,"ct":1,"plug":1,"join":1,"rf":1,"bin":1},"upS":42,"rst":"poweron"})";

static JsonObjectConst sel(DynamicJsonDocument& d) {
  deserializeJson(d, R"({"id":"g1","kind":"servoGate","controllerId":"n1","servo":{"channel":0,"referenceAngle":90,"minAngle":0,"maxAngle":180},
    "states":[{"id":"open","offsetDeg":0},{"id":"closed","isClosed":true,"offsetDeg":-70}]})");
  return d.as<JsonObjectConst>();
}

struct Fx {
  RecSink sink; NodeSession s;
  Fx() : s(clk, &sink) { g_now = 1000; s.configure("n1", "dustgate"); }
  void up() { std::string w = kWelcome; s.onFrame(w.c_str(), w.size()); }
  bool drain(std::string& out) { return s.nextFrame(out); }
  void feed(const char* json) { s.onFrame(json, strlen(json)); }
};

int main() {
  printf("\nS1 the handshake\n");
  {
    Fx f;
    ok("offline before a WELCOME", !f.s.online() && !f.s.connected());
    ok("the HELLO carries our id and the claim", f.s.helloFrame().find("\"primaryId\":\"dustgate\"") != std::string::npos);
    ok("...with no takeover unless asked", f.s.helloFrame().find("takeover") == std::string::npos);
    f.s.requestTakeover();
    ok("a requested takeover rides ONE hello", f.s.helloFrame().find("\"takeover\":true") != std::string::npos);
    ok("...and is not repeated", f.s.helloFrame().find("takeover") == std::string::npos);
    f.up();
    ok("an accepted WELCOME brings the link up", f.s.online() && f.s.connected());
    ok("...and logs link_up with the node's own account of its boot",
       f.sink.event("link_up") && f.sink.events.back().find("\"nodeUpS\":42") != std::string::npos &&
       f.sink.events.back().find("\"nodeRst\":\"poweron\"") != std::string::npos);
    NodeSession::NodeInfo i = f.s.info();
    ok("the board and firmware are recorded", std::string(i.board) == "xiao_c5" && std::string(i.fw) == "abc 1004-1200");
    ok("every capability is read", i.capServos == 2 && i.capClamps == 1 && f.s.pollsPlugs() && f.s.dialsIn() && f.s.canPressRf() && f.s.watchesBin());
  }

  printf("\nS1b the /api/nodes entry\n");
  {
    Fx f; f.up();
    DynamicJsonDocument d(2048);
    // Built in its own scope and serialised AFTER the builder's locals are gone: a char[] copied by
    // pointer instead of by value reads fine inside the call and is garbage here.
    topo::NodeImageView iv; iv.present = true; iv.fw = "other 1";
    topo::writeNodeEntry(d.createNestedArray("nodes"), f.s, "n1", "n1.local", "Back wall", iv);
    std::string out; serializeJson(d, out);
    ok("the board and firmware survive the builder", out.find("\"board\":\"xiao_c5\"") != std::string::npos && out.find("\"fw\":\"abc 1004-1200\"") != std::string::npos);
    ok("a clamp is reported only when the board has one", out.find("\"ct\":1") != std::string::npos);
    ok("a different image on offer says an update is due", out.find("\"update\":true") != std::string::npos);
    ok("no OTA fields while nothing is updating", out.find("\"ota\"") == std::string::npos);
  }

  printf("\nS2 capabilities absent mean NO\n");
  {
    Fx f;
    f.feed(R"({"t":"WELCOME","v":1,"nodeId":"n1","board":"b","fw":"f","caps":{"servos":2,"linear":0}})");
    ok("an old board has none of the newer jobs", !f.s.pollsPlugs() && !f.s.dialsIn() && !f.s.canPressRf() && !f.s.watchesBin());
    ok("and a PRESS to it is refused", !f.s.pressRf(94, 14, 270, 24));
  }

  printf("\nS3 a refusal and a version mismatch\n");
  {
    Fx f;
    f.feed(R"({"t":"WELCOME","v":1,"nodeId":"n1","board":"b","fw":"f","caps":{"servos":2,"linear":0},"accepted":false,"claimedBy":"garage"})");
    ok("a refused claim leaves the link offline and names the owner", !f.s.online() && f.s.wasRefused() && std::string(f.s.refusedBy()) == "garage");
    ok("...and logs it", f.sink.event("refused"));
    ok("health says refused", f.s.health().refused);
    Fx g;
    g.feed(R"({"t":"WELCOME","v":9,"nodeId":"n1","board":"b","fw":"f","caps":{"servos":2,"linear":0}})");
    ok("a different protocol version is never half-understood", !g.s.online() && g.sink.saw("Version mismatch"));
  }

  printf("\nS4 freshness: heartbeat counts, silence does not\n");
  {
    Fx f; f.up();
    g_now += nodelink::kPongTimeoutMs - 100;
    ok("fresh just inside the pong timeout", f.s.online());
    g_now += 200;
    ok("offline once the node has been silent past it", !f.s.online());
    f.s.onPong();
    ok("a pong makes it fresh again with no TEXT frame at all", f.s.online());
  }

  printf("\nS5 a move: queued, in flight, arrived, timed out, refused\n");
  {
    Fx f; f.up();
    DynamicJsonDocument d(1024);
    ok("an offline session refuses to move", [&] { Fx o; return !o.s.setState("g1", sel(d), "open"); }());
    ok("a calibrated selector is accepted", f.s.setState("g1", sel(d), "open"));
    ok("the bus is busy until the node answers", f.s.busy());
    std::string frame;
    ok("the SET goes out", f.drain(frame) && frame.find("\"t\":\"SET\"") != std::string::npos && frame.find("\"angle\":90") != std::string::npos);
    ok("...resolved to a number, with no state name in it", frame.find("\"angle\"") != std::string::npos);
    ok("nothing more to send", !f.drain(frame));
    ok("still busy while the move is outstanding", f.s.busy());
    f.feed(R"({"t":"STATE","selectorId":"g1","stateId":"open","moving":false})");
    ok("arrival frees the bus", !f.s.busy());
    f.s.setState("g1", sel(d), "closed"); f.drain(frame);
    f.feed(R"({"t":"ACK","seq":99,"ok":false,"err":"no"})");
    ok("a refused move stops the wait and says why", !f.s.busy() && f.s.health().moveFault != nullptr);
    f.s.setState("g1", sel(d), "open"); f.drain(frame);
    g_now += nodelink::kMoveTimeoutMs + 1000;
    f.s.onPong();
    f.s.tick();
    ok("a STATE that never comes times the move out", !f.s.busy() && f.s.health().moveFault != nullptr);
    f.s.setState("g1", sel(d), "closed"); f.drain(frame);
    f.s.onDown();
    ok("a dropped link mid-move frees the bus and says the gate may not have finished", !f.s.busy() && f.s.health().moveFault != nullptr);
    DynamicJsonDocument d2(1024);
    deserializeJson(d2, R"({"id":"g2","kind":"servoGate","controllerId":"n1","servo":{"channel":0},"states":[{"id":"open"}]})");
    Fx u; u.up();
    ok("an uncalibrated servo is refused rather than sent a guess", !u.s.setState("g2", d2.as<JsonObjectConst>(), "open"));
  }

  printf("\nS6 CONFIG is cached and re-sent on every accepted WELCOME\n");
  {
    Fx f; f.up();
    DynamicJsonDocument d(2048);
    deserializeJson(d, R"([{"sensorId":"saw","kind":"ct","channel":0,"tripRatio":1.5,"minCounts":20.0,"clearRatio":0.8},
                           {"sensorId":"bin:s","kind":"bin","invert":false},
                           {"sensorId":"m","kind":"plug","ip":"192.168.86.40","plug":"tasmota","thresholdW":25}])");
    f.s.configureSensors(d.as<JsonArrayConst>());
    std::string frame;
    ok("a CONFIG goes out", f.drain(frame) && frame.find("\"t\":\"CONFIG\"") != std::string::npos);
    ok("the clamp's tuning REACHES THE WIRE (it was silently dropped before 2026-10-04)",
       frame.find("\"tripRatio\":1.5") != std::string::npos && frame.find("\"minCounts\":20") != std::string::npos &&
       frame.find("\"clearRatio\":0.8") != std::string::npos);
    ok("a bin sensor keeps its invert", frame.find("\"kind\":\"bin\",\"invert\":false") != std::string::npos);
    ok("a plug carries its address, protocol and threshold",
       frame.find("\"ip\":\"192.168.86.40\"") != std::string::npos && frame.find("\"plug\":\"tasmota\"") != std::string::npos);
    ok("the frame validates on the node's side", [&] {
      DynamicJsonDocument p(2048); deserializeJson(p, frame);
      nodelink::SensorSpec specs[nodelink::kMaxSensorsPerNode]; size_t n = 0; const char* err = nullptr;
      return nodelink::parseConfigFrame(p.as<JsonObjectConst>(), specs, nodelink::kMaxSensorsPerNode, n, err) && n == 3;
    }());
    ok("sent once", !f.drain(frame));
    f.s.onDown();
    f.up();
    ok("the node forgets its CONFIG across a reboot, so every accepted WELCOME re-sends it", f.drain(frame) && frame.find("\"t\":\"CONFIG\"") != std::string::npos);
  }

  printf("\nS7 readings: SENSE in, forgotten when the link drops\n");
  {
    Fx f; f.up();
    f.feed(R"({"t":"SENSE","sensorId":"saw","on":true,"level":2.5,"amps":3.2,"floorA":0.1,"tripA":0.2})");
    bool on = false; uint32_t at = 0;
    ok("a reading is found by its id", f.s.senseOf("saw", on, at) && on && at == g_now);
    SenseView v;
    ok("and its telemetry is kept for a human", f.s.senseAt(0, v) && v.amps == 3.2f && v.level == 2.5f && v.reported && v.ageMs == 0);
    f.feed(R"({"t":"SENSE","sensorId":"m","on":true,"watts":412.5,"plug":true})");
    float w = 0; bool fault = true; uint32_t at2 = 0;
    ok("a plug reports watts and reachability", f.s.plugReading("m", w, fault, at2) && w == 412.5f && !fault);
    ok("a clamp is not mistaken for a plug", !f.s.plugReading("saw", w, fault, at2));
    f.s.onDown();
    ok("a dropped link FORGETS the readings (absent is OFF, never a stale ON)", !f.s.senseOf("saw", on, at) && f.s.senseCount() == 0);
    ok("an unknown sensor is not found", !f.s.senseOf("nobody", on, at));
  }

  printf("\nS8 a PRESS is an edge: queued, answered, never replayed\n");
  {
    Fx f; f.up();
    ok("accepted for a linked board that has a transmitter", f.s.pressRf(94, 14, 270, 24));
    std::string frame;
    ok("and goes out", f.drain(frame) && frame.find("\"t\":\"PRESS\"") != std::string::npos && frame.find("\"address\":94") != std::string::npos);
    f.feed(R"({"t":"ACK","seq":0,"ok":true})");
    f.s.pressRf(94, 14, 270, 24);
    ok("queued", true);
    f.s.onDown();
    f.up();
    ok("a PRESS queued across a drop is NEVER sent after the reconnect", !f.drain(frame));
    Fx g; g.up();
    g.s.pressRf(1, 1, 270, 24); g.drain(frame);
    g.feed(R"({"t":"ACK","seq":1,"ok":false,"err":"the transmitter could not send"})");
    ok("a refusal is its own fault string", std::string(g.s.pressFault()) == "the transmitter could not send");
    ok("and does NOT touch the move bookkeeping", !g.s.busy() && g.s.health().moveFault == nullptr);
    Fx h; h.up();
    h.s.pressRf(1, 1, 270, 24); h.drain(frame);
    h.feed(R"({"t":"ACK","seq":1,"ok":true})");
    ok("a good press clears the fault", std::string(h.s.pressFault()).empty());
  }

  printf("\nS9 OTA\n");
  {
    Fx f;
    const char* why = nullptr;
    ok("an offline board cannot be updated", !f.s.requestOta("/node-pwm.bin", 1302295, "0123456789abcdef0123456789abcdef", "x", why) && std::string(why) == "the board is offline");
    f.up();
    ok("a linked board accepts an order", f.s.requestOta("/node-pwm.bin", 1302295, "0123456789abcdef0123456789abcdef", "fw1", why));
    ok("it reads 'start' at once", std::string(f.s.info().ota) == "start");
    ok("a second order while one runs is refused", !f.s.requestOta("/node-pwm.bin", 1302295, "0123456789abcdef0123456789abcdef", "fw1", why) && std::string(why) == "an update is already running");
    std::string frame;
    ok("the order goes out", f.drain(frame) && frame.find("\"t\":\"OTA\"") != std::string::npos);
    f.feed(R"({"t":"OTASTATE","state":"progress","pct":40})");
    ok("progress is recorded", std::string(f.s.info().ota) == "progress" && f.s.info().otaPct == 40);
    f.feed(R"({"t":"OTASTATE","state":"fail","err":"no room"})");
    ok("a failure keeps its sentence", std::string(f.s.info().ota) == "fail" && std::string(f.s.info().otaErr) == "no room");
    ok("start and fail are link-log events; progress is not", f.sink.event("ota"));
    Fx g; g.up();
    g.s.requestOta("/n.bin", 1302295, "0123456789abcdef0123456789abcdef", "x", why);
    g_now += 31000; g.s.onPong();
    ok("an update that stopped reporting reads as a failure, not a progress bar that never moves",
       std::string(g.s.info().ota) == "fail" && std::string(g.s.info().otaErr).find("predates") != std::string::npos);
    Fx h; h.up(); h.s.requestOta("/n.bin", 1302295, "0123456789abcdef0123456789abcdef", "x", why);
    h.s.onDown(); h.up();
    ok("a node that has come back has finished (or abandoned) its update", std::string(h.s.info().ota).empty());
    h.s.noteOtaRefused("a tool is running");
    ok("a refusal made on the primary's side is readable", std::string(h.s.info().ota) == "fail" && std::string(h.s.info().otaErr) == "a tool is running");
  }

  printf("\nS10 link health and node-initiated attach\n");
  {
    Fx f;
    g_now = 5000;
    ok("down for as long as it has been since configure", !f.s.health().linked && f.s.health().downForMs == 4000);
    f.s.noteHollow(); f.s.noteHollow();
    ok("hollow sockets are counted", f.s.health().hollowDrops == 2);
    f.up();
    ok("and cleared by a good WELCOME", f.s.health().hollowDrops == 0 && f.s.health().downForMs == 0);
    ok("a second socket is refused while the link is healthy", !f.s.onAttach());
    g_now += nodelink::kPongTimeoutMs + 100;
    ok("but accepted once the first has gone stale", f.s.onAttach());
    ok("...and the half-open state is stale", !f.s.connected());
    f.up();
    f.s.clearLink();
    ok("a teardown the shell asked for is silent and final", !f.s.connected() && !f.sink.saw("never"));
  }

  printf("\nS11 a different node inherits nothing\n");
  {
    Fx f; f.up();
    ok("the node is known", f.s.dialsIn());
    f.s.configure("n1", "dustgate");
    ok("the SAME node keeps what it told us across a restart", f.s.dialsIn() && f.s.canPressRf());
    f.s.configure("n2", "dustgate");
    ok("a slot reused for a DIFFERENT node starts empty", !f.s.dialsIn() && !f.s.canPressRf() && std::string(f.s.info().board).empty());
  }

  printf("\n%d/%d passed%s\n", passed, passed + failed, failed ? " — FAILED" : "");
  return failed ? 1 : 0;
}

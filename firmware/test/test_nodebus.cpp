// =============================================================================
// test_nodebus.cpp — host conformance for the Stage-3b dispatch + execution layer.
//
// Covers the two pieces that stand between a routing decision and a moving
// valve, using a stub ActuatorBus in place of hardware:
//
//   NodeBus         — routes a selector to the right board by controllerId
//   TopologyRuntime — drains the sequencer's plan ONE MOVE AT A TIME
//
// The load-bearing assertions are the safety invariants, not the happy path:
//   • never two moves in flight at once (the 5V rail current mutex, RFC §7)
//   • make-before-break order survives the queue
//   • a move to an offline / unregistered node fails loudly, never silently
//   • the blower only starts against an already-open path, and never against a
//     sealed one (dead-head)
//
// Build + run via tools/ script `firmware:nodebus:test`.
// =============================================================================

#include <ArduinoJson.h>
#include "../control/TopologyRuntime.h"
#include "../control/NodeLink.h"
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

static int passed = 0, failed = 0;
static void ok(const char* name, bool cond, const std::string& detail = "") {
  printf("  %s %s%s\n", cond ? "✓" : "✗", name,
         cond || detail.empty() ? "" : ("  — " + detail).c_str());
  cond ? passed++ : failed++;
}

static std::string slurp(const std::string& p) {
  std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

// -----------------------------------------------------------------------------
// A bus that records what it was asked to do and stays "busy" until released,
// exactly as real hardware does mid-sweep.
// -----------------------------------------------------------------------------
struct StubBus : public topo::ActuatorBus {
  bool                     up      = true;
  bool                     moving  = false;
  bool                     accept  = true;
  std::vector<std::string> log;     // "selector->state", in issue order

  // ── sensing (tool-sensing RFC §5.6) ──────────────────────────────────────
  // What CONFIG this bus was last handed, flattened to "id@ch", plus how many
  // times it was configured at all — an empty list is a meaningful CONFIG, so
  // "was it called" and "what did it say" are different questions.
  std::vector<std::string> sensorCfg;
  int                      cfgCalls = 0;
  // Staged readings: sensorId -> (on, arrivedAtMs). Absent = never reported.
  std::map<std::string, std::pair<bool, uint32_t>> senses;

  void configureSensors(JsonArrayConst sensors) override {
    cfgCalls++;
    sensorCfg.clear();
    for (JsonObjectConst sen : sensors) {
      sensorCfg.push_back(std::string(sen["sensorId"] | "?") + "@" +
                          std::to_string((int)(sen["channel"] | -1)));
    }
  }
  bool senseOf(const char* sensorId, bool& on, uint32_t& atMs) const override {
    auto it = senses.find(std::string(sensorId ? sensorId : ""));
    if (it == senses.end()) return false;
    on = it->second.first; atMs = it->second.second;
    return true;
  }

  bool online() const override { return up; }
  bool busy()   const override { return moving; }
  bool setState(const char* selectorId, JsonObjectConst, const char* stateId) override {
    if (!accept) return false;
    log.push_back(std::string(selectorId) + "->" + stateId);
    moving = true;                  // a real move takes time
    return true;
  }
  void settle() { moving = false; } // the sweep finished
};

static std::string joined(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); i++) { if (i) s += "|"; s += v[i]; }
  return s;
}

// Pump the runtime the way loop() does, settling each move as it completes.
// Returns the number of passes it took to go quiet.
static int drain(topo::TopologyRuntime& rt, std::vector<StubBus*> buses, int maxPasses = 50) {
  int passes = 0;
  while (passes++ < maxPasses) {
    rt.update();
    bool anyMoving = false;
    for (StubBus* b : buses) if (b->moving) { b->settle(); anyMoving = true; }
    if (!anyMoving && !rt.transitioning()) { rt.update(); break; }
  }
  return passes;
}

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : "firmware/test/fixtures/";
  std::string twoGatesJson = slurp(dir + "twoGates.json");
  if (twoGatesJson.empty()) { printf("bad twoGates.json\n"); return 2; }

  // ── one move at a time, in sequencer order ───────────────────────────────
  {
    StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    rt.begin(&nb);
    std::string err;
    ok("adopt twoGates", rt.adopt(twoGatesJson.c_str(), twoGatesJson.size(), err), err);

    rt.setToolPower("toolX", 200);
    ok("queued but nothing issued yet", local.log.empty() && rt.transitioning());

    rt.update();
    ok("first pass issues exactly one move", local.log.size() == 1, joined(local.log));
    ok("that move is gate1->open", joined(local.log) == "gate1->open", joined(local.log));

    // The invariant: while the servo is still sweeping, no second move goes out.
    rt.update(); rt.update(); rt.update();
    ok("no second move while busy", local.log.size() == 1, joined(local.log));

    local.settle();
    rt.update();
    ok("queue drained after the move settles", !rt.transitioning());
  }

  // ── make-before-break survives the queue ─────────────────────────────────
  {
    StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    rt.begin(&nb);
    std::string err;
    rt.adopt(twoGatesJson.c_str(), twoGatesJson.size(), err);

    rt.setToolPower("toolX", 200); drain(rt, {&local});
    local.log.clear();

    // toolY on and toolX off in one decision: gate2 must OPEN before gate1 CLOSES,
    // or the system seals momentarily with the blower running.
    rt.setToolPower("toolY", 200);
    rt.setToolPower("toolX", 0);
    drain(rt, {&local});
    ok("make (gate2->open) precedes break (gate1->closed)",
       joined(local.log) == "gate2->open|gate1->closed", joined(local.log));
    ok("both moves issued", local.log.size() == 2, joined(local.log));
  }

  // ── collector policy: ON only against an open path, OFF immediately ──────
  {
    StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    rt.begin(&nb);
    std::string err;
    rt.adopt(twoGatesJson.c_str(), twoGatesJson.size(), err);

    rt.setToolPower("toolX", 200);
    ok("collector still off while the gate is mid-move", !rt.collectorOn());
    drain(rt, {&local});
    ok("collector on once the path is open", rt.collectorOn());

    // Idle coasts rather than cutting (the dedicated coast-down block below owns
    // the timing); what matters here is that the brain has decided "no tools".
    rt.setToolPower("toolX", 0);
    ok("idle → collector coasting, not cut", rt.collectorCoasting());
    ok("all-off flags dead-head risk", rt.deadHeadRisk());
  }

  // ── controllerId dispatch across two boards ──────────────────────────────
  {
    // gate1 on the primary, gate2 on secondary "node2".
    DynamicJsonDocument d(4096);
    deserializeJson(d, R"({"schemaVersion":1,
      "controllers":[{"id":"primary","role":"primary"},{"id":"node2","role":"secondary"}],
      "elements":[
        {"id":"dc","type":"collector"},
        {"id":"gate1","type":"selector","controllerId":"primary","kind":"servoGate",
         "states":[{"id":"open","isClosed":false,"offsetDeg":0},{"id":"closed","isClosed":true,"offsetDeg":90}],
         "branches":[{"id":"g1","opensState":"open","role":"tool"}],
         "servo":{"channel":0,"referenceAngle":10}},
        {"id":"gate2","type":"selector","controllerId":"node2","kind":"servoGate",
         "states":[{"id":"open","isClosed":false,"offsetDeg":0},{"id":"closed","isClosed":true,"offsetDeg":90}],
         "branches":[{"id":"g2","opensState":"open","role":"tool"}],
         "servo":{"channel":0,"referenceAngle":10}},
        {"id":"toolX","type":"tool"},{"id":"toolY","type":"tool"}],
      "ducts":[{"child":"gate1","parent":"dc"},{"child":"gate2","parent":"dc"},
        {"child":"toolX","parent":"gate1","parentBranch":"g1"},
        {"child":"toolY","parent":"gate2","parentBranch":"g2"}]})");
    std::string js; serializeJson(d, js);

    StubBus local, node2;
    topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    nb.registerRemote("node2", &node2);
    rt.begin(&nb);
    std::string err;
    rt.adopt(js.c_str(), js.size(), err);

    rt.setToolPower("toolY", 200);
    drain(rt, {&local, &node2});
    ok("gate2 dispatched to node2", joined(node2.log) == "gate2->open", joined(node2.log));
    ok("nothing sent to the local bus", local.log.empty(), joined(local.log));

    // ONE MACHINE PER SYSTEM, so starting toolX does not just open gate1 — it
    // takes the air from toolY and shuts gate2 as well, on the other board. The
    // switchover spans two controllers and is still make-before-break.
    node2.log.clear();
    rt.setToolPower("toolX", 200);
    drain(rt, {&local, &node2});
    ok("gate1 dispatched to the local bus", joined(local.log) == "gate1->open", joined(local.log));
    ok("and the loser's gate is shut on the OTHER board",
       joined(node2.log) == "gate2->closed", joined(node2.log));

    // The current mutex is global: a busy local bus stalls a remote move too.
    // Switching back to toolY is what needs the remote gate opened again — and
    // it has to be a rising EDGE to count as newest, so it stops first. Setting
    // an already-active tool to the same watts is not a restart and does not
    // reorder anything (see activationSeq).
    rt.setToolPower("toolY", 0);
    drain(rt, {&local, &node2});
    local.log.clear(); node2.log.clear();
    local.moving = true;
    rt.setToolPower("toolY", 200);        // newest again → wants gate2->open
    rt.update(); rt.update();
    ok("busy local bus blocks a remote move", node2.log.empty(), joined(node2.log));
    local.settle();
    drain(rt, {&local, &node2});
    ok("remote move proceeds once the local bus frees up",
       joined(node2.log) == "gate2->open", joined(node2.log));
    // Make-before-break held across the boards: the remote gate opened, and only
    // then did the local one close.
    ok("and the local gate closes only after it", joined(local.log) == "gate1->closed",
       joined(local.log));
  }

  // ── offline and unregistered controllers fail loudly ─────────────────────
  {
    DynamicJsonDocument d(4096);
    deserializeJson(d, R"({"schemaVersion":1,
      "controllers":[{"id":"primary","role":"primary"},{"id":"ghost","role":"secondary"}],
      "elements":[
        {"id":"dc","type":"collector"},
        {"id":"gate1","type":"selector","controllerId":"ghost","kind":"servoGate",
         "states":[{"id":"open","isClosed":false,"offsetDeg":0},{"id":"closed","isClosed":true,"offsetDeg":90}],
         "branches":[{"id":"g1","opensState":"open","role":"tool"}],
         "servo":{"channel":0,"referenceAngle":10}},
        {"id":"toolX","type":"tool"}],
      "ducts":[{"child":"gate1","parent":"dc"},{"child":"toolX","parent":"gate1","parentBranch":"g1"}]})");
    std::string js; serializeJson(d, js);

    // (a) controller never registered at all
    {
      StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
      nb.setLocal(&local, "primary");
      rt.begin(&nb);
      std::string err;
      rt.adopt(js.c_str(), js.size(), err);
      rt.setToolPower("toolX", 200);
      drain(rt, {&local});
      ok("unregistered controller → move fails, not silently dropped",
         rt.failedMoves().size() == 1 && rt.failedMoves()[0].selectorId == "gate1");
      ok("unregistered controller → nothing sent locally by mistake", local.log.empty());
      ok("collector never starts against an unopened path", !rt.collectorOn());
    }

    // (b) registered but link down
    {
      StubBus local, ghost; ghost.up = false;
      topo::NodeBus nb; topo::TopologyRuntime rt;
      nb.setLocal(&local, "primary");
      nb.registerRemote("ghost", &ghost);
      rt.begin(&nb);
      std::string err;
      rt.adopt(js.c_str(), js.size(), err);
      rt.setToolPower("toolX", 200);
      drain(rt, {&local, &ghost});
      ok("offline node → move recorded as failed",
         rt.failedMoves().size() == 1 && rt.failedMoves()[0].reason == "controller offline",
         rt.failedMoves().empty() ? "<none>" : rt.failedMoves()[0].reason);
      ok("offline node → no command issued", ghost.log.empty());
    }
  }

  // ── a selector with no controllerId is local (single-board shops) ─────────
  {
    DynamicJsonDocument d(4096);
    deserializeJson(d, R"({"schemaVersion":1,"controllers":[{"id":"primary","role":"primary"}],
      "elements":[
        {"id":"dc","type":"collector"},
        {"id":"gate1","type":"selector","kind":"servoGate",
         "states":[{"id":"open","isClosed":false,"offsetDeg":0},{"id":"closed","isClosed":true,"offsetDeg":90}],
         "branches":[{"id":"g1","opensState":"open","role":"tool"}],
         "servo":{"channel":0,"referenceAngle":10}},
        {"id":"toolX","type":"tool"}],
      "ducts":[{"child":"gate1","parent":"dc"},{"child":"toolX","parent":"gate1","parentBranch":"g1"}]})");
    std::string js; serializeJson(d, js);

    StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    rt.begin(&nb);
    std::string err;
    rt.adopt(js.c_str(), js.size(), err);
    rt.setToolPower("toolX", 200);
    drain(rt, {&local});
    ok("no controllerId → driven locally", joined(local.log) == "gate1->open", joined(local.log));
  }

  // ── collector coast-down ─────────────────────────────────────────────────
  // Idle must not cut the blower dead: a spinning-down tool still throws dust,
  // and cutting on every brief pause short-cycles the motor. Time is injected,
  // so this asserts the real boundary rather than sleeping through it.
  {
    StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    rt.begin(&nb);
    std::string err;
    ok("adopt for coast test", rt.adopt(twoGatesJson.c_str(), twoGatesJson.size(), err), err);

    const uint32_t t0 = 1000000;
    rt.setToolPower("toolX", 200);
    for (int i = 0; i < 10; i++) { rt.update(t0); local.settle(); }
    ok("blower running with a tool on", rt.collectorOn());

    rt.setToolPower("toolX", 0);
    rt.update(t0);
    ok("idle → coasting, still energized", rt.collectorOn() && rt.collectorCoasting());

    rt.update(t0 + topo::kDefaultCollectorOffDelayMs - 1);
    ok("still coasting one tick before expiry", rt.collectorOn());

    rt.update(t0 + topo::kDefaultCollectorOffDelayMs);
    ok("off once the coast expires", !rt.collectorOn() && !rt.collectorCoasting());

    // A tool restarting mid-coast cancels it — otherwise the blower would cut out
    // partway into the next cut.
    rt.setToolPower("toolX", 200);
    for (int i = 0; i < 10; i++) { rt.update(t0 + 100); local.settle(); }
    rt.setToolPower("toolX", 0);
    rt.update(t0 + 100);
    ok("coasting again after the tool stops", rt.collectorCoasting());
    rt.setToolPower("toolX", 200);
    rt.update(t0 + 200);
    ok("restart cancels the coast", rt.collectorOn() && !rt.collectorCoasting());
    for (int i = 0; i < 10; i++) { rt.update(t0 + 99999); local.settle(); }
    ok("and it stays on well past the old deadline", rt.collectorOn());
  }

  // ── malformed input is rejected, not adopted ─────────────────────────────
  {
    StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    rt.begin(&nb);
    std::string err;
    const char* junk = "{not json";
    ok("bad JSON rejected", !rt.adopt(junk, strlen(junk), err) && !rt.loaded());
    rt.setToolPower("toolX", 200);
    rt.update();
    ok("no topology → no moves", local.log.empty());
  }

  // ── the CLAIM frames (nodelink.js hello/welcome) ─────────────────────────
  //
  // A node belongs to ONE primary. These pin the frame halves the sketch relies
  // on; the conversation itself (refusal, takeover, "nothing moved") is pinned
  // end-to-end by nodelink-conformance.js against the mock node, because the
  // node firmware is a sketch and can't be host-tested.
  {
    {
      DynamicJsonDocument f(256);
      topo::nodelink::buildHello(f.to<JsonObject>(), "dustgate-shop", "node-1");
      ok("HELLO carries the claim", std::string(f["primaryId"] | "") == "dustgate-shop");
      ok("...and no takeover by default", !f.containsKey("takeover"));
    }
    {
      DynamicJsonDocument f(256);
      topo::nodelink::buildHello(f.to<JsonObject>(), "dustgate-bench", "node-1", true);
      ok("HELLO takeover is explicit when asked for", (f["takeover"] | false) == true);
    }
    {
      DynamicJsonDocument f(512);
      topo::nodelink::buildWelcome(f.to<JsonObject>(), "node-1", "qtpy_s3", "1.0.0", 4, 0,
                                   "dustgate-shop", /*accepted=*/true);
      ok("WELCOME names the owner", std::string(f["claimedBy"] | "") == "dustgate-shop");
      ok("an accepted WELCOME carries no refusal", !f.containsKey("accepted"));
      ok("welcomeAccepted reads it as yes",
         topo::nodelink::welcomeAccepted(f.as<JsonObjectConst>()));
    }
    {
      DynamicJsonDocument f(512);
      topo::nodelink::buildWelcome(f.to<JsonObject>(), "node-1", "qtpy_s3", "1.0.0", 4, 0,
                                   "dustgate-shop", /*accepted=*/false);
      ok("a refusal is explicit", (f["accepted"] | true) == false);
      ok("...and still names who has the board",
         std::string(f["claimedBy"] | "") == "dustgate-shop");
      ok("welcomeAccepted reads it as no",
         !topo::nodelink::welcomeAccepted(f.as<JsonObjectConst>()));
    }
    {
      // The safe reading is the DEFAULT one: a node built before claims answers
      // with neither field, and its silence must mean "yes", not "maybe".
      DynamicJsonDocument f(512);
      deserializeJson(f, R"({"t":"WELCOME","v":1,"nodeId":"n","board":"b","fw":"1",
                             "caps":{"servos":4,"linear":0}})");
      ok("a legacy WELCOME is accepted",
         topo::nodelink::welcomeAccepted(f.as<JsonObjectConst>()));
    }
  }

  // ── NodeLink frames: the primary resolves, the secondary obeys ───────────
  {
    DynamicJsonDocument tg(16384);
    deserializeJson(tg, twoGatesJson);
    JsonObjectConst gate1;
    for (JsonObjectConst e : tg["elements"].as<JsonArrayConst>())
      if (topo::_eq(e["id"], "gate1")) gate1 = e;

    // A SET must carry a resolved ANGLE, never a state name to interpret.
    // gate1: referenceAngle 10, open +0 / closed +90.
    {
      DynamicJsonDocument f(512);
      bool built = topo::nodelink::buildSetFrame(f.to<JsonObject>(), 7, "gate1", gate1, "open");
      ok("SET built for a calibrated gate", built);
      ok("SET resolves open → angle 10", (f["angle"] | -1) == 10, std::to_string(f["angle"] | -1));
      ok("SET carries drive=servo", std::string(f["drive"] | "") == "servo");
      ok("SET carries the channel", (f["channel"] | -1) == 0);
      ok("SET echoes seq + ids", (f["seq"] | 0) == 7 &&
         std::string(f["selectorId"] | "") == "gate1" &&
         std::string(f["stateId"] | "") == "open");
    }
    {
      DynamicJsonDocument f(512);
      topo::nodelink::buildSetFrame(f.to<JsonObject>(), 8, "gate1", gate1, "closed");
      ok("SET resolves closed → angle 100", (f["angle"] | -1) == 100, std::to_string(f["angle"] | -1));
    }

    // The safety refusal: an uncalibrated servo must NOT go on the wire. The
    // JS/C++ resolver both default a missing referenceAngle to 0, so without
    // this guard a real valve would be driven to a made-up position.
    {
      DynamicJsonDocument d(1024);
      deserializeJson(d, R"({"id":"g","kind":"servoGate",
        "states":[{"id":"open","isClosed":false,"offsetDeg":0}],
        "servo":{"channel":2}})");
      DynamicJsonDocument f(512);
      ok("SET refused for an uncalibrated servo",
         !topo::nodelink::buildSetFrame(f.to<JsonObject>(), 1, "g", d.as<JsonObjectConst>(), "open"));
    }
    // Likewise a linear state with no captured positionMm.
    {
      DynamicJsonDocument d(1024);
      deserializeJson(d, R"({"id":"s","kind":"linear","states":[{"id":"a","isClosed":false}]})");
      DynamicJsonDocument f(512);
      ok("SET refused for an uncalibrated linear state",
         !topo::nodelink::buildSetFrame(f.to<JsonObject>(), 1, "s", d.as<JsonObjectConst>(), "a"));
    }

    // Round-trip: what the primary builds is exactly what the secondary parses.
    {
      DynamicJsonDocument f(512);
      topo::nodelink::buildSetFrame(f.to<JsonObject>(), 42, "gate1", gate1, "closed");
      std::string wire; serializeJson(f, wire);

      DynamicJsonDocument in(512);
      deserializeJson(in, wire);
      topo::nodelink::SetCommand cmd;
      const char* err = nullptr;
      bool okParse = topo::nodelink::parseSetFrame(in.as<JsonObjectConst>(), cmd, err);
      ok("SET round-trips primary → wire → secondary", okParse, err ? err : "");
      ok("round-trip preserves angle/channel/seq",
         cmd.angle == 100 && cmd.channel == 0 && cmd.seq == 42 && cmd.isServo);
      ok("round-trip preserves ids",
         std::string(cmd.selectorId) == "gate1" && std::string(cmd.stateId) == "closed");
    }

    // A secondary must refuse malformed frames rather than default anything —
    // it moves only when told exactly where.
    {
      const char* bad[] = {
        R"({"t":"SET","seq":1,"selectorId":"g","stateId":"open","drive":"servo","channel":0})",       // no angle
        R"({"t":"SET","seq":1,"selectorId":"g","stateId":"open","drive":"servo","channel":0,"angle":400})", // out of range
        R"({"t":"SET","seq":1,"selectorId":"g","stateId":"open","drive":"wat","channel":0})",         // bad drive
        R"({"t":"SET","seq":1,"stateId":"open","drive":"servo","channel":0,"angle":10})",             // no selectorId
        R"({"t":"PING"})",                                                                             // not a SET
        // The linear half, which used to accept anything at all. Bounds mirror
        // validateFrame() in nodelink.js — PAIR, see CLAUDE.md.
        R"({"t":"SET","seq":1,"selectorId":"g","stateId":"open","drive":"linear","channel":0})",              // no positionMm
        R"({"t":"SET","seq":1,"selectorId":"g","stateId":"open","drive":"linear","channel":0,"positionMm":99999})",  // out of range
        R"({"t":"SET","seq":1,"selectorId":"g","stateId":"open","drive":"linear","channel":0,"positionMm":-99999})", // out of range, other way
        R"({"t":"SET","seq":1,"selectorId":"g","stateId":"open","drive":"linear","channel":0,"positionMm":"far"})",  // not a number at all
      };
      int refused = 0;
      for (const char* b : bad) {
        DynamicJsonDocument in(512);
        deserializeJson(in, b);
        topo::nodelink::SetCommand cmd;
        const char* err = nullptr;
        if (!topo::nodelink::parseSetFrame(in.as<JsonObjectConst>(), cmd, err)) refused++;
      }
      ok("secondary refuses every malformed SET", refused == 9, std::to_string(refused) + "/9");

      // ...and still ACCEPTS an ordinary linear move, so the bounds above are a
      // gate and not a wall.
      {
        DynamicJsonDocument in(512);
        deserializeJson(in, R"({"t":"SET","seq":7,"selectorId":"lin","stateId":"s2",)"
                            R"("drive":"linear","channel":0,"positionMm":95.4})");
        topo::nodelink::SetCommand cmd;
        const char* err = nullptr;
        bool okParse = topo::nodelink::parseSetFrame(in.as<JsonObjectConst>(), cmd, err);
        ok("secondary accepts an in-range linear SET", okParse, err ? err : "");
        ok("...and keeps its position", okParse && !cmd.isServo &&
           cmd.positionMm > 95.3f && cmd.positionMm < 95.5f);
      }
    }
  }

  // ── two systems, two blowers ─────────────────────────────────────────────
  // Everything the runtime used to answer once it now answers per system. The
  // failure this guards against is the obvious one: a busy 4" system dragging
  // the idle 2.5" blower on with it, or an idle one cutting a running blower.
  {
    std::string shopJson = slurp(dir + "twoSystemShop.json");
    if (shopJson.empty()) { printf("bad twoSystemShop.json\n"); return 2; }

    StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    rt.begin(&nb);
    std::string err;
    ok("adopt a two-system shop", rt.adopt(shopJson.c_str(), shopJson.size(), err), err);
    ok("both systems known", joined(rt.systemIds()) == "big|small", joined(rt.systemIds()));

    const uint32_t t0 = 2000000;

    // The jointer lives only on the 4" system.
    rt.setMachinePower("jointer", 200);
    for (int i = 0; i < 10; i++) { rt.update(t0); local.settle(); }
    ok("jointer runs the 4\" blower",       rt.collectorOn("big"));
    ok("and leaves the 2.5\" one alone",    !rt.collectorOn("small"));
    ok("its own valve was opened",          joined(local.log) == "bv-jnt->open", joined(local.log));
    local.log.clear();

    // The drill press lives only on the 2.5" system. Both blowers now run, which
    // a single collectorOn() could not have represented.
    rt.setMachinePower("drill-press", 200);
    for (int i = 0; i < 10; i++) { rt.update(t0); local.settle(); }
    ok("drill press runs the 2.5\" blower", rt.collectorOn("small"));
    ok("4\" blower still running",          rt.collectorOn("big"));
    ok("only the manifold moved",           joined(local.log) == "man->m2", joined(local.log));

    // Stopping one machine coasts ONLY its own blower.
    rt.setMachinePower("jointer", 0);
    rt.update(t0);
    ok("4\" coasting after its machine stops", rt.collectorOn("big") && rt.collectorCoasting("big"));
    ok("2.5\" not coasting — still working",   rt.collectorOn("small") && !rt.collectorCoasting("small"));

    rt.update(t0 + topo::kDefaultCollectorOffDelayMs);
    ok("4\" off once its coast expires",       !rt.collectorOn("big"));
    // The whole point: an unrelated system finishing must not stop this blower.
    ok("2.5\" unaffected by the other's coast", rt.collectorOn("small"));

    // A machine spanning both systems drives both at once, from one power event.
    rt.setMachinePower("drill-press", 0);
    rt.update(t0 + topo::kDefaultCollectorOffDelayMs * 2);
    local.log.clear();
    rt.setMachinePower("table-saw", 200);
    for (int i = 0; i < 10; i++) { rt.update(t0 + topo::kDefaultCollectorOffDelayMs * 2); local.settle(); }
    ok("one machine starts both blowers", rt.collectorOn("big") && rt.collectorOn("small"));
    // Concatenated per system in document order, never interleaved. The jointer
    // valve is still open here (idle-HOLD left it where it was), so the big
    // system contributes a make AND a break — and the small system's move lands
    // after BOTH of them, not between them. Interleaving would put man->m1
    // between bv-cab->open and bv-jnt->closed, which is the dead-head window the
    // sequencer exists to close.
    ok("moves grouped by system, big first",
       joined(local.log) == "bv-cab->open|bv-jnt->closed|man->m1", joined(local.log));

    // The status blob has to carry the per-system truth, or the UI can only ever
    // show one blower and will show the wrong one half the time.
    DynamicJsonDocument st(8192);
    rt.writeStatus(st.to<JsonObject>());
    ok("status reports both systems",
       st["systems"]["big"]["collectorOn"] == true && st["systems"]["small"]["collectorOn"] == true);
    ok("status keys tools by machine", st["tools"]["table-saw"]["active"] == true);
    ok("status rolls up the machine verdict",
       std::string(st["machines"]["table-saw"]["status"] | "") == "routed",
       std::string(st["machines"]["table-saw"]["status"] | ""));
    ok("status keys reachability by port", st["reachable"]["ts-cabinet"] == true &&
                                           st["reachable"]["ts-overarm"] == true);
  }

  // ── a CT-sensed tool drives the routing brain (RFC §5.6) ────────────────
  //
  // The half that was missing until 2026-09-15: the frames existed and nothing
  // sent or consumed them. These assert the PRIMARY's side — that the layout
  // becomes a CONFIG, and that a reported bit turns into a routed tool.
  {
    // twoGates with toolX watched by a clamp on channel 2 of this board.
    DynamicJsonDocument tg(16384);
    deserializeJson(tg, twoGatesJson);
    for (JsonObject e : tg["elements"].as<JsonArray>()) {
      if (topo::_eq(e["id"], "toolX")) {
        JsonObject ct = e.createNestedObject("sensor").createNestedObject("ct");
        ct["channel"] = 2;   // no controllerId — "this board"
      }
    }
    std::string ctJson; serializeJson(tg, ctJson);

    StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    rt.begin(&nb);
    std::string err;
    ok("adopt a layout with a CT", rt.adopt(ctJson.c_str(), ctJson.size(), err), err);

    // Adopting is what configures the boards — the layout is the only thing
    // that knows a clamp exists.
    ok("adopting pushes a CONFIG", local.cfgCalls > 0);
    ok("...naming the tool and its pad", joined(local.sensorCfg) == "toolX@2",
       joined(local.sensorCfg));

    // Nothing has reported yet. ABSENT IS OFF (RFC §5.6a) — and it must not
    // route, because a tool that has never been heard from is not running.
    rt.update(1000);
    drain(rt, { &local });
    ok("a tool that has never reported does not route", local.log.empty(),
       joined(local.log));

    // THE EVENT: the clamp says the planer is running. One bit in, and the
    // whole make-before-break machine downstream of it should engage.
    local.log.clear();
    local.senses["toolX"] = { true, 1000 };
    rt.update(1000);
    drain(rt, { &local });
    ok("a CT reporting ON routes the tool", !local.log.empty(), joined(local.log));

    // ...and off again.
    local.log.clear();
    local.senses["toolX"] = { false, 2000 };
    rt.update(2000);
    drain(rt, { &local });
    DynamicJsonDocument st(8192);
    rt.writeStatus(st.to<JsonObject>());
    ok("a CT reporting OFF stops asking for the collector",
       st["tools"]["toolX"]["active"] == false);

    // STALE IS OFF TOO, for now. A reading older than kSenseStaleMs cannot be
    // trusted, and the safe reading of "I do not know" is the one that leaves
    // the shop dusty rather than the blower running.
    local.senses["toolX"] = { true, 1000 };
    rt.update(1000 + topo::nodelink::kSenseStaleMs + 1);
    DynamicJsonDocument st2(8192);
    rt.writeStatus(st2.to<JsonObject>());
    ok("a reading older than kSenseStaleMs is not believed",
       st2["tools"]["toolX"]["active"] == false);

    // ...but a reading INSIDE the window still is, or the check above would be
    // passing for the wrong reason.
    local.senses["toolX"] = { true, 1000 };
    rt.update(1000 + topo::nodelink::kSenseStaleMs - 1);
    DynamicJsonDocument st3(8192);
    rt.writeStatus(st3.to<JsonObject>());
    ok("a reading inside the window is", st3["tools"]["toolX"]["active"] == true);

    // MANUAL BEATS THE CLAMP, the same way it beats a plug: the override exists
    // for "run it anyway", and a poll tick reporting off a second later would
    // make the button look broken.
    rt.setMachineManual("toolX", true);
    local.senses["toolX"] = { false, 9000 };
    rt.update(9000);
    DynamicJsonDocument st4(8192);
    rt.writeStatus(st4.to<JsonObject>());
    ok("a manual override outranks an off CT", st4["tools"]["toolX"]["active"] == true);
    rt.setMachineManual("toolX", false);
  }

  // ── a CLAMPED COLLECTOR feeds the plug path, not the machine path ───────
  //
  // A tool's reading answers "should the blower run"; a COLLECTOR's answers
  // "did the press we sent actually land" — every way DustGate commands a
  // blower is stateless, so this is the only thing that can say. Different
  // question, different consumer: setMachinePower() would find no such machine
  // and do nothing at all, silently.
  {
    DynamicJsonDocument tg(16384);
    deserializeJson(tg, twoGatesJson);
    for (JsonObject e : tg["elements"].as<JsonArray>())
      if (topo::_eq(e["type"], "collector"))
        e.createNestedObject("sensor").createNestedObject("ct")["channel"] = 0;
    std::string j; serializeJson(tg, j);

    StubBus local; topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    rt.begin(&nb);
    std::string err;
    ok("adopt a clamped collector", rt.adopt(j.c_str(), j.size(), err), err);
    ok("the collector's clamp is configured too", joined(local.sensorCfg) == "dc@0",
       "cfg=[" + joined(local.sensorCfg) + "] calls=" + std::to_string(local.cfgCalls));

    // Nothing reported: the blower's plug reads unreachable rather than "off",
    // because never-heard-from is not the same as measured-at-zero.
    rt.update(1000);
    DynamicJsonDocument a(8192);
    rt.writeStatus(a.to<JsonObject>());
    // "system-1" is what the shop layer names a v1 document's single system.
    ok("an unreported clamp leaves the blower plug unreachable",
       a["systems"]["system-1"]["plug"]["reachable"] == false);

    // Reporting ON must reach the PLUG state, which is what judges whether a
    // stateless press landed.
    local.senses["dc"] = { true, 1000 };
    rt.update(1000);
    DynamicJsonDocument b(8192);
    rt.writeStatus(b.to<JsonObject>());
    ok("a clamped collector reporting ON is seen as drawing",
       (b["systems"]["system-1"]["plug"]["watts"] | 0.0f) > topo::kCollectorRunningW,
       std::to_string(b["systems"]["system-1"]["plug"]["watts"] | 0.0f));

    local.senses["dc"] = { false, 2000 };
    rt.update(2000);
    DynamicJsonDocument c(8192);
    rt.writeStatus(c.to<JsonObject>());
    ok("...and OFF is seen as not drawing",
       (c["systems"]["system-1"]["plug"]["watts"] | -1.0f) == 0.0f,
       std::to_string(c["systems"]["system-1"]["plug"]["watts"] | -1.0f));
  }

  // ── a CT on ANOTHER board, and removing one ─────────────────────────────
  {
    DynamicJsonDocument tg(16384);
    deserializeJson(tg, twoGatesJson);
    tg["controllers"].as<JsonArray>().createNestedObject()["id"] = "planer-node";
    for (JsonObject e : tg["elements"].as<JsonArray>()) {
      if (topo::_eq(e["id"], "toolY")) {
        JsonObject ct = e.createNestedObject("sensor").createNestedObject("ct");
        ct["controllerId"] = "planer-node";
        ct["channel"]      = 0;
      }
    }
    std::string j; serializeJson(tg, j);

    StubBus local, node; topo::NodeBus nb; topo::TopologyRuntime rt;
    nb.setLocal(&local, "primary");
    nb.registerRemote("planer-node", &node);
    rt.begin(&nb);
    std::string err;
    rt.adopt(j.c_str(), j.size(), err);

    ok("a CT on a node is configured on THAT board",
       joined(node.sensorCfg) == "toolY@0", joined(node.sensorCfg));
    // The local board carries no clamp in this layout, and must be told so
    // explicitly — an empty list is how a sensor gets removed, so a board that
    // simply stopped being mentioned would go on reporting a dead tool.
    ok("...and the local board is told it has none",
       local.cfgCalls > 0 && local.sensorCfg.empty(),
       std::to_string(local.cfgCalls) + " / " + joined(local.sensorCfg));

    // Re-adopting WITHOUT the clamp must clear it, not leave it behind.
    DynamicJsonDocument plain(16384);
    deserializeJson(plain, twoGatesJson);
    plain["controllers"].as<JsonArray>().createNestedObject()["id"] = "planer-node";
    std::string j2; serializeJson(plain, j2);
    rt.adopt(j2.c_str(), j2.size(), err);
    ok("re-adopting without the CT clears the board",
       node.sensorCfg.empty(), joined(node.sensorCfg));
  }

  // ── CONFIG / SENSE — the PAIR of nodelink.test.js's cases ────────────────
  //
  // Same rules, same order, same literal numbers as the JS side. The firmware
  // cannot import nodelink.js, so this file IS the agreement.
  {
    using namespace topo::nodelink;

    ok("kSenseRepeatMs matches SENSE_REPEAT_MS", kSenseRepeatMs == 5000);
    ok("kSenseStaleMs matches SENSE_STALE_MS", kSenseStaleMs == 15000);
    ok("stale is 3x the repeat", kSenseStaleMs / kSenseRepeatMs == 3);
    ok("kMaxSensorsPerNode matches MAX_SENSORS_PER_NODE", kMaxSensorsPerNode == 4);
    // Adding CONFIG/SENSE did NOT bump the version: both ends ignore a frame
    // type they don't know, so every old/new combination degrades safely and a
    // bump would force a flash of every board in the shop to buy nothing.
    ok("protocol version unchanged by CONFIG/SENSE", kVersion == 1);

    // caps.ct — the PAIR of nodelink.test.js's "a clamp is DECLARED" block.
    {
      // 384 for the same reason dustgate_node.cpp uses it: at 256 this document
      // overflows and ArduinoJson drops `caps.ct` without a word.
      StaticJsonDocument<384> w;
      buildWelcome(w.to<JsonObject>(), "node-1", "xiao_c5", "1.0.0", 4, 0, nullptr, true, 1);
      ok("a WELCOME may declare a clamp", (w["caps"]["ct"] | 0) == 1);

      StaticJsonDocument<384> none;
      buildWelcome(none.to<JsonObject>(), "node-1", "xiao_c5", "1.0.0", 4, 0);
      // OMITTED, not zeroed: absent already means none, so writing it would add
      // a field to every board's answer to repeat what silence said.
      ok("a board with no clamp omits the field", !none["caps"].containsKey("ct"));
      ok("...and reads back as none", (none["caps"]["ct"] | 0) == 0);
    }

    SensorSpec specs[kMaxSensorsPerNode];
    size_t n = 99;
    const char* err = nullptr;

    // A well-formed CONFIG.
    {
      StaticJsonDocument<512> d;
      deserializeJson(d, R"({"t":"CONFIG","seq":7,"sensors":[{"sensorId":"planer-ct","kind":"ct","channel":0}]})");
      ok("CONFIG parses", parseConfigFrame(d.as<JsonObjectConst>(), specs, kMaxSensorsPerNode, n, err),
         err ? err : "");
      ok("one sensor", n == 1);
      ok("sensorId is carried verbatim", std::string(specs[0].sensorId) == "planer-ct");
      ok("channel is carried", specs[0].channel == 0);
    }

    // An EMPTY list is valid and means "report nothing" — the same state as a
    // board that has never been configured, so there is no third case.
    {
      StaticJsonDocument<256> d;
      deserializeJson(d, R"({"t":"CONFIG","seq":8,"sensors":[]})");
      ok("an empty sensor list parses", parseConfigFrame(d.as<JsonObjectConst>(), specs, kMaxSensorsPerNode, n, err));
      ok("and configures nothing", n == 0);
    }

    // ALL OR NOTHING: a bad entry rejects the whole frame. A half-applied
    // config would leave the primary believing in a sensor the board dropped.
    {
      StaticJsonDocument<512> d;
      deserializeJson(d, R"({"t":"CONFIG","seq":9,"sensors":[{"sensorId":"a","kind":"ct","channel":0},{"sensorId":"b","kind":"bin","channel":1}]})");
      n = 99;
      ok("an unknown kind rejects the WHOLE frame",
         !parseConfigFrame(d.as<JsonObjectConst>(), specs, kMaxSensorsPerNode, n, err));
      ok("and applies none of it", n == 99);
    }

    {
      StaticJsonDocument<512> d;
      deserializeJson(d, R"({"t":"CONFIG","seq":10,"sensors":[{"sensorId":"a","kind":"ct","channel":0},{"sensorId":"a","kind":"ct","channel":1}]})");
      ok("a duplicate sensorId is refused",
         !parseConfigFrame(d.as<JsonObjectConst>(), specs, kMaxSensorsPerNode, n, err));
    }

    // TYPE FIRST: as<int>() on a string yields 0, which is a real pad on every
    // board — the same trap positionMm fell into.
    {
      StaticJsonDocument<512> d;
      deserializeJson(d, R"({"t":"CONFIG","seq":11,"sensors":[{"sensorId":"a","kind":"ct","channel":"zero"}]})");
      ok("a non-numeric channel is refused, not read as pad 0",
         !parseConfigFrame(d.as<JsonObjectConst>(), specs, kMaxSensorsPerNode, n, err));
    }

    {
      StaticJsonDocument<768> d;
      deserializeJson(d, R"({"t":"CONFIG","seq":12,"sensors":[
        {"sensorId":"a","kind":"ct","channel":0},{"sensorId":"b","kind":"ct","channel":1},
        {"sensorId":"c","kind":"ct","channel":2},{"sensorId":"d","kind":"ct","channel":3},
        {"sensorId":"e","kind":"ct","channel":4}]})");
      ok("more sensors than the board holds is refused, not truncated",
         !parseConfigFrame(d.as<JsonObjectConst>(), specs, kMaxSensorsPerNode, n, err));
    }

    { StaticJsonDocument<256> d;
      deserializeJson(d, R"({"t":"SET","seq":1})");
      ok("a SET is not a CONFIG", !parseConfigFrame(d.as<JsonObjectConst>(), specs, kMaxSensorsPerNode, n, err)); }

    // SENSE: one bit, and `level` is a multiple of the trip point — diagnostic
    // only, omitted rather than zeroed when the board has no trip point.
    {
      StaticJsonDocument<256> d;
      buildSense(d.to<JsonObject>(), "planer-ct", true, 2.8f);
      ok("SENSE carries the bit", d["on"] == true);
      ok("and echoes the primary's id", std::string(d["sensorId"] | "") == "planer-ct");
      ok("and carries level when there is one", d.containsKey("level"));

      StaticJsonDocument<256> e;
      buildSense(e.to<JsonObject>(), "planer-ct", false);
      ok("level is OMITTED, not zeroed, when absent", !e.containsKey("level"));
      ok("an off frame is still a report, not silence", e["on"] == false);
    }
  }

  printf("\n%d/%d passed%s\n", passed, passed + failed,
         failed ? (", " + std::to_string(failed) + " FAILED").c_str() : "");
  return failed ? 1 : 0;
}

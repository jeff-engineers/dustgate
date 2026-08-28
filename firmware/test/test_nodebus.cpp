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
      };
      int refused = 0;
      for (const char* b : bad) {
        DynamicJsonDocument in(512);
        deserializeJson(in, b);
        topo::nodelink::SetCommand cmd;
        const char* err = nullptr;
        if (!topo::nodelink::parseSetFrame(in.as<JsonObjectConst>(), cmd, err)) refused++;
      }
      ok("secondary refuses every malformed SET", refused == 5, std::to_string(refused) + "/5");
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

  printf("\n%d/%d passed%s\n", passed, passed + failed,
         failed ? (", " + std::to_string(failed) + " FAILED").c_str() : "");
  return failed ? 1 : 0;
}

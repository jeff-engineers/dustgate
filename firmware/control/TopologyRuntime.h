// =============================================================================
// TopologyRuntime.h — the device layer that turns decisions into motion.
//
// TopologyController.h calls this "a thin device layer (the main sketch)". This
// is it, factored out of the sketch so it stays host-testable. It owns:
//
//   • the parsed document (adopted at boot and on PUT /api/topology) — either a
//     schemaVersion-1 topology or a v2 shop; Shop.h flattens the difference
//   • a topo::Controller (the brain: machine power in, routed states + per-system
//     plans out)
//   • a MOVE QUEUE, drained one move at a time through NodeBus
//
// Why a queue and not a loop: TopologySequencer already orders the moves
// make-before-break, and issuing them one at a time — never starting the next
// until NodeBus::busy() clears — is what keeps the one-servo-at-a-time current
// budget honored (RFC §7). Executing the plan is therefore a small state machine
// pumped from loop(), not a blocking sequence.
//
// THE QUEUE STAYS SHOP-WIDE AND SERIAL even though the air in two systems never
// mixes. The current budget is a property of the power supply, not of a duct
// run, so two systems transitioning at once would break it just as thoroughly as
// two gates in one (RFC §10.2). Plans are therefore CONCATENATED in system
// order, never interleaved: interleaving would let system B's break land between
// system A's make and A's break, which is precisely the dead-head the sequencer
// exists to prevent. Each queued move carries its systemId so the collector
// policy below can be answered per blower.
//
// The runtime plans from HARDWARE truth, not from the brain's optimistic view.
// Controller::reconcile() adopts its routed states the moment it decides them,
// but the valves haven't moved yet — so re-planning off the controller's states
// would silently drop moves whenever two power events land between two loop
// passes (tool A off + tool B on in one poll tick: the routine case). The
// runtime therefore keeps its own `_hwStates` — what has actually been commanded
// — and re-runs planShopTransition() from there each time the destination
// changes. The brain owns the destination; the runtime owns the journey.
//
// Collector policy (the other half of "never dead-head"). EVERY CLAUSE BELOW IS
// PER SYSTEM: a shop with a 4" cyclone and a 2.5" wall unit has two blowers, two
// coast timers and two dead-head verdicts, and answering for one of them with
// the other's state is the failure this layer exists to prevent.
//   OFF at idle COASTS — the blower keeps running for control.offDelayMs after
//   the last machine on ITS system stops drawing. A bandsaw spinning down still
//   throws dust, and without this the blower short-cycles between cuts on the
//   table saw. Safe by construction: an idle system's queued moves are dropped
//   and its gates HELD, so no path can close underneath a coasting blower.
//   OFF from dead-head risk is IMMEDIATE and cancels any coast — that off is a
//   safety stop, not an idle.
//   ON is deferred until that system's OPENING moves have drained, so a blower
//   only ever starts against an already-open path. (Until 2026-09-28 it waited
//   for every queued move; a switch-on now re-closes every gate — see
//   markReassert — and the closes cannot seal a path that is already open.) A tool starting mid-coast just keeps it
//   on. A failed MAKE in that system also holds it off: if the gate that was
//   supposed to open didn't (dead node, uncalibrated servo), running the blower
//   would pull against a closed system. A failed break only leaks suction, so it
//   doesn't block — one dead secondary shouldn't make the whole shop unusable.
//   deadHeadRisk forces OFF regardless.
//   A MANUAL RUN (setCollectorManual) is a blower running because a person said
//   so, with no machine asking for it. It holds — there is no automation to hand
//   back to — and it never coasts, because a coast catches the dust still in the
//   pipe after a cut and nothing was being cut. It obeys every rule above: the
//   path is opened first (queued moves, drained before ON, exactly like a tool
//   transition), so "never dead-head" is not weakened by it. Mirrors
//   setCollectorManual() in shared/device-model/topology-device.js — the paired
//   cases live in manual-blower.test.js and test_manual_blower.cpp.
//
// PURE — ArduinoJson + STL only, NO Arduino.h. `nowMs` is injected so the host
// conformance test can drive time deterministically.
// =============================================================================

#pragma once
#include "../utils/JsonAlloc.h"
#include "CollectorPlugState.h"
#include <ArduinoJson.h>
#include "TopologyController.h"
#include "NodeBus.h"
#include "SensorPlan.h"   // what the layout wants watched, resolved once
#include "NodeLink.h"   // nodelink::kSenseStaleMs — how long a CT reading stays good
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

namespace topo {

// Synthetic wattage for a manually-switched-on machine.
//
// It has to clear that machine's OWN threshold and nothing more, so it is derived
// per machine rather than being one enormous constant. It used to be 100000.0f —
// "above any threshold anyone could set" — which was harmless only while nothing
// displayed the number. /api/status publishes it as `watts`, the build canvas now
// draws that on the tool, and a hand-switched tool read as 100.0 kW: 833 A at
// 120 V, on a plug rated for 15.
//
// Three times the trip point makes the same activation decision and reads like a
// machine. The floor keeps it strictly positive when a threshold is set to 0, so
// "manually on" can never be indistinguishable from "drawing nothing". Mirrors
// setToolManual() in dustgate-ui/.../demo-api.service.ts — see the twin-pair
// table in CLAUDE.md.
static inline float manualWattsFor(float threshold) {
    return threshold > 0.0f ? threshold * 3.0f : 15.0f;
}

// Coast-down used when the collector element doesn't name one. Not zero on
// purpose: every shop wants some, and the stop-selection path's equivalent slack
// is 3 s. Jeff's call, 2026-08-19, when the setting got a UI at last.
// CHANGE THIS AND CHANGE DEFAULT_COLLECTOR_OFF_DELAY_MS in
// shared/device-model/topology-device.js — the conformance suite compares them.
static const uint32_t kDefaultCollectorOffDelayMs = 5000;

// A move that has been issued but whose bus rejected it (offline node, missing
// calibration). Surfaced through /api/status so the UI can say which gate.
// No default member initializers: the ESP32 toolchain builds at gnu++11, where
// that would make this a non-aggregate and break the brace-init call sites.
// Something that was asked for and is not happening. Shape and codes are in
// shared/device-model/topology-device.js ("Problems"); the two collector codes
// are derived below, the rest are RAISED by firmware.ino, which sees the links,
// the transmitter and the plugs.
struct Problem {
    std::string code;
    std::string severity;   // "bad" | "warn"
    std::string subjectType;
    std::string subjectId;
    std::string text;
    uint32_t    sinceMs;
};
static const char* const kProblemNoStart = "Commanded on but drawing nothing \xE2\x80\x94 check the breaker, the cord and the remote.";
static const char* const kProblemBlind   = "Commanded on, but its plug isn't answering \xE2\x80\x94 can't tell whether it is running.";

struct FailedMove {
    std::string systemId;
    std::string selectorId;
    std::string toState;
    std::string reason;
    bool        isBreak;   // a failed make blocks its blower; a failed break doesn't
};

// A queued move, tagged with the system whose blower it belongs to.
struct QueuedMove {
    std::string systemId;
    Move        move;
};

// Everything the device knows about one blower.
struct CollectorState {
    bool     running;
    bool     coasting;      // still energized, but only to finish spinning the ducts clear
    uint32_t coastUntilMs;
    bool     desired;       // the brain wants this system routing air
    bool     deadHeadRisk;
    bool     manualRun;     // a person switched this blower on; no machine is asking

    // ── What the plug says, fed in from the outlet layer ─────────────────────
    //
    // `running` above is what we COMMANDED. These are what came back, and the
    // gap between them is the whole point: every way we command a blower is
    // STATELESS — a servo pressing a fob, an RF frame — so what we sent proves
    // nothing and only the draw does.
    //
    // The verdict is now computed HERE TOO, in CollectorPlugState.h, a matched
    // pair with collectorPlugState() in topology-device.js. It used to live only
    // in JS, on the reasoning that the firmware reports and the browser judges;
    // that stopped being good enough when the sender became stateless, because a
    // browser nobody has open cannot be the only thing that notices a blower
    // failed to start.
    bool     plugKnown;     // false = nothing has reported; omit `plug` entirely
    bool     plugReachable;
    float    plugWatts;
    uint32_t plugOnForMs;   // how long it has been commanded on, AS THE FEED SAID
    // When `running` last went true, by the runtime's own clock (0 = not running,
    // or no clock). The feed's age cannot be trusted for this: a clamp's is how
    // long the clamp has read ON — 0 for a blower that never started — and a
    // pressed remote has no switch to time. Either way the state read "starting"
    // for ever, so a collector that never started was never judged not-starting
    // and never pressed a second time (found 2026-10-03). See commandedForMs().
    uint32_t runningSinceMs;

    // ── Dust bin, fed in from whatever is watching the sensor ────────────────
    //
    // Same contract as the plug above: the runtime REPORTS and never judges. The
    // debounce lives in utils/BinSensor.h and the inversion lives in the wiring,
    // so by the time it reaches here it is simply "full, or not".
    //
    // `binKnown` false means NOBODY IS WATCHING — omit `bin` entirely. An
    // unwatched bin and an empty bin are different claims, and defaulting to
    // "not full" would quietly promise a warning that can never come.
    bool     binKnown;
    bool     binFull;
};

class TopologyRuntime {
public:
    void begin(NodeBus* bus) { _bus = bus; }

    // Parse and adopt a document. Replaces any current one and resets the brain
    // (machine power history, actuator states seeded to closed). Returns false
    // with `err` set if the JSON won't parse.
    bool adopt(const char* json, size_t len, std::string& err) {
        // ArduinoJson v6 needs a heap doc sized for the parse tree, not the text.
        // 2x + slack covers the key/value overhead of these documents; a bad
        // guess surfaces as NoMemory rather than silent truncation.
        size_t cap = len * 2 + 2048;
        std::unique_ptr<BigJsonDocument> doc(new BigJsonDocument(cap));
        DeserializationError e = deserializeJson(*doc, json, len);
        if (e) { err = e.c_str(); return false; }
        if (!doc->is<JsonObject>()) { err = "topology must be an object"; return false; }

        // ── schemaVersion 1 IS REFUSED, and says so (2026-09-17, jeff) ───────
        //
        // Nothing has been able to PRODUCE a v1 document for a long time: the
        // configurator writes v2 shops with systems[] and machines[]. What kept
        // v1 alive was the test fixtures, and they did active harm — in v1 a tool
        // element IS its own machine, so `element.sensor.ct` reads correctly
        // there and is null for every real document. That is precisely how the
        // suite stayed green on 2026-09-15 while no clamp paired in the app ever
        // reached its node.
        //
        // REFUSED WITH A SENTENCE, not ignored, because the alternative is the
        // failure that cost an evening on 2026-09-17: a board with a rejected
        // layout is indistinguishable from a board with no layout, and both are
        // one blue LED. The message rides g_topoRejectReason to the BAD LAYOUT
        // light, the screen, and /api/status.
        //
        // Detected by SHAPE as well as by the version field, since an export
        // that lost its schemaVersion is still a v1 document and still cannot be
        // read correctly. systems[] is what a v2 shop always has.
        {
            JsonObjectConst o = doc->as<JsonObjectConst>();
            const int ver = o["schemaVersion"] | 0;
            const bool hasSystems = o["systems"].is<JsonArrayConst>();
            if (ver == 1 || (!hasSystems && o["elements"].is<JsonArrayConst>())) {
                err = "layout is from an older version (v1) — re-save it";
                return false;
            }
        }

        _doc = std::move(doc);
        _ctrl.setTopology(_doc->as<JsonObjectConst>());
        _queue.clear();
        _failed.clear();
        _stuck.clear();
        _raised.clear();
        // Physical position is unknown after a config change; seed the same way
        // the brain does (every selector at its closed state) so the two agree.
        _hwStates = _ctrl.actuatorStates();
        _inFlightSystem.clear();
        _inFlightIsMake = false;
        _reassert.clear();
        _collectors.clear();
        for (const SystemView& sys : systemsOf(topology()))
            // Value-initialised rather than listed positionally. The struct
            // has twelve members and this used to name ten, leaving binKnown
            // and binFull to fall off the end — correct by luck, and exactly
            // what breaks silently when someone inserts a field in the middle.
            // The comment on the struct explains why it has no default member
            // initializers (gnu++11 aggregate rules), which is the same reason
            // this cannot simply be `CollectorState c;`.
            _collectors[std::string(sys.id ? sys.id : "")] = CollectorState{};
        _loaded = true;
        pushSensorConfig();
        return true;
    }

    void clear() {
        _doc.reset();
        _queue.clear();
        _failed.clear();
        _stuck.clear();
        _raised.clear();
        _manual.clear();
        _hwStates.clear();
        _collectors.clear();
        _inFlightSystem.clear();
        _inFlightIsMake = false;
        _reassert.clear();
        _loaded = false;
    }

    bool loaded() const { return _loaded; }

    // Re-push every board its sensor list.
    //
    // adopt() already does this, but it runs BEFORE the caller has had a chance
    // to map controllerIds onto paired hosts — so on a primary the first push
    // cannot resolve a remote board, and a CT on a node would never be
    // configured. Call this again once the aliases are in place. Idempotent and
    // cheap: a few small frames, and a node re-sent an identical CONFIG simply
    // ACKs it.
    void reconfigureSensors() { pushSensorConfig(); }

    // What every CT node should squeeze by. See sensing/CtTrip.h.
    //
    // PLAIN FLOATS, NOT A sensing::TripParams, and set from outside rather than
    // included: this header compiles against g++ and ArduinoJson for the host
    // tests, and CtTrip needs <Arduino.h>. The sketch hands its own compiled-in
    // values down at boot, which keeps ONE set of numbers in the system — the
    // primary's — and keeps this file testable.
    //
    // ZERO MEANS "SAY NOTHING" per field, so a runtime that was never told (every
    // host test, and the mock) sends the same frames it always did and a node
    // keeps its own values. Same sentinel the wire uses.
    void setSensorTuning(float tripRatio, float minCounts, float clearRatio) {
        _tripRatio = tripRatio; _minCounts = minCounts; _clearRatio = clearRatio;
        if (_loaded) pushSensorConfig();   // a retune is not worth a reboot
    }
    JsonObjectConst topology() const {
        return _doc ? _doc->as<JsonObjectConst>() : JsonObjectConst();
    }

    // Feed a live power reading for one machine. Reconciles and (re)builds the
    // move queue from the resulting plans. Safe to call every poll tick — an
    // unchanged decision produces empty plans and leaves the queue alone.
    //
    // A machine under MANUAL override ignores its plug: the override is the whole
    // point ("just run the collector so I can clear a clog"), and letting a poll
    // tick reporting 0 W switch it back off a second later would make the button
    // look broken.
    void setMachinePower(const std::string& machineId, float watts) {
        if (!_loaded) return;
        if (_manual.count(machineId)) return;
        ingest(_ctrl.setMachinePower(machineId, watts));
    }

    // Switch a machine on/off by hand, from the Live view. Every machine is
    // overridable, not just the ones without a plug — a sensed tool sometimes
    // needs running with the blower on for a reason the plug can't know about.
    //
    // Implemented as a synthetic power reading rather than a separate concept, so
    // the routing brain has exactly one notion of "active" and manual machines
    // take part in most-recent-wins alongside sensed ones.
    // Returns false when no such machine exists in the layout. Worth reporting:
    // an unknown id otherwise sets a wattage nothing reads and routes nothing, so
    // a typo'd or stale id looks exactly like a working switch that does nothing.
    bool setMachineManual(const std::string& machineId, bool on) {
        if (!_loaded) return false;
        if (!hasMachine(machineId)) return false;
        if (on) _manual.insert(machineId);
        else    _manual.erase(machineId);
        // Comfortably over any plausible thresholdW; off returns it to 0 W, which
        // is also what a plug reports for a machine at rest.
        ingest(_ctrl.setMachinePower(machineId, on ? manualWattsFor(_ctrl.machineThreshold(machineId)) : 0.0f));
        return true;
    }

    // Run ONE system's blower by hand, or stop it. The Live view's collector card.
    //
    // It HOLDS: a blower with no machine asking for it is running because a person
    // said so, and there is no automation to hand back to. A machine starting on
    // the same system routes over the top as usual and the run outlives it — a
    // blower that switched itself off partway through clearing a clog would be
    // worse than one left running. Only switching it off ends it.
    //
    // Opening the path is the first thing it does, not an afterthought: a system
    // whose gates are all closed (a layout adopted a moment ago, or one stopped by
    // dead-head risk) is exactly what a blower must not start into. Those moves go
    // through the ordinary queue, so ON still waits for them to drain.
    //
    // Returns false for a system that isn't in the layout — a typo'd id otherwise
    // looks exactly like a switch that does nothing.
    /**
     * Hand this system's blower plug reading in, the same way setMachinePower()
     * hands a tool's in. The sketch owns the slot↔system pairing and the clock;
     * this stays pure.
     *
     * `onForMs` is an AGE, not a timestamp — millis() never crosses this seam,
     * which is what keeps the runtime host-testable.
     */
    void setCollectorPlug(const std::string& systemId, float watts, bool reachable,
                          uint32_t onForMs) {
        auto it = _collectors.find(systemId);
        if (it == _collectors.end()) return;
        it->second.plugKnown     = true;
        it->second.plugReachable = reachable;
        it->second.plugWatts     = watts;
        it->second.plugOnForMs   = onForMs;
    }

    bool setCollectorManual(const std::string& systemId, bool on) {
        if (!_loaded) return false;
        auto it = _collectors.find(systemId);
        if (it == _collectors.end()) return false;
        CollectorState& c = it->second;
        c.manualRun = on;
        if (!on && !machineActiveOn(systemId)) {
            // Off is off. No coast on the way out — see the header note.
            c.running = false;
            c.coasting = false;
        }
        ingest(_ctrl.reconcile());
        return true;
    }

    // What this system's blower is ACTUALLY doing, as opposed to what we asked.
    // The judgement itself is pure and lives in CollectorPlugState.h; this only
    // supplies the state to judge.
    // Is a reading in hand that says this blower is drawing running current?
    // Independent of what we commanded — that is the point: collectorPlugState()
    // answers "off" whenever we are not asking, whatever the wire says.
    bool collectorDrawing(const std::string& systemId) const {
        auto it = _collectors.find(systemId);
        return it != _collectors.end() && it->second.plugKnown && it->second.plugReachable &&
               it->second.plugWatts >= topo::kCollectorRunningW;
    }

    // What the press policy should believe the blower is doing. Differs from
    // collectorPlugStateFor() in one way: collectorPlugState() answers "off"
    // whenever we are not asking, whatever the wire says — right for the app
    // (nothing is wrong) and wrong for a press, which is a toggle and so must know
    // whether the blower is really still drawing before it decides an OFF landed.
    topo::PlugState pressObservation(const std::string& systemId) const {
        // NO FEEDBACK SOURCE AT ALL means NoPlug (open loop), not Unknown. collectorPlugState() says
        // Unknown for a blower we have commanded on that nothing has reported on — right for the app
        // ("the plug is not answering") and wrong for a press: the policy WAITS on Unknown, so a
        // collector with no plug, sensor or clamp was never pressed on at all (found by the native
        // brain's end-to-end test). A layout that names a source and has not heard from it yet is
        // still Unknown: that one genuinely is blind.
        if (!collectorHasOutlet(systemId) && collectorSensorOutlet(systemId).isNull() && !collectorHasClamp(systemId))
            return topo::PlugState::NoPlug;
        topo::PlugState st = collectorPlugStateFor(systemId);
        if (st == topo::PlugState::Off && collectorDrawing(systemId)) return topo::PlugState::Running;
        return st;
    }

    // How long this blower has been COMMANDED on. The runtime's own timer wins
    // whenever it has one; the feed's number is the fallback for a caller that
    // passes no clock (the host tests).
    uint32_t commandedForMs(const CollectorState& c) const {
        if (c.running && c.runningSinceMs && _nowMs) return (uint32_t)(_nowMs - c.runningSinceMs);
        return c.plugOnForMs;
    }

    topo::PlugState collectorPlugStateFor(const std::string& systemId) const {
        auto it = _collectors.find(systemId);
        if (it == _collectors.end())
            return topo::collectorPlugState(false, false, 0.0f, 0, false, false);
        const CollectorState& c = it->second;
        // plugOnForMs is 0 both for "just commanded" and for "never commanded",
        // and `running` is what separates them: a blower we are not asking for
        // has no age to report, and passing haveOnFor=true there would make an
        // idle system look like one that just started.
        return topo::collectorPlugState(c.plugKnown, c.plugReachable, c.plugWatts,
                                        commandedForMs(c), /*haveOnFor=*/c.running,
                                        /*commandedOn=*/c.running);
    }

    bool collectorIsManual(const std::string& systemId) const {
        auto it = _collectors.find(systemId);
        return it != _collectors.end() && it->second.manualRun;
    }

    bool hasMachine(const std::string& machineId) const {
        if (!_loaded) return false;
        for (const std::string& id : machineIds(topology()))
            if (id == machineId) return true;
        return false;
    }

    bool machineIsManual(const std::string& machineId) const { return _manual.count(machineId) > 0; }

    // Map a Shelly plug identity to the machine it powers ("" if none). Thin
    // pass-through so callers don't need the controller.
    std::string machineForOutlet(const char* host, const char* ip) const {
        return _loaded ? _ctrl.machineForOutlet(host, ip) : std::string();
    }

    // ---- v1 spellings, kept for call sites that predate ports ----
    // Only the three that still have callers. hasTool/toolIsManual/activeTools
    // were kept for the same reason and turned out to have none — a comment
    // promising compatibility with nothing.
    void setToolPower(const std::string& id, float w) { setMachinePower(id, w); }
    bool setToolManual(const std::string& id, bool on) { return setMachineManual(id, on); }
    std::string toolForOutlet(const char* h, const char* i) const { return machineForOutlet(h, i); }

    // Pump the move queue. Issues at most one move per call and never while the
    // bus is busy — that IS the current mutex.
    // `nowMs` is millis() on the device and a fake clock in the host tests — the
    // runtime stays free of Arduino.h. Only the coast-down reads it, so a caller
    // that passes a constant simply never coasts (which is what the pre-coast
    // test call sites do, deliberately).
    void update(uint32_t nowMs = 0) {
        if (!_loaded || !_bus) return;
        _nowMs = nowMs;
        _bus->update();
        pollSensors();

        // Coast expiry is checked here rather than in ingest(): at idle no power
        // events arrive, so ingest() isn't called again and nothing would ever
        // switch the blower off.
        for (auto& kv : _collectors) {
            CollectorState& c = kv.second;
            if (!c.running) c.runningSinceMs = 0;
            else if (!c.runningSinceMs && nowMs) c.runningSinceMs = nowMs;
            if (c.coasting && (int32_t)(nowMs - c.coastUntilMs) >= 0) {
                c.coasting = false;
                c.running  = false;
            }
        }

        // At most one move per pass, and never while the bus is busy — that IS
        // the current mutex.
        if (!_bus->busy()) {
            _inFlightSystem.clear();
            _inFlightIsMake = false;
            if (!_queue.empty()) {
                QueuedMove q = _queue.front();
                _queue.pop_front();
                const Move& m = q.move;
                // Issued (or failed) — either way it is no longer owed. A failed
                // re-assert is recorded like any failed move, not retried forever.
                _reassert.erase(m.selectorId);
                JsonObjectConst sel = selectorById(m.selectorId);
                // `_failed` is wiped by every re-decision (ingest), which is every
                // poll tick — so on its own a failure was on the status for a few
                // hundred ms. `_stuck` keeps it until that selector next moves.
                auto fail = [&](const char* why) {
                    FailedMove f{q.systemId, m.selectorId, m.toState, why, m.isBreak};
                    _failed.push_back(f);
                    _stuck[m.selectorId] = f;
                };
                if (sel.isNull()) {
                    fail("unknown selector");
                } else if (!_bus->onlineFor(sel)) {
                    fail("controller offline");
                } else if (!_bus->setState(m.selectorId.c_str(), sel, m.toState.c_str())) {
                    fail("actuator rejected move");
                } else {
                    _stuck.erase(m.selectorId);
                    _hwStates[m.selectorId] = m.toState;   // commanded → hardware truth
                    _inFlightSystem = q.systemId;
                    _inFlightIsMake = !m.isBreak;
                }
            }
        }

        // START A BLOWER ONCE ITS SYSTEM'S OPENS HAVE LANDED — not once the whole
        // queue is empty (changed 2026-09-28). The make is what guarantees air has
        // somewhere to go; a break still queued only closes some OTHER gate, and
        // cannot seal a path that is already open (make-before-break). Waiting for
        // every break was harmless while a switch-on moved one or two gates, but a
        // switch-on now re-asserts every servo gate in the system, and holding the
        // blower off while five gates re-close one at a time would leave the tool
        // cutting without extraction for no reason. Checked per system, as before.
        for (auto& kv : _collectors) {
            CollectorState& c = kv.second;
            if (makePending(kv.first)) continue;
            if (!c.desired || c.deadHeadRisk || anyMakeFailed(kv.first)) continue;
            // A blower started BY HAND has no routing plan behind it to guarantee
            // an open path, so the guarantee is checked here instead, at the
            // moment of starting and after its opening moves have drained.
            if (c.manualRun && !systemHasOpenPath(kv.first)) continue;
            c.running = true;
        }
    }

    // True while there are moves queued or one in flight (anywhere in the shop).
    bool transitioning() const { return !_inFlightSystem.empty() || !_queue.empty(); }

    // True while THIS system still has moves pending or in flight.
    bool transitioning(const std::string& systemId) const {
        if (_inFlightSystem == systemId) return true;
        for (const QueuedMove& q : _queue) if (q.systemId == systemId) return true;
        return false;
    }

    // Should this system's dust collector be energized right now?
    bool collectorOn(const std::string& systemId) const {
        auto it = _collectors.find(systemId);
        return it != _collectors.end() && it->second.running;
    }
    // Is ANY blower running. The single-collector shorthand the sketch and the
    // v1 status field still use.
    bool collectorOn() const {
        for (auto& kv : _collectors) if (kv.second.running) return true;
        return false;
    }
    bool collectorCoasting(const std::string& systemId) const {
        auto it = _collectors.find(systemId);
        return it != _collectors.end() && it->second.coasting;
    }
    bool collectorCoasting() const {
        for (auto& kv : _collectors) if (kv.second.coasting) return true;
        return false;
    }
    bool deadHeadRisk(const std::string& systemId) const {
        auto it = _collectors.find(systemId);
        return it != _collectors.end() && it->second.deadHeadRisk;
    }
    bool deadHeadRisk() const {
        for (auto& kv : _collectors) if (kv.second.deadHeadRisk) return true;
        return false;
    }

    std::vector<std::string> systemIds() const {
        std::vector<std::string> out;
        for (const SystemView& sys : systemsOf(topology())) out.push_back(sys.id ? sys.id : "");
        return out;
    }

    // The switchable plug for one system's blower ("" if the layout names none).
    JsonObjectConst collectorOutlet(const std::string& systemId) const {
        for (const SystemView& sys : systemsOf(topology())) {
            if (std::string(sys.id ? sys.id : "") != systemId) continue;
            return collectorOf(sys)["control"]["outlet"];
        }
        return JsonObjectConst();
    }

    // The SENSE-ONLY plug watching one system's blower ("" if none).
    //
    // Independent of collectorOutlet() above, and the distinction is the whole
    // closed loop (2026-09-10). `control.outlet` is how we SWITCH a blower;
    // `sensor.outlet` is how we WATCH one. A collector commanded by a servo
    // pressing its remote, or by RF, has no control.outlet at all — and every
    // way we press that button is STATELESS, so watching it is the only way to
    // know whether the press landed. See docs/tool-sensing-rfc.md §4.2a/§4.2b.
    //
    // A collector switched by a metering Shelly needs no second device: it
    // senses itself through the plug that switches it, and the sketch falls back
    // to the control plug when this is absent.
    JsonObjectConst collectorSensorOutlet(const std::string& systemId) const {
        for (const SystemView& sys : systemsOf(topology())) {
            if (std::string(sys.id ? sys.id : "") != systemId) continue;
            return collectorOf(sys)["sensor"]["outlet"];
        }
        return JsonObjectConst();
    }

    // Coast-down for one system. Absent means "the shop didn't say", not "none"
    // — see kDefaultCollectorOffDelayMs. An explicit 0 does disable it.
    // The RF transmitter that presses this collector's remote ("" if none).
    // A collector with this has no control.outlet — validateTopology() refuses
    // both, because two ways to command one blower fight each other.
    // Does a CLAMP watch this system's blower? Then pollSensors() owns its
    // reading, and nothing else may write one: a plug's watts landing on top of
    // the clamp's (firmware.ino's collector feed) is two sensors taking turns, and
    // the one the layout chose loses half the time. Same lookup pollSensors uses.
    bool collectorHasClamp(const std::string& systemId) const {
        for (const SystemView& sys : systemsOf(topology())) {
            if (std::string(sys.id ? sys.id : "") != systemId) continue;
            return !clampOf(topology(), collectorOf(sys)).isNull();
        }
        return false;
    }

    JsonObjectConst collectorRf(const std::string& systemId) const {
        for (const SystemView& sys : systemsOf(topology())) {
            if (std::string(sys.id ? sys.id : "") != systemId) continue;
            return collectorOf(sys)["control"]["rf"];
        }
        return JsonObjectConst();
    }

    uint32_t collectorOffDelayMs(const std::string& systemId) const {
        for (const SystemView& sys : systemsOf(topology())) {
            if (std::string(sys.id ? sys.id : "") != systemId) continue;
            JsonVariantConst d = collectorOf(sys)["control"]["offDelayMs"];
            return d.isNull() ? kDefaultCollectorOffDelayMs : d.as<uint32_t>();
        }
        return kDefaultCollectorOffDelayMs;
    }

    // Per-selector state the brain believes each actuator is in (shop-wide;
    // selector ids are unique across systems).
    const std::map<std::string, std::string>& actuatorStates() const {
        return _ctrl.actuatorStates();
    }
    const ShopRouting& routing() const { return _ctrl.lastRouting(); }
    std::vector<std::string> activeMachines() const {
        return _loaded ? _ctrl.activeMachines() : std::vector<std::string>();
    }
    const std::vector<FailedMove>& failedMoves() const { return _failed; }

    // Raise or refresh a problem under `key` (idempotent: call it every pass and
    // it keeps its original sinceMs). Clear it when the cause is gone.
    void raiseProblem(const std::string& key, const char* code, const char* severity,
                      const char* subjectType, const std::string& subjectId,
                      const std::string& text, uint32_t nowMs) {
        auto it = _raised.find(key);
        if (it != _raised.end()) { it->second.text = text; return; }
        _raised[key] = Problem{code, severity, subjectType, subjectId, text, nowMs};
    }
    void clearProblem(const std::string& key) { _raised.erase(key); }
    bool hasProblem(const std::string& key) const { return _raised.count(key) > 0; }

    // Does this system's collector name a switchable outlet?
    bool collectorHasOutlet(const std::string& systemId) const {
        for (const SystemView& sys : systemsOf(topology())) {
            if (std::string(sys.id ? sys.id : "") != systemId) continue;
            return !collectorOf(sys)["control"]["outlet"].isNull();
        }
        return false;
    }

    // Serialize the live view into `out`, matching statusView() in
    // shared/device-model/topology-device.js field-for-field so the Live view and
    // the conformance suite see the same shape from firmware and mock:
    //   { actuators, tools, collectorOn, conflicts, reachable }
    // Plus `transitioning`, `failed` and `systems`, which the mock has no
    // analogue for — additive, so the contract holds.
    //
    // `collectorOn` at the top level stays "is ANY blower running". It is the
    // field a single-collector shop has always read, and for the overwhelmingly
    // common one-system case it means exactly what it did. Per-blower truth lives
    // in `systems`, which is where a caller that knows about N systems should
    // look — collapsing two blowers into one boolean is fine for a summary and
    // wrong for a decision.
    void writeStatus(JsonObject out) const {
        JsonObject actuators = out.createNestedObject("actuators");
        JsonObject tools     = out.createNestedObject("tools");
        if (!_loaded) {
            out["collectorOn"] = false;
            out.createNestedArray("conflicts");
            out.createNestedObject("reachable");
            out.createNestedObject("machines");
            out.createNestedObject("systems");
            return;
        }

        for (auto& kv : _ctrl.actuatorStates()) {
            if (kv.second.empty()) actuators[kv.first] = (const char*)nullptr;
            else                   actuators[kv.first] = kv.second;
        }

        // `tools` is keyed by MACHINE, not by port: it answers "what is running",
        // and what runs is a machine. A two-port saw appears once, as it should.
        for (const std::string& id : machineIds(topology())) {
            float w = _ctrl.machineWatts(id);
            JsonObject t = tools.createNestedObject(id);
            t["watts"]  = w;
            t["active"] = w >= _ctrl.machineThreshold(id);
            // So the Live view can show WHY a tool is on — a hand-thrown switch
            // reads differently from a tool the shop noticed by itself.
            if (_manual.count(id)) t["manual"] = true;
        }

        out["collectorOn"] = collectorOn();
        // Additive, like `transitioning`: lets the Live view say "coasting down"
        // instead of showing a blower running with every tool off, which reads as
        // a stuck relay.
        if (collectorCoasting()) out["collectorCoasting"] = true;

        const ShopRouting& r = _ctrl.lastRouting();
        JsonArray conflicts = out.createNestedArray("conflicts");
        for (const ShopConflict& c : r.conflicts) {
            JsonObject o = conflicts.createNestedObject();
            o["systemId"]    = c.systemId;
            o["selectorId"]  = c.selectorId;
            o["winner"]      = c.winner;
            o["winnerState"] = c.winnerState;
            JsonArray losers = o.createNestedArray("losers");
            for (const std::string& l : c.losers) losers.add(l);
        }

        // Keyed by PORT id — the thing that either got air or didn't.
        JsonObject reachable = out.createNestedObject("reachable");
        for (auto& kv : r.reachable) reachable[kv.first] = kv.second;

        // The rolled-up verdict per machine. `stripped` is the one the UI needs
        // to shout about: a machine running with a primary port shut.
        JsonObject machines = out.createNestedObject("machines");
        for (auto& kv : r.machines) {
            JsonObject m = machines.createNestedObject(kv.first);
            m["status"] = machineStatusName(kv.second.status);
            JsonArray routed = m.createNestedArray("routed");
            for (const std::string& p : kv.second.routed) routed.add(p);
            JsonArray blocked = m.createNestedArray("blocked");
            for (const std::string& p : kv.second.blocked) blocked.add(p);
        }

        JsonObject systems = out.createNestedObject("systems");
        for (auto& kv : _collectors) {
            JsonObject s = systems.createNestedObject(kv.first);
            s["collectorOn"]   = kv.second.running;
            s["coasting"]      = kv.second.coasting;
            // So the Live view can say WHY a blower is running with nothing on it,
            // and offer the switch that ends it. Mirrors statusView() in
            // topology-device.js.
            s["manual"]        = kv.second.manualRun;
            s["deadHeadRisk"]  = kv.second.deadHeadRisk;
            s["transitioning"] = transitioning(kv.first);
            // Omitted entirely when no plug has reported: an all-zero reading
            // would render as a dead blower rather than an absent one, which is
            // the opposite of the truth for a shop that starts its collector by
            // hand. Mirrors statusView() in topology-device.js.
            if (kv.second.plugKnown) {
                JsonObject p = s.createNestedObject("plug");
                p["watts"]     = kv.second.plugWatts;
                p["reachable"] = kv.second.plugReachable;
                p["onForMs"]   = commandedForMs(kv.second);
                // The verdict, alongside the facts it came from. Both are sent:
                // the facts because a client may want to render the number, and
                // the state because the device now has an opinion of its own and
                // the OLED reads it without re-deriving anything. A client that
                // computes it from the facts must get the same answer — that is
                // what the matched pair is for.
                p["state"] = plugStateName(collectorPlugStateFor(kv.first));
            }
            // Omitted when nothing is watching this bin, for the same reason the
            // plug is: absent and empty are different claims. Mirrors
            // statusView() in topology-device.js.
            if (kv.second.binKnown) {
                JsonObject b = s.createNestedObject("bin");
                b["full"] = kv.second.binFull;
            }
        }

        out["transitioning"] = transitioning();
        writeProblems(out);
        JsonArray failed = out.createNestedArray("failed");
        for (const FailedMove& f : _failed) {
            JsonObject o = failed.createNestedObject();
            o["systemId"]   = f.systemId;
            o["selectorId"] = f.selectorId;
            o["toState"]    = f.toState;
            o["reason"]     = f.reason;
        }
    }

    // `problems`: derived collector verdicts, failed moves, then whatever
    // firmware raised. Mirrors problemsView() in topology-device.js.
    void writeProblems(JsonObject out) const {
        JsonArray arr = out.createNestedArray("problems");
        auto add = [&](const char* code, const char* sev, const char* st, const std::string& id,
                       const std::string& text, bool hasSince, uint32_t since) {
            JsonObject o = arr.createNestedObject();
            o["code"] = code; o["severity"] = sev;
            JsonObject sub = o.createNestedObject("subject");
            sub["type"] = st; sub["id"] = id;
            o["text"] = text;
            if (hasSince) o["forMs"] = (uint32_t)(_nowMs - since);
        };
        for (auto& kv : _collectors) {
            const topo::PlugState st = collectorPlugStateFor(kv.first);
            if (st == topo::PlugState::NotStarting)
                add("collector-no-start", "bad", "system", kv.first, kProblemNoStart, false, 0);
            else if (st == topo::PlugState::Unknown && kv.second.running &&
                     (kv.second.plugKnown || collectorHasOutlet(kv.first) || collectorHasClamp(kv.first)))
                add("collector-blind", "bad", "system", kv.first, kProblemBlind, false, 0);
        }
        for (auto& kv : _stuck) {
            const FailedMove& f = kv.second;
            add("move-failed", f.isBreak ? "warn" : "bad", "selector", f.selectorId,
                std::string("Move to ") + f.toState + " failed: " + f.reason, false, 0);
        }
        for (auto& kv : _raised)
            add(kv.second.code.c_str(), kv.second.severity.c_str(), kv.second.subjectType.c_str(),
                kv.second.subjectId, kv.second.text, true, kv.second.sinceMs);
    }

    // Feed in what the bin sensor says. The CALLER owns the pin, the debounce
    // and the question of which system this is — see localBinSystemId() in
    // utils/BinSensor.h. Calling it at all is what makes `bin` appear in the
    // status; a system nobody calls this for stays silent.
    void setBinFull(const std::string& systemId, bool full) {
        auto it = _collectors.find(systemId);
        if (it == _collectors.end()) return;   // no such system: say nothing
        it->second.binKnown = true;
        it->second.binFull  = full;
    }

    // This system's collector's bin sensor, or a null object if it has none.
    // The caller reads `invert` off it — the inversion belongs to the WIRING,
    // not to the firmware build (see boards/xiao_c5.h).
    JsonObjectConst binSensorFor(const std::string& systemId) const {
        if (!_doc) return JsonObjectConst();
        for (const SystemView& sys : systemsOf(topology())) {
            if (!sys.id || systemId != sys.id) continue;
            for (JsonObjectConst e : sys.elements)
                if (_eq(e["type"], "collector")) return e["bin"]["sensor"];
        }
        return JsonObjectConst();
    }

    JsonObjectConst selectorById(const std::string& id) const {
        if (!_doc) return JsonObjectConst();
        for (const SystemView& sys : systemsOf(topology()))
            for (JsonObjectConst e : sys.elements)
                if (_eq(e["id"], id.c_str())) return e;
        return JsonObjectConst();
    }

private:
    // Adopt a fresh decision. The queue is REBUILT (not appended to): a newer
    // decision always supersedes a pending older one — most-recent-wins applies
    // to the plan too. Crucially it is rebuilt from `_hwStates`, so any move the
    // previous plan hadn't executed yet is still in the new one if it's still
    // needed.
    void ingest(const ReconcileResult& r) {
        _failed.clear();
        _queue.clear();
        // A machine SWITCHING ON re-asserts every servo gate in its systems.
        for (const std::string& mid : r.switchedOn) markReassert(mid);

        // What each blower is doing RIGHT NOW, which is what the dead-head
        // question is asked against.
        std::map<std::string, bool> running;
        for (auto& kv : _collectors) running[kv.first] = kv.second.running;

        std::vector<SystemPlan> plans =
            planShopTransition(topology(), _hwStates, r.routing.states, running, &_reassert);

        for (auto& kv : _collectors) {
            const std::string& sysId = kv.first;
            CollectorState&    c     = kv.second;

            auto ait = r.systemActive.find(sysId);
            c.desired = ait != r.systemActive.end() && ait->second;

            const SystemPlan* plan = nullptr;
            for (const SystemPlan& p : plans) if (p.systemId == sysId) { plan = &p; break; }

            if (!c.desired && c.manualRun) {
                // Running by hand. Held exactly like an idle system — its gates
                // stay where they are and its moves are not queued — but the
                // blower is WANTED, so it goes through the same ON path an active
                // system uses: update() starts it once this system's moves drain.
                //
                // The dead-head question is asked against the gates we are
                // actually leaving it at, NOT against the plan. The plan's
                // destination for a system with nothing running is "all closed",
                // which is the one destination that dead-heads — and the moves
                // that would get there are precisely the ones being dropped.
                c.desired  = true;
                c.coasting = false;
                // Queued HERE and not in setCollectorManual(): ingest() rebuilds
                // the queue from scratch on every reconcile, so moves pushed
                // before it are thrown away — and re-deriving them each pass also
                // means a system still sealed when the next power event lands
                // gets another go at opening.
                queueOpenPath(sysId);
                // Whether it may actually start is asked in update(), against the
                // gates as they stand once these moves have drained. A verdict
                // recorded here would be a verdict about the system BEFORE the
                // moves that fix it.
                c.deadHeadRisk = false;
                continue;
            }

            if (!c.desired) {
                // Idle. Policy is HOLD: leave this system's gates exactly where
                // they are rather than driving it closed (routing.states would say
                // "all closed", which is the one destination that can dead-head).
                // Its moves are simply not queued — see the queue rebuild below.
                //
                // Judged against the gates AS THEY STAND, not the plan: the plan's
                // destination for an idle system is "all closed", which would
                // dead-head — and is exactly the destination being dropped. Reading
                // plan->deadHeadRisk here reported a dead-head after every tool
                // turned off while the gates were in fact left open (2026-10-03).
                c.deadHeadRisk = c.running && !systemHasOpenPath(sysId);
                // Idle HOLDS its gates, so a re-assert still pending here is
                // dropped with the rest of its moves — the next switch-on marks
                // them again.
                forgetReassert(sysId);
                // Coast rather than cut. Only from a RUNNING blower: if it was
                // already off there's nothing to coast, and starting a timer would
                // just delay the next honest decision.
                if (c.running && !c.coasting && collectorOffDelayMs(sysId) > 0) {
                    c.coasting     = true;
                    c.coastUntilMs = _nowMs + collectorOffDelayMs(sysId);
                } else if (!c.coasting) {
                    c.running = false;
                }
                continue;
            }

            // A machine is running on this system again — whatever we were
            // coasting toward is moot.
            c.coasting = false;
            c.deadHeadRisk = plan && plan->deadHeadRisk;
            // A dead-head OFF is a safety stop: immediate, and it cancels a coast.
            // ON waits for this system's moves to drain (see update()).
            if (c.deadHeadRisk) c.running = false;

            // Plans are concatenated in system order, never interleaved — see the
            // header note. Only ACTIVE systems contribute moves; an idle one is
            // held, which is what makes coasting safe.
            if (plan)
                for (const Move& m : plan->moves) _queue.push_back(QueuedMove{sysId, m});
        }
    }

    // Is an OPENING move for this system still queued or moving? The collector
    // waits for these, and only these — see update().
    bool makePending(const std::string& systemId) const {
        if (_inFlightIsMake && _inFlightSystem == systemId) return true;
        for (const QueuedMove& q : _queue)
            if (q.systemId == systemId && !q.move.isBreak) return true;
        return false;
    }

    // Every servo selector in the systems this machine feeds, marked to be
    // commanded on the next plan whether or not we believe it is already right.
    // Mirrors reassertFor() in topology-device.js. Sliders are skipped later, by
    // planTransition, so this can stay "every selector".
    void markReassert(const std::string& machineId) {
        auto ports = portsByMachine(topology());
        auto pit = ports.find(machineId);
        if (pit == ports.end()) return;
        std::set<std::string> systems;
        for (const PortRef& pr : pit->second) if (portEnabled(pr.port)) systems.insert(pr.systemId);
        for (const SystemView& sys : systemsOf(topology())) {
            if (!systems.count(sys.id ? sys.id : "")) continue;
            for (JsonObjectConst e : sys.elements)
                if (_eq(e["type"], "selector")) _reassert.insert(_str(e["id"]));
        }
    }

    void forgetReassert(const std::string& systemId) {
        for (const SystemView& sys : systemsOf(topology())) {
            if (systemId != (sys.id ? sys.id : "")) continue;
            for (JsonObjectConst e : sys.elements)
                if (_eq(e["type"], "selector")) _reassert.erase(_str(e["id"]));
        }
    }

    // Is anything in this system open — the same test planTransition calls "not
    // sealed", asked of the gates as they actually stand. A system with no
    // selectors at all is open by construction: there is nothing there to close.
    bool systemHasOpenPath(const std::string& systemId) const {
        bool any = false;
        for (const SystemView& sys : systemsOf(topology())) {
            if (std::string(sys.id ? sys.id : "") != systemId) continue;
            for (JsonObjectConst e : sys.elements) {
                if (!_eq(e["type"], "selector")) continue;
                any = true;
                std::string id = _str(e["id"]);
                const char* closed = _closedState(e);
                auto it = _hwStates.find(id);
                // Unknown position counts as CLOSED. Nothing has been commanded,
                // so the honest answer is "we don't know", and guessing "open"
                // here is the guess that starts a blower into a sealed system.
                if (it == _hwStates.end()) continue;
                if (!closed || it->second != std::string(closed)) return true;
            }
        }
        return !any;
    }

    // Does any machine drawing power have an enabled port in this system?
    bool machineActiveOn(const std::string& systemId) const {
        auto ports = portsByMachine(topology());
        for (const std::string& mid : _ctrl.activeMachines()) {
            auto pit = ports.find(mid);
            if (pit == ports.end()) continue;
            for (const PortRef& pr : pit->second)
                if (pr.systemId == systemId && portEnabled(pr.port)) return true;
        }
        return false;
    }

    // Open SOMETHING in this system, so a hand-started blower has somewhere to
    // pull from. Routes toward the system's first machine in document order —
    // the same choice openAPath() makes in topology-device.js, and for the same
    // reason: the only way to reach a sealed system is a layout just adopted or a
    // dead-head stop, and in neither is there a "last one served" worth keeping.
    // A system already open is left exactly as it is; idle-hold means the shop
    // rests where you left it, and reshuffling gates nobody asked about is its own
    // kind of surprise.
    void queueOpenPath(const std::string& systemId) {
        if (systemHasOpenPath(systemId)) return;
        auto ports = portsByMachine(topology());
        std::string target;
        for (const std::string& mid : machineIds(topology())) {
            auto pit = ports.find(mid);
            if (pit == ports.end()) continue;
            for (const PortRef& pr : pit->second)
                if (pr.systemId == systemId && portEnabled(pr.port)) { target = mid; break; }
            if (!target.empty()) break;
        }
        if (target.empty()) return;    // a system with no machines has nothing to open toward

        ShopRouting r = routeShop(topology(), { target });
        std::map<std::string, std::string> desired;
        for (const SystemView& sys : systemsOf(topology())) {
            if (std::string(sys.id ? sys.id : "") != systemId) continue;
            for (JsonObjectConst e : sys.elements) {
                if (!_eq(e["type"], "selector")) continue;
                std::string id = _str(e["id"]);
                auto sit = r.states.find(id);
                if (sit != r.states.end()) desired[id] = sit->second;
            }
        }
        // Through the ordinary queue, so these are make-before-break and ON still
        // waits for them — a hand start is not a shortcut past the safety rule.
        std::map<std::string, bool> running;
        for (auto& kv : _collectors) running[kv.first] = kv.second.running;
        for (const SystemPlan& p : planShopTransition(topology(), _hwStates, desired, running)) {
            if (p.systemId != systemId) continue;
            for (const Move& m : p.moves) _queue.push_back(QueuedMove{systemId, m});
        }
    }

    bool anyMakeFailed(const std::string& systemId) const {
        for (const FailedMove& f : _failed)
            if (!f.isBreak && f.systemId == systemId) return true;
        return false;
    }

    // ── CT-sensed tools (tool-sensing RFC §5.6) ────────────────────────────
    //
    // A tool with `sensor.ct` is watched by a clamp on some board rather than by
    // a plug the primary polls over HTTP. The board decides the one bit (§5.4b —
    // a CT cannot give watts, and a woodworking tool's standby is under the
    // noise floor) and reports it unsolicited; this half turns that bit into the
    // only currency the routing brain has.
    //
    // THE SENSOR ID IS THE ELEMENT ID. Deliberately, and it is the reason there
    // is no lookup table anywhere: CONFIG carries it out, SENSE echoes it back,
    // and the id that comes home is already the machine id to act on. Nothing
    // can get out of step because nothing is mapped.

    // Push every board the WHOLE list of sensors the layout gives it.
    //
    // Sent to every controller in the document, including the ones with no CT at
    // all — an empty list is how a sensor gets REMOVED, and a board that simply
    // stopped being mentioned would otherwise go on reporting a tool that the
    // layout no longer believes in.
    // THIS BOARD ANSWERS TO TWO NAMES — "" and its own controllerId — and that
    // is not a detail. Bucketing naively by the id string configured the local
    // bus TWICE on adopt: once for "" (carrying its clamps) and once for
    // "primary" (empty, because no element spells it that way), and the second
    // push silently erased the first. Found by the test below, 2026-09-15.
    //
    // bareHost() rather than ==, for NodeBus's own reason: the same board is
    // legitimately "node-1" and "node-1.local".
    bool sameBoard(const std::string& a, const std::string& b) const {
        return topo::sameBoard(a, b, _bus ? _bus->ownControllerId() : std::string());
    }

    // The shop-wide CT tuning, 0 = unset. See setSensorTuning().
    float _tripRatio  = 0.0f;
    float _minCounts  = 0.0f;
    float _clearRatio = 0.0f;

    // The layout's sensors, resolved once — see SensorPlan.h. The push serialises them per board;
    // pollSensors() reads them back by the same ids.
    std::vector<PlannedSensor> sensorPlan() const {
        return planSensors(topology(), [this](const std::string& mid) { return _ctrl.machineThreshold(mid); });
    }

    void pushSensorConfig() {
        if (!_bus) return;
        _nodePlugs.clear();
        const std::vector<PlannedSensor> plan = sensorPlan();
        // One bucket per BOARD, not per id. "" is this board and is pushed
        // first, so any controller id that also means this board is skipped
        // rather than overwriting what "" just sent.
        std::vector<std::string> ids;
        ids.push_back(std::string());                     // "" = this board
        for (const SystemView& sys : systemsOf(topology())) {
            for (JsonObjectConst c : sys.controllers) {
                const std::string cid = c["id"] | "";
                if (cid.empty()) continue;
                bool dup = false;
                for (const std::string& seen : ids) if (sameBoard(seen, cid)) { dup = true; break; }
                if (!dup) ids.push_back(cid);
            }
        }
        for (const std::string& cid : ids) {
            // 1536, not 512, and RESIZED AGAIN on 2026-09-17 when the tuning
            // fields landed. kMaxSensorsPerNode is 4 and a sensorId may be 48
            // chars (kMaxSensorIdLen), so a full frame is now 4 x (48-char
            // string + SIX members + object overhead) — it was three members
            // when 1024 was chosen, and doubling the member count is exactly the
            // kind of change that walks a doc off its own cliff in silence. Overflow here does not
            // fail: it SILENTLY DROPS members, and a dropped sensorId is a board
            // that is deaf to one tool while reporting nothing wrong. Same
            // failure that ate caps.ct on 2026-09-15 and would have eaten the
            // tail of the board list on 2026-09-16.
            DynamicJsonDocument doc(1536);
            JsonArray arr = doc.to<JsonArray>();
            const bool thisBoard = sameBoard(cid, std::string());

            // CLAMPS — this board's own ADC. Absent controllerId means THIS board, the same rule
            // as the bin sensor, every selector, and NodeBus itself.
            for (const PlannedSensor& p : plan) {
                if (p.kind != PlannedSensor::Kind::Clamp || !sameBoard(cid, p.board)) continue;
                JsonObject sen = arr.createNestedObject();
                sen["sensorId"] = p.id;
                sen["kind"]     = "ct";
                sen["channel"]  = p.channel;
                // OMITTED, not zeroed: an absent key means "keep your own",
                // where a present 0 is a value parseConfigFrame refuses —
                // and it refuses the WHOLE frame, so a board would end up
                // watching nothing rather than watching with old numbers.
                if (_tripRatio  != 0.0f) sen["tripRatio"]  = _tripRatio;
                if (_minCounts  != 0.0f) sen["minCounts"]  = _minCounts;
                if (_clearRatio != 0.0f) sen["clearRatio"] = _clearRatio;
            }
            // BINS on a node (2026-10-04). Only for a board that SAID it has the pad (WELCOME
            // caps.bin): a node that predates it would refuse the whole CONFIG, clamp and all. An
            // absent controllerId means THIS board, whose pin the sketch reads itself, so it is
            // never sent.
            if (!thisBoard && _bus->watchesBin(cid.c_str())) {
                for (const PlannedSensor& p : plan) {
                    if (p.kind != PlannedSensor::Kind::Bin || p.board.empty() || !sameBoard(cid, p.board)) continue;
                    if (arr.size() >= nodelink::kMaxSensorsPerNode) break;
                    JsonObject sen = arr.createNestedObject();
                    sen["sensorId"] = p.id;
                    sen["kind"]     = "bin";
                    sen["invert"]   = p.invert;
                }
            }
            // PLUGS this board polls for the brain (2026-10-03). The board that controls a tool
            // handles its plug — the owner rule is plugOwnerOf() in Shop.h, a matched pair with
            // shop.js's plugOwners(). Only for a board that SAID it can (WELCOME caps.plug): an
            // older node would refuse the whole CONFIG, clamp and all, so its plugs stay with the
            // brain. A machine with a clamp is sensed by the clamp (the plan leaves it out). And
            // the board is never THIS one — the brain polls its own plugs through
            // SmartOutletControl.
            if (!thisBoard && _bus->pollsPlugs(cid.c_str())) {
                for (const PlannedSensor& p : plan) {
                    if (p.kind != PlannedSensor::Kind::Plug || !sameBoard(cid, p.board)) continue;
                    if (arr.size() >= nodelink::kMaxSensorsPerNode) break;   // the rest stay with the brain
                    JsonObject sen = arr.createNestedObject();
                    sen["sensorId"]   = p.id;
                    sen["kind"]       = "plug";
                    sen["ip"]         = p.ip;
                    sen["plug"]       = p.tasmota ? "tasmota" : "shelly";
                    sen["thresholdW"] = p.thresholdW;
                    _nodePlugs[p.id]  = cid;
                }
            }
            _bus->configureSensors(cid.c_str(), JsonArrayConst(arr));
        }
        _plugSig = plugCapSignature();
    }

    // Which boards can poll plugs right now, as one string. A node's capability
    // arrives in its WELCOME, which is AFTER the layout was adopted, so a push made
    // at adopt time did not know it — update() compares this against the one the
    // last push saw and pushes again when a board has joined or left.
    std::string plugCapSignature() const {
        std::string sig;
        for (const SystemView& sys : systemsOf(topology())) {
            for (JsonObjectConst c : sys.controllers) {
                const std::string cid = c["id"] | "";
                if (cid.empty() || sameBoard(cid, std::string())) continue;
                if (_bus->pollsPlugs(cid.c_str())) { sig += cid; sig += ';'; }
                if (_bus->watchesBin(cid.c_str()))  { sig += cid; sig += ":bin;"; }
            }
        }
        return sig;
    }
    std::string _plugSig;
    // machineId -> the controllerId polling its plug, as of the last push. The
    // single source for "is this plug node-polled?" — pollSensors and the sketch
    // both ask it, so they cannot disagree with what was actually sent.
    std::map<std::string, std::string> _nodePlugs;

    // Turn each CT's bit into a power reading, once per update().
    //
    // Expressed as a SYNTHETIC WATTAGE rather than a new concept, exactly as
    // setMachineManual() is, and for the identical reason: the routing brain has
    // one notion of "active", and a second one would have to be taught
    // most-recent-wins, coast-down and the dead-head rule all over again.
    //
    // ABSENT IS OFF (RFC §5.6a). A board that never reported, or has gone away
    // with the tool it is powered from, reads as off — which fails toward a
    // dusty shop rather than a collector that runs forever, and cannot
    // dead-head anything because idle leaves the gate where it is.
    void pollSensors() {
        // The same plan the push serialised, so a reading is looked up by the id we SENT.
        const std::vector<PlannedSensor> plan = sensorPlan();
        for (const PlannedSensor& p : plan) {
            if (p.kind == PlannedSensor::Kind::Clamp) {
                const char* id  = p.id.c_str();
                bool on = false; uint32_t atMs = 0;
                bool reported = _bus->senseOf(p.board.c_str(), id, on, atMs);
                if (!reported) on = false;
                // STALE IS NOT THE SAME AS OFF, and this treats it as off on purpose while the
                // distinction has nowhere to be shown: a board still answering PINGs but no longer
                // reporting is a FAULT, and when there is a UI for it this is where it gets raised.
                // Skipped entirely when nowMs is 0 — the test call sites pass a constant clock, and
                // a zero "now" would age every reading out.
                else if (_nowMs && (uint32_t)(_nowMs - atMs) > nodelink::kSenseStaleMs) {
                    on = false; reported = false;
                }

                // How long it has been on, which only the collector path needs (a spin-up grace)
                // but which is cheapest to track for both.
                uint32_t& since = _ctSince[p.id];
                if (!on) since = 0;
                else if (!since) since = _nowMs ? _nowMs : 1;

                // ── A COLLECTOR IS NOT A MACHINE ───────────────────────────
                //
                // A tool's reading answers "should the collector run"; a COLLECTOR's answers "did
                // the thing we commanded actually happen", which is a different question with a
                // different consumer. Routing a blower through setMachinePower() would reach
                // machineIndex(), find nothing, and do nothing at all — silently, which is the
                // failure mode this whole path exists to avoid. CollectorPlugState is where a
                // blower's own draw is judged, so a clamp feeds that instead.
                //
                // It matters MORE on a collector than on a tool: every way we command a blower is
                // stateless (a servo on a fob, an RF frame), so `sensor` is the only thing that can
                // say the press landed.
                if (p.onCollector) {
                    const uint32_t onFor = (on && since && _nowMs) ? (_nowMs - since) : 0;
                    // Synthetic watts, the same trick used for a manual machine: the brain has ONE
                    // notion of a running blower and a clamp that cannot give watts still has to
                    // speak it. Comfortably over kCollectorRunningW rather than equal to it, so a
                    // change to that threshold cannot silently strand a clamp.
                    setCollectorPlug(p.systemId, on ? kCollectorRunningW * 2.0f : 0.0f, reported, onFor);
                    continue;
                }
                setMachinePower(p.id, on ? manualWattsFor(_ctrl.machineThreshold(p.id)) : 0.0f);
            } else if (p.kind == PlannedSensor::Kind::Bin) {
                // BINS a node watches. A reading only counts while it is fresh; a bin whose board
                // has gone quiet keeps its LAST verdict rather than flipping to "not full", and one
                // that never reported stays unknown (the status omits `bin`), because an unwatched
                // bin and an empty bin are different claims. An absent board is THIS board's own
                // pin: the sketch feeds it.
                if (p.board.empty() || sameBoard(p.board, std::string())) continue;
                bool full = false; uint32_t atMs = 0;
                if (!_bus->senseOf(p.board.c_str(), p.id.c_str(), full, atMs)) continue;
                if (_nowMs && (uint32_t)(_nowMs - atMs) > nodelink::kSenseStaleMs) continue;
                setBinFull(p.systemId, full);
            }
        }

        // ── PLUGS a node polls (see pushSensorConfig) ──────────────────────
        // Real watts, not a synthetic one: a plug HAS them, and the same reading
        // feeds the brain's threshold, the Live view and the plug's own
        // reachability. ABSENT IS OFF for the same reason as a clamp — a node that
        // stopped reporting reads as an idle tool, never a running one.
        for (const auto& kv : _nodePlugs) {
            float w = 0.0f; bool fault = false; uint32_t atMs = 0;
            bool reported = _bus->plugReading(kv.second.c_str(), kv.first.c_str(), w, fault, atMs);
            if (reported && _nowMs && (uint32_t)(_nowMs - atMs) > nodelink::kSenseStaleMs) reported = false;
            const bool reachable = reported && !fault;
            _nodePlugReading[kv.first] = NodePlugReading{ reachable, reachable ? w : 0.0f };
            setMachinePower(kv.first, reachable ? w : 0.0f);
        }
        // A board that joined, or left, since the last push changes who can be
        // handed a plug. Checked here because every pass already runs this.
        if (_loaded && plugCapSignature() != _plugSig) pushSensorConfig();
    }

public:
    // Is this machine's plug polled by a NODE, and what did it last say? Read by
    // the sketch so the brain stops polling that plug itself and reports the node's
    // answer as the plug's reading (and its reachability — a plug nobody can reach
    // raises a problem either way).
    struct NodePlugReading { bool reachable; float watts; };
    bool nodePlug(const std::string& machineId, NodePlugReading& out) const {
        if (_nodePlugs.find(machineId) == _nodePlugs.end()) return false;
        auto it = _nodePlugReading.find(machineId);
        out = (it == _nodePlugReading.end()) ? NodePlugReading{ false, 0.0f } : it->second;
        return true;
    }
private:
    std::map<std::string, NodePlugReading> _nodePlugReading;

    // When each clamped element's reading last went true, for the collector's
    // spin-up grace. Keyed by element id; cleared to 0 the moment it reads off.
    std::map<std::string, uint32_t>      _ctSince;

    NodeBus*                             _bus = nullptr;
    std::unique_ptr<BigJsonDocument> _doc;
    Controller                           _ctrl;
    std::deque<QueuedMove>               _queue;
    std::map<std::string, Problem>       _raised;
    std::vector<FailedMove>              _failed;
    std::map<std::string, FailedMove>    _stuck;      // selectorId → its last failed move, until it next moves
    // What has actually been COMMANDED to hardware, as opposed to what the brain
    // has decided. Diverges from Controller::actuatorStates() whenever a plan is
    // superseded mid-flight; that divergence is exactly what makes re-planning
    // from here correct. See the header note.
    std::map<std::string, std::string>   _hwStates;
    // Machines switched on by hand. Held here rather than in the controller
    // because it's a device-layer concern: the brain only knows watts.
    std::set<std::string>                _manual;
    std::map<std::string, CollectorState> _collectors;   // systemId → blower
    // Which system owns the move currently in flight ("" = none). Only that
    // system's blower is held back by it.
    std::string                          _inFlightSystem;
    bool                                 _inFlightIsMake = false;   // an OPEN is moving — see makePending()
    // Servo selectors to command on the next plan even if we BELIEVE they are
    // already right — filled on a machine's rising edge (markReassert), emptied
    // as each one's move is issued. Kept HERE rather than in one plan because
    // ingest() rebuilds the queue on every poll tick: a re-assert living only in
    // the plan that made it would be dropped by the next tick before it ran.
    std::set<std::string>                _reassert;
    bool     _loaded = false;
    uint32_t _nowMs  = 0;
};

} // namespace topo

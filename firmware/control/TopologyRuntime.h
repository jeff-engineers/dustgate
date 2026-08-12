// =============================================================================
// TopologyRuntime.h — the device layer that turns decisions into motion.
//
// TopologyController.h calls this "a thin device layer (the main sketch)". This
// is it, factored out of the sketch so it stays host-testable. It owns:
//
//   • the parsed topology document (adopted at boot and on PUT /api/topology)
//   • a topo::Controller (the brain: tool power in, routed states + plan out)
//   • a MOVE QUEUE, drained one move at a time through NodeBus
//
// Why a queue and not a loop: TopologySequencer already orders the moves
// make-before-break, and issuing them one at a time — never starting the next
// until NodeBus::busy() clears — is what keeps the one-servo-at-a-time current
// budget honored (RFC §7). Executing the plan is therefore a small state machine
// pumped from loop(), not a blocking sequence.
//
// The runtime plans from HARDWARE truth, not from the brain's optimistic view.
// Controller::reconcile() adopts its routed states the moment it decides them,
// but the valves haven't moved yet — so re-planning off the controller's states
// would silently drop moves whenever two power events land between two loop
// passes (tool A off + tool B on in one poll tick: the routine case). The
// runtime therefore keeps its own `_hwStates` — what has actually been commanded
// — and re-runs planTransition() from there each time the destination changes.
// The brain owns the destination; the runtime owns the journey.
//
// Collector policy (the other half of "never dead-head"):
//   OFF at idle COASTS — the blower keeps running for control.offDelayMs after
//   the last tool stops drawing. A bandsaw spinning down still throws dust, and
//   without this the blower short-cycles between cuts on the table saw. Safe by
//   construction: an idle transition clears the queue and HOLDS every gate, so
//   no path can close underneath a coasting blower. (The stop-selection path
//   gets this for free from OUTLET_OFF_DEBOUNCE_MS, but a loaded topology
//   bypasses that path — so routing had no coast-down at all until this.)
//   OFF from dead-head risk is IMMEDIATE and cancels any coast — that off is a
//   safety stop, not an idle.
//   ON is deferred until the queue has drained, so the blower only ever starts
//   against an already-open path. A tool starting mid-coast just keeps it on.
//   A failed MAKE also holds it off: if the gate that was supposed to open
//   didn't (dead node, uncalibrated servo), running the blower would pull
//   against a closed system. A failed break only leaks suction, so it doesn't
//   block — one dead secondary shouldn't make the whole shop unusable.
//   plan.deadHeadRisk forces OFF regardless — a destination that seals the whole
//   system must never be reached with the blower running.
//
// PURE — ArduinoJson + STL only, NO Arduino.h. `nowMs` is injected so the host
// conformance test can drive time deterministically.
// =============================================================================

#pragma once
#include <ArduinoJson.h>
#include "TopologyController.h"
#include "NodeBus.h"
#include <deque>
#include <set>
#include <memory>
#include <string>
#include <vector>

namespace topo {

// A move that has been issued but whose bus rejected it (offline node, missing
// calibration). Surfaced through /api/status so the UI can say which gate.
// No default member initializers: the ESP32 toolchain builds at gnu++11, where
// that would make this a non-aggregate and break the brace-init call sites.
// Synthetic wattage for a manually-switched-on tool. Above any real threshold
// anyone would set (the schema's own default is 5 W).
static const float kManualWatts = 100000.0f;

// Coast-down used when the collector element doesn't name one. Not zero on
// purpose: every shop wants some, nobody has a UI to set it yet, and the
// stop-selection path's equivalent slack is 3 s. Matches the schema doc's example.
static const uint32_t kDefaultCollectorOffDelayMs = 4000;

struct FailedMove {
    std::string selectorId;
    std::string toState;
    std::string reason;
    bool        isBreak;   // a failed make blocks the blower; a failed break doesn't
};

class TopologyRuntime {
public:
    void begin(NodeBus* bus) { _bus = bus; }

    // Parse and adopt a topology document. Replaces any current one and resets
    // the brain (tool power history, actuator states seeded to closed). Returns
    // false with `err` set if the JSON won't parse.
    bool adopt(const char* json, size_t len, std::string& err) {
        // ArduinoJson v6 needs a heap doc sized for the parse tree, not the text.
        // 2x + slack covers the key/value overhead of these documents; a bad
        // guess surfaces as NoMemory rather than silent truncation.
        size_t cap = len * 2 + 2048;
        std::unique_ptr<DynamicJsonDocument> doc(new DynamicJsonDocument(cap));
        DeserializationError e = deserializeJson(*doc, json, len);
        if (e) { err = e.c_str(); return false; }
        if (!doc->is<JsonObject>()) { err = "topology must be an object"; return false; }

        _doc = std::move(doc);
        _ctrl.setTopology(_doc->as<JsonObjectConst>());
        _queue.clear();
        _failed.clear();
        // Physical position is unknown after a config change; seed the same way
        // the brain does (every selector at its closed state) so the two agree.
        _hwStates = _ctrl.actuatorStates();
        _inFlight = false;
        _collectorRunning = false;
        _coasting = false;
        _loaded = true;
        return true;
    }

    void clear() {
        _doc.reset();
        _queue.clear();
        _failed.clear();
        _manual.clear();
        _hwStates.clear();
        _inFlight = false;
        _collectorRunning = false;
        _coasting = false;
        _loaded = false;
    }

    bool loaded() const { return _loaded; }
    JsonObjectConst topology() const {
        return _doc ? _doc->as<JsonObjectConst>() : JsonObjectConst();
    }

    // Feed a live power reading for one tool. Reconciles and (re)builds the move
    // queue from the resulting plan. Safe to call every poll tick — an unchanged
    // decision produces an empty plan and leaves the queue alone.
    //
    // A tool under MANUAL override ignores its plug: the override is the whole
    // point ("just run the collector so I can clear a clog"), and letting a poll
    // tick reporting 0 W switch it back off a second later would make the button
    // look broken.
    void setToolPower(const std::string& toolId, float watts) {
        if (!_loaded) return;
        if (_manual.count(toolId)) return;
        ReconcileResult r = _ctrl.setToolPower(toolId, watts);
        ingest(r);
    }

    // Switch a tool on/off by hand, from the Live view. Every tool is overridable,
    // not just the ones without a plug — a sensed tool sometimes needs running
    // with the blower on for a reason the plug can't know about.
    //
    // Implemented as a synthetic power reading rather than a separate concept, so
    // the routing brain has exactly one notion of "active" and manual tools take
    // part in most-recent-wins alongside sensed ones.
    // Returns false when no such tool exists in the layout. Worth reporting: an
    // unknown id otherwise sets a wattage nothing reads and routes nothing, so a
    // typo'd or stale toolId looks exactly like a working switch that does nothing.
    bool setToolManual(const std::string& toolId, bool on) {
        if (!_loaded) return false;
        if (!hasTool(toolId)) return false;
        if (on) _manual.insert(toolId);
        else    _manual.erase(toolId);
        // Comfortably over any plausible thresholdW; off returns it to 0 W, which
        // is also what a plug reports for a tool at rest.
        ReconcileResult r = _ctrl.setToolPower(toolId, on ? kManualWatts : 0.0f);
        ingest(r);
        return true;
    }

    bool hasTool(const std::string& toolId) const {
        for (JsonObjectConst e : topology()["elements"].as<JsonArrayConst>())
            if (_eq(e["type"], "tool") && _eq(e["id"], toolId.c_str())) return true;
        return false;
    }

    bool toolIsManual(const std::string& toolId) const { return _manual.count(toolId) > 0; }

    // Map a Shelly plug identity to the tool it powers ("" if none). Thin
    // pass-through so callers don't need the controller.
    std::string toolForOutlet(const char* host, const char* ip) const {
        return _loaded ? _ctrl.toolForOutlet(host, ip) : std::string();
    }

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

        // Coast expiry is checked here rather than in ingest(): at idle no power
        // events arrive, so ingest() isn't called again and nothing would ever
        // switch the blower off.
        if (_coasting && (int32_t)(nowMs - _coastUntilMs) >= 0) {
            _coasting         = false;
            _collectorRunning = false;
        }

        if (_bus->busy()) return;          // a move is still in flight
        _inFlight = false;

        if (!_queue.empty()) {
            Move m = _queue.front();
            _queue.pop_front();
            JsonObjectConst sel = selectorById(m.selectorId);
            if (sel.isNull()) {
                _failed.push_back({m.selectorId, m.toState, "unknown selector", m.isBreak});
            } else if (!_bus->onlineFor(sel)) {
                _failed.push_back({m.selectorId, m.toState, "controller offline", m.isBreak});
            } else if (!_bus->setState(m.selectorId.c_str(), sel, m.toState.c_str())) {
                _failed.push_back({m.selectorId, m.toState, "actuator rejected move", m.isBreak});
            } else {
                _hwStates[m.selectorId] = m.toState;   // commanded → hardware truth
                _inFlight = true;
            }
            return;                         // one move per pass, busy() gates the next
        }

        // Queue drained and nothing moving — safe to start the blower if the
        // brain wants it. See the collector policy in the header comment.
        if (_collectorDesired && !_deadHeadRisk && !anyMakeFailed()) _collectorRunning = true;
    }

    // True while there are moves queued or one in flight.
    bool transitioning() const { return _inFlight || !_queue.empty(); }

    // Should the dust collector be energized right now?
    bool collectorOn() const { return _collectorRunning; }
    // True while it's only still on to finish coasting down.
    bool collectorCoasting() const { return _coasting; }

    // Coast-down for this layout. Absent means "the shop didn't say", not "none"
    // — see kDefaultCollectorOffDelayMs. An explicit 0 does disable it.
    uint32_t collectorOffDelayMs() const {
        for (JsonObjectConst e : topology()["elements"].as<JsonArrayConst>()) {
            if (!_eq(e["type"], "collector")) continue;
            JsonVariantConst d = e["control"]["offDelayMs"];
            return d.isNull() ? kDefaultCollectorOffDelayMs : d.as<uint32_t>();
        }
        return kDefaultCollectorOffDelayMs;
    }

    // Per-selector state the brain believes each actuator is in.
    const std::map<std::string, std::string>& actuatorStates() const {
        return _ctrl.actuatorStates();
    }
    // toolId → won a clear path on the last reconcile.
    const Routing& routing() const { return _ctrl.lastRouting(); }
    std::vector<std::string> activeTools() const {
        return _loaded ? _ctrl.activeTools() : std::vector<std::string>();
    }
    const std::vector<FailedMove>& failedMoves() const { return _failed; }
    bool deadHeadRisk() const { return _deadHeadRisk; }

    // Serialize the live view into `out`, matching statusView() in
    // shared/device-model/topology-device.js field-for-field so the Live view
    // and the conformance suite see the same shape from firmware and mock:
    //   { actuators, tools, collectorOn, conflicts, reachable }
    // Plus `transitioning` and `failed`, which the mock has no analogue for
    // (it has no hardware that can refuse) — additive, so the contract holds.
    void writeStatus(JsonObject out) const {
        JsonObject actuators = out.createNestedObject("actuators");
        JsonObject tools     = out.createNestedObject("tools");
        if (!_loaded) {
            out["collectorOn"] = false;
            out.createNestedArray("conflicts");
            out.createNestedObject("reachable");
            return;
        }

        const std::map<std::string, std::string>& st = _ctrl.actuatorStates();
        for (auto& kv : st) {
            if (kv.second.empty()) actuators[kv.first] = (const char*)nullptr;
            else                   actuators[kv.first] = kv.second;
        }

        for (JsonObjectConst e : topology()["elements"].as<JsonArrayConst>()) {
            if (!_eq(e["type"], "tool")) continue;
            const char* id = e["id"].as<const char*>();
            if (!id) continue;
            float w  = _ctrl.toolWatts(id);
            JsonObject t = tools.createNestedObject(id);
            t["watts"]  = w;
            t["active"] = w >= _ctrl.toolThreshold(id);
            // So the Live view can show WHY a tool is on — a hand-thrown switch
            // reads differently from a tool the shop noticed by itself.
            if (_manual.count(id)) t["manual"] = true;
        }

        out["collectorOn"] = _collectorRunning;
        // Additive, like `transitioning`: lets the Live view say "coasting down"
        // instead of showing a blower running with every tool off, which reads
        // as a stuck relay.
        if (_coasting) out["collectorCoasting"] = true;

        const Routing& r = _ctrl.lastRouting();
        JsonArray conflicts = out.createNestedArray("conflicts");
        for (const Conflict& c : r.conflicts) {
            JsonObject o = conflicts.createNestedObject();
            o["selectorId"]  = c.selectorId;
            o["winner"]      = c.winner;
            o["winnerState"] = c.winnerState;
            JsonArray losers = o.createNestedArray("losers");
            for (const std::string& l : c.losers) losers.add(l);
        }

        JsonObject reachable = out.createNestedObject("reachable");
        for (auto& kv : r.reachable) reachable[kv.first] = kv.second;

        out["transitioning"] = transitioning();
        JsonArray failed = out.createNestedArray("failed");
        for (const FailedMove& f : _failed) {
            JsonObject o = failed.createNestedObject();
            o["selectorId"] = f.selectorId;
            o["toState"]    = f.toState;
            o["reason"]     = f.reason;
        }
    }

    JsonObjectConst selectorById(const std::string& id) const {
        if (!_doc) return JsonObjectConst();
        for (JsonObjectConst e : topology()["elements"].as<JsonArrayConst>())
            if (_eq(e["id"], id.c_str())) return e;
        return JsonObjectConst();
    }

private:
    // Adopt a fresh decision. The queue is REBUILT (not appended to): a newer
    // decision always supersedes a pending older one — most-recent-tool-wins
    // applies to the plan too. Crucially it is rebuilt from `_hwStates`, so any
    // move the previous plan hadn't executed yet is still in the new one if it's
    // still needed.
    void ingest(const ReconcileResult& r) {
        _failed.clear();
        _collectorDesired = _ctrl.collectorOn();

        if (!_collectorDesired) {
            // Idle. Policy is HOLD: leave every gate exactly where it is rather
            // than driving the shop closed (routing.states would say "all
            // closed", which is the one destination that can dead-head). Same
            // rule Controller applies to its own state adoption.
            _queue.clear();
            _deadHeadRisk = r.plan.deadHeadRisk;
            // Coast rather than cut. Only from a RUNNING blower: if it was
            // already off there's nothing to coast, and starting a timer would
            // just delay the next honest decision.
            if (_collectorRunning && !_coasting && collectorOffDelayMs() > 0) {
                _coasting     = true;
                _coastUntilMs = _nowMs + collectorOffDelayMs();
            } else if (!_coasting) {
                _collectorRunning = false;
            }
            return;
        }

        // A tool is running again — whatever we were coasting toward is moot.
        _coasting = false;

        TransitionPlan plan = planTransition(topology(), _hwStates, r.routing.states,
                                             _collectorRunning);
        _queue.assign(plan.moves.begin(), plan.moves.end());
        _deadHeadRisk = plan.deadHeadRisk;
        // A dead-head OFF is a safety stop: immediate, and it cancels a coast.
        // ON waits for the queue to drain (see update()).
        if (_deadHeadRisk) { _coasting = false; _collectorRunning = false; }
    }

    bool anyMakeFailed() const {
        for (const FailedMove& f : _failed) if (!f.isBreak) return true;
        return false;
    }

    NodeBus*                             _bus = nullptr;
    std::unique_ptr<DynamicJsonDocument> _doc;
    Controller                           _ctrl;
    std::deque<Move>                     _queue;
    std::vector<FailedMove>              _failed;
    // What has actually been COMMANDED to hardware, as opposed to what the brain
    // has decided. Diverges from Controller::actuatorStates() whenever a plan is
    // superseded mid-flight; that divergence is exactly what makes re-planning
    // from here correct. See the header note.
    std::map<std::string, std::string>   _hwStates;
    // Tools switched on by hand. Held here rather than in the controller because
    // it's a device-layer concern: the brain only knows watts.
    std::set<std::string>                _manual;
    bool _loaded           = false;
    bool _inFlight         = false;
    bool _collectorDesired = false;
    bool _collectorRunning = false;
    bool _deadHeadRisk     = false;
    // Coast-down: still energized, but only to finish spinning the ducts clear.
    bool     _coasting     = false;
    uint32_t _coastUntilMs = 0;
    uint32_t _nowMs        = 0;
};

} // namespace topo

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
//   ON is deferred until that system's moves have drained, so a blower only ever
//   starts against an already-open path. A tool starting mid-coast just keeps it
//   on. A failed MAKE in that system also holds it off: if the gate that was
//   supposed to open didn't (dead node, uncalibrated servo), running the blower
//   would pull against a closed system. A failed break only leaks suction, so it
//   doesn't block — one dead secondary shouldn't make the whole shop unusable.
//   deadHeadRisk forces OFF regardless.
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

// Synthetic wattage for a manually-switched-on machine. Above any real threshold
// anyone would set (the schema's own default is 5 W).
static const float kManualWatts = 100000.0f;

// Coast-down used when the collector element doesn't name one. Not zero on
// purpose: every shop wants some, nobody has a UI to set it yet, and the
// stop-selection path's equivalent slack is 3 s. Matches the schema doc's example.
static const uint32_t kDefaultCollectorOffDelayMs = 4000;

// A move that has been issued but whose bus rejected it (offline node, missing
// calibration). Surfaced through /api/status so the UI can say which gate.
// No default member initializers: the ESP32 toolchain builds at gnu++11, where
// that would make this a non-aggregate and break the brace-init call sites.
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
        _inFlightSystem.clear();
        _collectors.clear();
        for (const SystemView& sys : systemsOf(topology()))
            _collectors[std::string(sys.id ? sys.id : "")] = CollectorState{false, false, 0, false, false};
        _loaded = true;
        return true;
    }

    void clear() {
        _doc.reset();
        _queue.clear();
        _failed.clear();
        _manual.clear();
        _hwStates.clear();
        _collectors.clear();
        _inFlightSystem.clear();
        _loaded = false;
    }

    bool loaded() const { return _loaded; }
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
        ingest(_ctrl.setMachinePower(machineId, on ? kManualWatts : 0.0f));
        return true;
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
    void setToolPower(const std::string& id, float w) { setMachinePower(id, w); }
    bool setToolManual(const std::string& id, bool on) { return setMachineManual(id, on); }
    bool hasTool(const std::string& id) const { return hasMachine(id); }
    bool toolIsManual(const std::string& id) const { return machineIsManual(id); }
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

        // Coast expiry is checked here rather than in ingest(): at idle no power
        // events arrive, so ingest() isn't called again and nothing would ever
        // switch the blower off.
        for (auto& kv : _collectors) {
            CollectorState& c = kv.second;
            if (c.coasting && (int32_t)(nowMs - c.coastUntilMs) >= 0) {
                c.coasting = false;
                c.running  = false;
            }
        }

        if (_bus->busy()) return;          // a move is still in flight
        _inFlightSystem.clear();

        if (!_queue.empty()) {
            QueuedMove q = _queue.front();
            _queue.pop_front();
            const Move& m = q.move;
            JsonObjectConst sel = selectorById(m.selectorId);
            if (sel.isNull()) {
                _failed.push_back({q.systemId, m.selectorId, m.toState, "unknown selector", m.isBreak});
            } else if (!_bus->onlineFor(sel)) {
                _failed.push_back({q.systemId, m.selectorId, m.toState, "controller offline", m.isBreak});
            } else if (!_bus->setState(m.selectorId.c_str(), sel, m.toState.c_str())) {
                _failed.push_back({q.systemId, m.selectorId, m.toState, "actuator rejected move", m.isBreak});
            } else {
                _hwStates[m.selectorId] = m.toState;   // commanded → hardware truth
                _inFlightSystem = q.systemId;
            }
            return;                         // one move per pass, busy() gates the next
        }

        // Nothing queued and nothing moving — safe to start any blower the brain
        // wants that isn't blocked. Checked per system: a 2.5" system with a
        // pending move must not hold the 4" blower shut, and vice versa.
        for (auto& kv : _collectors) {
            CollectorState& c = kv.second;
            if (c.desired && !c.deadHeadRisk && !anyMakeFailed(kv.first)) c.running = true;
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

    // Coast-down for one system. Absent means "the shop didn't say", not "none"
    // — see kDefaultCollectorOffDelayMs. An explicit 0 does disable it.
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
    std::vector<std::string> activeTools() const { return activeMachines(); }
    const std::vector<FailedMove>& failedMoves() const { return _failed; }

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
            s["deadHeadRisk"]  = kv.second.deadHeadRisk;
            s["transitioning"] = transitioning(kv.first);
        }

        out["transitioning"] = transitioning();
        JsonArray failed = out.createNestedArray("failed");
        for (const FailedMove& f : _failed) {
            JsonObject o = failed.createNestedObject();
            o["systemId"]   = f.systemId;
            o["selectorId"] = f.selectorId;
            o["toState"]    = f.toState;
            o["reason"]     = f.reason;
        }
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

        // What each blower is doing RIGHT NOW, which is what the dead-head
        // question is asked against.
        std::map<std::string, bool> running;
        for (auto& kv : _collectors) running[kv.first] = kv.second.running;

        std::vector<SystemPlan> plans =
            planShopTransition(topology(), _hwStates, r.routing.states, running);

        for (auto& kv : _collectors) {
            const std::string& sysId = kv.first;
            CollectorState&    c     = kv.second;

            auto ait = r.systemActive.find(sysId);
            c.desired = ait != r.systemActive.end() && ait->second;

            const SystemPlan* plan = nullptr;
            for (const SystemPlan& p : plans) if (p.systemId == sysId) { plan = &p; break; }

            if (!c.desired) {
                // Idle. Policy is HOLD: leave this system's gates exactly where
                // they are rather than driving it closed (routing.states would say
                // "all closed", which is the one destination that can dead-head).
                // Its moves are simply not queued — see the queue rebuild below.
                c.deadHeadRisk = plan && plan->deadHeadRisk;
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

    bool anyMakeFailed(const std::string& systemId) const {
        for (const FailedMove& f : _failed)
            if (!f.isBreak && f.systemId == systemId) return true;
        return false;
    }

    NodeBus*                             _bus = nullptr;
    std::unique_ptr<DynamicJsonDocument> _doc;
    Controller                           _ctrl;
    std::deque<QueuedMove>               _queue;
    std::vector<FailedMove>              _failed;
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
    bool     _loaded = false;
    uint32_t _nowMs  = 0;
};

} // namespace topo

// =============================================================================
// ActuatorBus.h — the seam between the routing brain and whatever moves a gate.
//
// TopologyController decides WHERE every selector should be; TopologySequencer
// decides in WHAT ORDER. Neither knows — and must never learn — whether a given
// selector is a servo on this board's PWM bank, this board's stepper, or a servo
// on another ESP32 across the shop. That knowledge lives entirely behind this
// one interface, which is why adding secondary nodes is a second implementation
// rather than a second code path:
//
//   LocalActuatorBus   → g_servos[] / the TMC2209 on this board
//   RemoteActuatorBus  → a NodeLink WebSocket to a secondary board
//   NodeBus            → dispatches to one of the above by selector.controllerId
//
// `busy()` is load-bearing, not informational. The power budget assumes
// only ONE servo is ever driven at a time (RFC §7) — four at once browns out the
// 5V rail and resets the board mid-actuation. The runtime never issues a move
// while busy() is true, so a bus that reports busy() correctly gets the current
// mutex for free.
//
// PURE — ArduinoJson only, NO Arduino.h, so it host-compiles alongside
// TopologyRouter.h for the conformance tests.
// =============================================================================

#pragma once
#include <ArduinoJson.h>

namespace topo {

// One clamp's reported state, as a board would render it.
//
// A STRUCT RATHER THAN OUT-PARAMETERS because senseAt() had six of them and the
// amps telemetry added on 2026-09-17 would have made ten. Adding a field is then
// a one-line change here instead of an edit to four matching signatures, one of
// which would eventually be missed.
//
// `id` POINTS INTO THE BUS'S OWN STORAGE and is valid only until that bus is
// next reconfigured. Every caller today renders it immediately, which is the
// only use this is for; copy it if you ever need to keep one.
//
// NEGATIVE MEANS ABSENT for every float here, and that is load-bearing: 0 A is a
// real reading from an idle tool, while "this board has not learnt a floor" is
// not a reading at all. Zeroing them would make a faulted board and a quiet one
// look identical, which is exactly the lie `reported` exists to prevent.
//
// NOTHING MAY BRANCH ON THE TELEMETRY. `on` is the decision; amps, floor, trip
// and level are for a person to read. See shared/device-model/nodelink.js.
struct SenseView {
    const char* id        = "";
    // Has this clamp EVER spoken? The load-bearing field, and not `on` — see
    // SenseReport.h. A configured clamp that has never reported means the chain
    // is broken; "off" means the tool is idle, and they must not look alike.
    bool        reported  = false;
    bool        on        = false;
    uint32_t    ageMs     = 0;
    float       level     = -1.0f;   // multiple of the trip point
    float       amps      = -1.0f;   // what it reads now
    float       floorA    = -1.0f;   // the board's learnt noise floor
    float       tripA     = -1.0f;   // the point `amps` is judged against
    // The board refused to learn a floor: the clamp reads far too much for a
    // board at rest. floorA and tripA are absent when this is set, because there
    // is no floor. See kMaxFloorCounts in sensing/CtTrip.h.
    bool        fault     = false;
};

class ActuatorBus {
public:
    virtual ~ActuatorBus() {}

    // Is this bus usable right now? False for a remote node whose link is down,
    // or a local bus whose hardware failed to init. Selectors on an offline bus
    // are reported unreachable rather than having their moves silently dropped.
    virtual bool online() const = 0;

    // Is a move in flight? See the current-mutex note above.
    virtual bool busy() const = 0;

    // Begin moving `selectorId` to `stateId`. `sel` is the selector's element
    // object from the topology — the bus reads whatever realization it needs
    // from it (servo.channel + offsetDeg, or states[].positionMm). Returns false
    // if the move could not be started (offline, unknown channel, uncalibrated).
    // Non-blocking: completion is observed through busy().
    virtual bool setState(const char* selectorId, JsonObjectConst sel, const char* stateId) = 0;

    // Pump any deferred work. Called every main-loop pass.
    virtual void update() {}

    // ── SENSING, which most buses cannot do ────────────────────────────────
    //
    // A bus moves things. A board at the far end of one may ALSO watch a CT
    // clamp (tool-sensing RFC §5.6), and the socket is already there — so
    // sensing rides this seam rather than earning a second one, exactly as the
    // comment at the top argues for actuation.
    //
    // Both default to "this bus senses nothing", so LocalActuatorBus, the
    // stubs, and every future implementation are unaffected. Only
    // RemoteActuatorBus overrides them.

    // Tell the board what the LAYOUT says is wired to it. `sensors` is the WHOLE
    // list, as CONFIG.sensors in nodelink.js — an empty array means "report
    // nothing", which is the same state as never having been configured.
    //
    // JsonArrayConst rather than a struct so this header keeps its one include.
    virtual void configureSensors(JsonArrayConst sensors) { (void)sensors; }

    // The latest reading for a sensor on this bus.
    //
    // Returns FALSE when nothing has ever reported — which is not the same as
    // reporting "off", and the caller must not conflate them. `atMs` is when the
    // reading arrived, for the staleness check: a board that is still answering
    // PINGs but has stopped reporting is a FAULT, where a board that has gone
    // away entirely is the planer switched off at the wall (RFC §5.6a).
    // --- Setup-time jog ----------------------------------------------------
    //
    // Drive one channel to an absolute angle, outside any routing decision, so
    // the gate configurator can calibrate a valve wherever it lives. `detach`
    // de-energises instead of moving.
    //
    // ON THE SEAM SINCE 2026-09-17, and it belongs here for the reason
    // busForController() already gives about sensors: a lookup written a second
    // time is how one path works on a board whose other path does not. The jog
    // WAS that second copy — the sketch matched a controllerId against
    // RemoteActuatorBus::nodeId() with a literal ==, while every gate move went
    // through NodeBus's alias map and bareHost() normalisation. A layout whose
    // controllerId is not spelled exactly like the paired host therefore routed
    // gates perfectly and dropped every jog, which reads as "the configurator
    // is broken on nodes" and is impossible to guess at from the symptom.
    //
    // Default is a refusal rather than a no-op: a bus that cannot jog should say
    // so, because "nothing moved" is the one answer a calibration screen must
    // not silently accept.
    virtual bool jog(int channel, int angle, bool detach) {
        (void)channel; (void)angle; (void)detach; return false;
    }

    virtual bool senseOf(const char* sensorId, bool& on, uint32_t& atMs) const {
        (void)sensorId; (void)on; (void)atMs; return false;
    }
};

} // namespace topo

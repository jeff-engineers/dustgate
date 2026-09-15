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
    virtual bool senseOf(const char* sensorId, bool& on, uint32_t& atMs) const {
        (void)sensorId; (void)on; (void)atMs; return false;
    }
};

} // namespace topo

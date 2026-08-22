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
};

} // namespace topo

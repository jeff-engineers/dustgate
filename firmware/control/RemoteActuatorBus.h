// =============================================================================
// RemoteActuatorBus.h — an ActuatorBus backed by a secondary board over NodeLink.
//
// Same interface LocalActuatorBus implements, so NodeBus (and everything above
// it) can't tell the difference between a servo on this board's PWM bank and one
// across the shop. The only thing that changes is where the move lands.
//
// THREADING — this is the fourth task touching shared state, so the rules are
// strict. WebSocketsClient is NOT thread-safe and its loop() must be pumped
// often, so it lives on its OWN FreeRTOS task (Core 0, alongside the Shelly
// poller). The main loop never touches the socket:
//
//   main loop  →  setState()  →  _tx slot (under _mutex)  →  WS task sends
//   WS task    →  RX frame    →  _rx state (under _mutex) →  main loop reads
//
// LINK LOSS — online() goes false when the socket drops OR a PONG goes overdue.
// NodeBus then reports that node's selectors as un-commandable, TopologyRuntime
// records the failed move, and (because a failed *make* holds the blower off)
// the collector never starts against a path a dead board was supposed to open.
// The secondary independently holds its servos where they are; neither end ever
// slams a gate because the link went away.
//
// busy() is true from the moment a SET is handed over until the secondary
// reports STATE(moving=false) — or kMoveTimeoutMs elapses. The timeout is not
// optional: without it a dropped STATE frame would wedge the move queue forever.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "../config.h"
#include "ActuatorBus.h"
#include "NodeLink.h"
#include <WebSocketsClient.h>

namespace topo {

class RemoteActuatorBus : public ActuatorBus {
public:
    // `nodeId` is the controllerId from the topology; `host` is link.host
    // (mDNS name or IP, resolved by the WS client).
    void begin(const char* nodeId, const char* primaryId, const char* host, uint16_t port = 80);

    // Tear the link down (topology re-upload removed or re-pointed this node).
    void end();

    const char* nodeId() const { return _nodeId; }
    const char* host()   const { return _host; }

    // --- claim (RFC §8 applied to boards) ---------------------------------
    // A node belongs to ONE primary. If it refused us, it stays OFFLINE and
    // names its owner: refusing to command a board someone else owns is the
    // whole point, and "offline" is already the state every caller handles.
    //
    // refusedBy() is "" until a WELCOME actually refuses us, so a node that is
    // merely unreachable never reads as someone else's property.
    const char* refusedBy() const { return _refusedBy; }
    bool wasRefused() const       { return _refusedBy[0] != '\0'; }

    // Ask for the node even though another primary owns it. USER-CONFIRMED
    // ONLY — never call this on a refusal, or the claim degrades into "whoever
    // asks twice". Takes effect on the next connect and is cleared once used.
    void requestTakeover() { _takeover = true; }

    // --- ActuatorBus ------------------------------------------------------
    bool online() const override;
    bool busy()   const override;
    bool setState(const char* selectorId, JsonObjectConst sel, const char* stateId) override;
    void update() override {}   // all pumping happens on the WS task

    // --- Setup-time jog ---------------------------------------------------
    // Drive one channel to an absolute angle, outside any routing decision, so
    // the gate configurator can calibrate a valve that lives on this node. The
    // wire frame is an ordinary SET: a secondary's whole job is "channel +
    // angle", which is exactly what a jog is, so no new frame type is needed
    // and a node built before this existed still understands it.
    //
    // The selectorId/stateId are synthetic — the node echoes them back in its
    // STATE report and nothing here cares, but they must be present or
    // parseSetFrame() refuses the frame.
    //
    // There is no remote counterpart to a local detach: SET carries no such
    // field, and it doesn't need one. holdAtRest is false here, so the node's
    // ServoActuator de-energizes on its own once the sweep settles — which is
    // the behaviour the local detach call was asking for anyway.
    bool jog(int channel, int angle, bool detach) override;

    // --- Reporting (for GET /api/nodes) --------------------------------
    struct NodeInfo {
        bool     connected;
        uint32_t lastSeenMs;    // millis() of the last frame from this node
        char     board[24];
        char     fw[24];
        int      capServos;
        int      capLinear;
        int      capClamps;   // caps.ct — 0 for every board flashed before 2026-09-15
    };
    NodeInfo info() const;

    // ── sensing (tool-sensing RFC §5.6) ────────────────────────────────────
    // Tell the node what the layout says is wired to it, and read back what it
    // has reported. See ActuatorBus.h for the contract.
    void configureSensors(JsonArrayConst sensors) override;
    bool senseOf(const char* sensorId, bool& on, uint32_t& atMs) const override;

    // Enumerate what this node has reported, for GET /api/nodes. senseOf() asks
    // about one KNOWN id; this is for a screen that has to show whatever turned
    // up, including a clamp the layout does not mention.
    /** The address this node last answered on, or "" if it never has. Read by
     *  the sketch so a resolved address can outlive a power cut — mDNS being
     *  quiet at boot is the common case, not the exception. */
    const char* lastIp() const { return _lastIp; }
    /** Seed the fallback from storage, before the first resolve. */
    void setLastIp(const char* ip) { if (ip && *ip) nodelink::strlcpy_(_lastIp, ip, sizeof(_lastIp)); }

    size_t senseCount() const;
    bool   senseAt(size_t i, SenseView& v) const;

private:
    static void taskTrampoline(void* arg) { static_cast<RemoteActuatorBus*>(arg)->taskLoop(); }
    void taskLoop();
    void onEvent(WStype_t type, uint8_t* payload, size_t len);
    void handleFrame(const char* json, size_t len);
    void sendJson(const JsonDocument& doc);

    WebSocketsClient _ws;
    SemaphoreHandle_t _mutex   = nullptr;
    TaskHandle_t      _task    = nullptr;
    volatile bool     _running = false;

    // Resolve a bare/.local host to an IP via ESP-IDF mDNS, and (re)point the
    // socket at it. Returns false when the name doesn't answer.
    bool resolveAndDial();

    char     _nodeId[40]    = "";
    char     _primaryId[40] = "";
    char     _host[64]      = "";   // as configured: bare name, .local, or an IP
    char     _dialing[64]   = "";   // what the socket is actually pointed at
    bool     _hostIsIp      = false;
    uint32_t _lastResolveMs = 0;
    uint16_t _port          = 80;

    // --- shared state (guarded by _mutex) ---------------------------------
    bool     _connected    = false;   // socket up AND WELCOME received
    uint32_t _lastRxMs     = 0;       // any frame; drives the PONG timeout
    uint32_t _seq          = 0;
    bool     _moveOutstanding = false;
    uint32_t _moveStartedMs   = 0;
    char     _txFrame[320]    = "";   // one pending SET, main loop → WS task
    bool     _txPending       = false;
    // Set from a WELCOME carrying accepted:false. Read by the API/UI so the user
    // can be told WHO has the board before being offered a takeover.
    char     _refusedBy[40] = "";
    bool     _takeover      = false;   // one-shot, user-confirmed
    // ── sensors ────────────────────────────────────────────────────────────
    //
    // The CONFIG is CACHED, not just sent, and that is the load-bearing part:
    // the board this was built for is powered from the tool it watches (RFC
    // §5.6a), so it reboots every time someone switches the planer off at the
    // wall. A configuration sent once at adopt would be forgotten on the first
    // power cut and the tool would go quiet forever. Re-sent on every accepted
    // WELCOME instead, which costs one small frame per reconnect.
    char     _cfgFrame[512] = "";
    bool     _cfgPending    = false;
    bool     _cfgValid      = false;   // have we ever been given one?

    struct SenseState {
        char     sensorId[nodelink::kMaxSensorIdLen] = "";
        bool     on           = false;
        uint32_t atMs         = 0;
        // Multiple of the node's trip point, straight off the wire. DIAGNOSTIC
        // ONLY — nothing routes on it — and kept because it is the one number
        // that answers "is this clamp nearly tripping, or nowhere near?" while
        // the trip constants are still provisional (sensing/CtTrip.h).
        float    level        = -1.0f;
        // Telemetry for a human, straight off the wire, in AMPS. Negative =
        // the node omitted it. NOTHING BRANCHES ON THESE — see nodelink.js.
        float    amps         = -1.0f;
        float    floorA       = -1.0f;
        float    tripA        = -1.0f;
        bool     fault        = false;   // the node could not learn a floor
    };
    SenseState _senses[nodelink::kMaxSensorsPerNode];
    size_t     _senseCount = 0;

    // The address this node last actually answered on, and a stable per-host
    // offset so N tasks do not re-resolve on the same tick. Both derived, never
    // configured — see resolveAndDial().
    char     _lastIp[20]   = "";
    uint32_t _hostHash     = 0;

    char     _board[24]    = "";
    char     _fw[24]       = "";
    int      _capServos    = 0;
    int      _capLinear    = 0;
    int      _capClamps    = 0;   // caps.ct — how many CTs this board says it has
};

} // namespace topo

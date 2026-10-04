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
#include <ESPAsyncWebServer.h>   // the socket a node-initiated link arrives on

namespace topo {

// The library, with ONE thing made visible: when it last failed to connect.
//
// A failed TCP connect fires NO event — connectFailedCb() only logs — so an
// onEvent() handler cannot see it, and the backoff that hung off
// WStype_DISCONNECTED never engaged for the one case it most needed: a node that
// is switched off, dialled every second forever with a blocking connect each
// time. _lastConnectionFail is protected rather than private, so a subclass can
// read it without patching the library.
class NodeLinkClient : public WebSocketsClient {
public:
    unsigned long lastConnectFailMs() const { return _lastConnectionFail; }
};

class RemoteActuatorBus : public ActuatorBus {
public:
    // `nodeId` is the controllerId from the topology; `host` is link.host
    // (mDNS name or IP, resolved by the WS client).
    void begin(const char* nodeId, const char* primaryId, const char* host, uint16_t port = 80);

    // Wait this long before the first dial. Call BEFORE begin(). Starting every link
    // task together at boot made each one resolve, connect and allocate at once —
    // the internal heap fell to 3.7 KB with four nodes (2026-10-03). Spread over a
    // few seconds the same work peaks far lower, and no node waits long.
    void setStartDelay(uint32_t ms) { _startDelayMs = ms; }

    // At BOOT every node is assumed to dial in until proven otherwise: for this long no
    // dial-out task is started, so a node that dials in (almost all of them, within a few
    // seconds of the primary coming up) never costs a task at all, and only a node that has
    // not appeared by then is dialled the old way. Call BEFORE begin(). Not used when the
    // user pairs a board — an unowned node does not dial anyone, so that dial is immediate.
    void setBootGrace(uint32_t ms) { _bootGraceUntilMs = millis() + ms; }

    // Tear the link down (topology re-upload removed or re-pointed this node).
    // Waits for the link task to leave ON ITS OWN — see the definition for why
    // it is never deleted from outside.
    void end();

    // end() in two halves, so a caller tearing down N links pays for the
    // slowest one rather than the sum: ask every task to stop, then end() each.
    // requestStop() alone is harmless — end() still has to follow.
    void requestStop() { _running = false; }

    // Does this slot hold a link that is running (or starting)? A pool slot is reused
    // when a node is unpaired and another paired, so an old host name in `_host` says
    // nothing — this is the question. false before begin() and after end().
    bool live() const { return _running; }

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

    // --- node-initiated links (2026-10-04) ----------------------------------
    // The node dialled US (a JOIN on the primary's /nodelink listener). Bind that
    // socket as this bus's transport and let the legacy dial stand down. False
    // when a link is already up and fresh — the node is refused as a DUPLICATE
    // rather than allowed to replace a healthy one. On true, `helloOut` holds the
    // HELLO to send: it carries our claim, built the way the dial-out path builds it.
    bool attachInbound(AsyncWebSocketClient* c, String& helloOut);
    // The socket closed. A no-op if it was not this bus's.
    void detachInbound(uint32_t clientId);
    bool ownsInbound(uint32_t clientId) const { return _inId != 0 && _inId == clientId; }
    // A frame from the node on that socket — the same frames the dial-out path
    // receives, handled by the same code.
    void onInboundFrame(const char* json, size_t len) { handleFrame(json, len); }
    // The node answered our WebSocket ping: liveness, as PONG is on the dial-out path.
    void onInboundPong();
    // Does this node say it dials in (WELCOME caps.join)? Read by the sketch to
    // decide whether the dial-out path still has a job.
    bool dialsIn() const { return _capJoin > 0; }
    bool inboundUp() const { return _inId != 0; }
    // Does this bus have a dial-out task running right now? A node that dials in does
    // not need one while it is linked — see update().
    bool hasTask() const { return _taskAlive; }

    // --- ActuatorBus ------------------------------------------------------
    bool online() const override;
    bool busy()   const override;
    bool setState(const char* selectorId, JsonObjectConst sel, const char* stateId) override;
    // Dial-out links are pumped on their own task; a node-initiated link has no task,
    // so its frames go out from here, on the main loop, and its ping with them.
    void update() override;

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
        // An update this node was told to run (OTA / OTASTATE): "" when none,
        // else start | progress | done | fail. `otaPct` is -1 until reported.
        char     ota[9];
        int      otaPct;
        char     otaErr[65];
    };
    NodeInfo info() const;

    // Tell this node to pull and install an image from the primary. Called from
    // the main loop; the frame itself goes out on the link task like CONFIG.
    // Returns false (and says why through `why`) when the node is not linked or
    // is already updating. The node does its own refusing for everything it
    // alone can know (a gate moving, no second slot) and answers in OTASTATE.
    bool requestOta(const char* path, uint32_t size, const char* md5, const char* fw,
                    const char*& why);
    // Record a refusal made on the PRIMARY's side (a tool running, no image
    // staged) so the Boards screen has one place to read "why not" from.
    void noteOtaRefused(const char* why);

    // ── sensing (tool-sensing RFC §5.6) ────────────────────────────────────
    // Tell the node what the layout says is wired to it, and read back what it
    // has reported. See ActuatorBus.h for the contract.
    void configureSensors(JsonArrayConst sensors) override;
    bool senseOf(const char* sensorId, bool& on, uint32_t& atMs) const override;
    bool pollsPlugs() const override { return _capPlugs > 0; }
    bool plugReading(const char* sensorId, float& watts, bool& fault, uint32_t& atMs) const override;

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

    // --- link health, for the WiFi rejoin (firmware.ino) -------------------
    //
    // WHY THIS EXISTS (2026-09-27). Twice in one evening the primary lost a node
    // that was powered, advertising and answering a laptop in 10 ms, while the
    // primary's own TCP to it timed out (`probe`: NO ROUTE). Only rejoining WiFi
    // — a RESET — brought it back; the guest network had stopped forwarding
    // between the two boards. The primary can do that rejoin itself, but ONLY
    // when the node is evidently alive: a node powered from its tool goes dark
    // every time the tool is switched off at the wall (RFC §5.6a), and rejoining
    // WiFi for that would be pointless churn. Two independent signs of life:
    //
    //   mdnsAgeMs   — its name answered an mDNS query this long ago
    //                 (UINT32_MAX = never since this link began)
    //   hollowDrops — sockets that OPENED and then died before the WebSocket
    //                 upgrade, since the last good WELCOME. A switched-off node
    //                 makes the connect FAIL, which is silent; something that
    //                 accepts the SYN and then never answers is the network
    //                 standing in for a node that is there. Both incidents showed
    //                 exactly this: "Link lost" repeating on the primary, nothing
    //                 at all on the node.
    struct LinkHealth {
        bool     linked;        // accepted WELCOME, link up
        bool     refused;       // the node said no — a claim question, not a path one
        uint32_t downForMs;     // 0 while linked
        uint32_t mdnsAgeMs;
        uint16_t hollowDrops;
        // Why the LAST move did not finish cleanly, or nullptr. Cleared by the next
        // STATE(moving=false). Surfaced as a problem in the app (raiseDeviceProblems).
        const char* moveFault;
    };
    LinkHealth health() const;

private:
    static void taskTrampoline(void* arg) { static_cast<RemoteActuatorBus*>(arg)->taskLoop(); }
    void taskLoop();
    // Start the dial-out task if there is none. A pool slot costs a ~5 KB stack while its
    // task lives, so a node that dials in is given NO task while it is linked, and one is
    // started only when the link is down and its grace has run out.
    bool ensureTask();
    uint32_t      _bootGraceUntilMs = 0;
    volatile bool _retire    = false;   // set by the task itself when an inbound link has taken over
    uint32_t      _inSinceMs = 0;       // when the inbound link attached
    void onEvent(WStype_t type, uint8_t* payload, size_t len);
    void handleFrame(const char* json, size_t len);
    void sendJson(const JsonDocument& doc);
    // The "socket went away" bookkeeping, shared by both transports.
    void markDown(bool wasUp);
    // Build the HELLO (our claim) — one-shot takeover consumed here.
    void buildHelloString(String& out);
    // Write any pending SET / CONFIG / OTA to the INBOUND socket. Main loop only.
    void pumpInbound();
    // Grow the reconnect interval toward kReconnectMaxMs. See the definition —
    // its absence is what let a retry storm exhaust a node's WebSocket slots.
    void _backoff();

    NodeLinkClient    _ws;
    SemaphoreHandle_t _mutex   = nullptr;
    TaskHandle_t      _task    = nullptr;
    volatile bool     _running = false;
    // True from task start until the task's LAST statement. end() waits on this
    // rather than on _task, which xTaskCreate writes through a pointer and so
    // cannot be volatile.
    volatile bool     _taskAlive = false;

    // Resolve a bare/.local host to an IP via ESP-IDF mDNS, and (re)point the
    // socket at it. Returns false when the name doesn't answer. Link task only.
    bool resolveAndDial();
    // Point the socket at `target` — and do NOTHING if it already is. See the
    // definition: WebSocketsClient::begin() on a live socket leaks it.
    void dialTo(const char* target);
    bool _dialedOnce = false;

    // Socket state as the LIBRARY sees it: up from WStype_CONNECTED to
    // WStype_DISCONNECTED, whether or not a WELCOME has been accepted. The
    // re-resolve must not run while this is true — a refused or not-yet-welcomed
    // link is still an open socket, and re-pointing it orphans the connection.
    // Touched only on the link task.
    bool _sockUp = false;
    // The last _lastConnectionFail the task has acted on, so each failure backs
    // off exactly once.
    unsigned long _seenFailMs = 0;

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
    uint32_t _startDelayMs    = 0;
    const char* _moveFault    = nullptr;   // static string; see LinkHealth::moveFault
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
    char     _cfgFrame[768] = "";   // 4 plug specs are ~120 B each, plus the header
    bool     _cfgPending    = false;
    // OTA order, sent by the link task. See requestOta().
    bool     _otaPending    = false;
    char     _otaFrame[256] = "";
    uint32_t _otaSeq        = 0;
    char     _otaState[9]   = "";
    int      _otaPct        = -1;
    char     _otaErr[65]    = "";
    uint32_t _otaTouchedMs  = 0;   // last time the order or a report moved the state — a silent node is not 'updating' for ever
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
        bool     fault        = false;   // the node could not learn a floor (a plug: did not answer)
        bool     isPlug       = false;   // reported by a plug sensor, not a clamp
        float    watts        = -1.0f;   // a plug's reading; negative = none
    };
    SenseState _senses[nodelink::kMaxSensorsPerNode];
    size_t     _senseCount = 0;
    // Current reconnect interval, between kReconnectMinMs and kReconnectMaxMs.
    unsigned long _retryMs = nodelink::kReconnectMinMs;

    // The address this node last actually answered on, and a stable per-host
    // offset so N tasks do not re-resolve on the same tick. Both derived, never
    // configured — see resolveAndDial().
    char     _lastIp[20]   = "";
    uint32_t _hostHash     = 0;

    // Link health (see LinkHealth). Written on the link task, read by the main
    // loop through health() under _mutex — except _lastMdnsOkMs, a lone aligned
    // word written in one place.
    uint32_t          _downSinceMs  = 0;
    volatile uint32_t _lastMdnsOkMs = 0;   // 0 = never
    uint16_t          _hollowDrops  = 0;
    bool              _everLinked   = false;   // first link_up of this begin() is marked "first"

    char     _board[24]    = "";
    char     _fw[24]       = "";
    int      _capServos    = 0;
    int      _capLinear    = 0;
    int      _capClamps    = 0;   // caps.ct — how many CTs this board says it has
    int      _capPlugs     = 0;   // caps.plug — 1 if it polls plugs for us; absent = 0
    int      _capJoin      = 0;   // caps.join — 1 if it dials us itself; absent = 0

    // The socket a node-initiated link arrived on. Set on the async_tcp task by the
    // JOIN, read by the main loop; the id is the handle, the pointer only used while
    // the id still matches.
    AsyncWebSocketClient* volatile _inClient = nullptr;
    volatile uint32_t     _inId       = 0;
    uint32_t              _lastPingMs = 0;
};

} // namespace topo

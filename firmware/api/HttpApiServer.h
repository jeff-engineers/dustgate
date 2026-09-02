// =============================================================================
// HttpApiServer.h — REST + WebSocket API server
//
// Runs alongside any control mode (CONTROL_SERIAL_DEBUG, CONTROL_SMART_OUTLET,
// etc.). Enabled by #define ENABLE_HTTP_API in config.h.
//
// REST endpoints:  http://<device-ip>/api/...
// WebSocket:       ws://<device-ip>/ws   (push on state change)
// Auth:            X-Api-Key: <key> header on all requests
//
// Thread safety:
//   Request handlers run on the AsyncTCP task (Core 0). They write only to
//   _pendingCmd (protected by _mutex). The main loop (Core 1) calls consume
//   methods to drain commands — no direct motor/feedback access from handlers.
//
// Status flow:
//   Main loop calls update(status) each iteration. If the serialized status
//   differs from the last push, all connected WebSocket clients are notified.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "../config.h"

#ifdef ENABLE_HTTP_API

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "../control/NodeLink.h"

// Forward declaration — outlets are optional (null if not in outlet mode)
#ifdef CONTROL_SMART_OUTLET
  class SmartOutletControl;
#endif

// ---------------------------------------------------------------------------
// Status snapshot passed from the main loop to update()
// ---------------------------------------------------------------------------
struct ApiStatus {
    const char* stateName;   // "IDLE", "HOMING", "MOVING", "AT_STOP", "ERROR", ...
    int  currentStop;        // last confirmed stop index (-1 = unknown)
    int  targetStop;
    long positionSteps;
    float positionMM;        // raw actuator position, independent of any saved stop —
                              // lets the UI show continuous movement while jogging,
                              // since currentStop/targetStop don't change during a jog
    bool homed;
    // Does this board drive a sliding gate at all? Straight from HAS_LINEAR, so
    // it is a property of the BUILD and cannot disagree with the hardware.
    //
    // The app needs it because a board's port count depends on it — four PWM
    // channels or one rack, never both — and asking the user which they flashed
    // is asking them to restate something the board already knows. Nodes report
    // the same fact in their WELCOME caps; this is the primary's copy.
    bool hasLinear;
    bool enabled;
    bool endstopHome;        // true = home switch currently triggered
    bool endstopMax;         // true = far switch currently triggered
    int  numActiveStops;     // runtime gate count (set from g_numActiveStops)
    float measuredStepsPerMM; // calibrated steps/mm (0 = not calibrated → nominal)
    long  measuredSpanSteps;  // near→far span in steps (0 = not calibrated)
    const char* manifoldModel; // "rockler-2.5" | "rockler-4" | "custom"
};

// ---------------------------------------------------------------------------
// HttpApiServer
// ---------------------------------------------------------------------------
class HttpApiServer {
public:
    HttpApiServer();

    // Call once from setup() after WiFi is connected.
    // Loads or generates the API key, starts the server.
    bool begin();

    // Call every loop(). Pushes a WebSocket frame if status has changed.
    // Pass current system state; also pass SmartOutletControl pointer for
    // outlet data (nullptr if not in outlet mode).
    void update(const ApiStatus& status
#ifdef CONTROL_SMART_OUTLET
                , SmartOutletControl* outlets = nullptr
#endif
    );

    // ------------------------------------------------------------------
    // Pending command consumers — call these from the main loop in order
    // of priority. Each returns true once, then clears the flag.
    // ------------------------------------------------------------------
    bool consumeEStopRequest();
    bool consumeHomeRequest();
    bool consumeMoveRequest(int& outStop);      // outStop: 0 = home, 1-N = gate
    bool consumeJogRequest(float& outMM);      // outMM: + = away from home
    bool consumeClearCalRequest();
    // Save current motor position as a numbered stop (used during setup).
    // Main loop must read current position and write it to g_stopPositionsMM.
    bool consumeSetStopRequest(int& outIndex); // outIndex: 1-N


    // Active gate count (runtime; bounded by compile-time NUM_STOPS).
    // Written to NVS by the handler; consumed by main loop to update g_numActiveStops.
    bool consumeSetNumGatesRequest(int& outN);

    // Reference-sweep calibration request (POST /api/calibrate). The main loop
    // runs the sweep + placement and persists to CalibrationData.
    bool consumeCalibrateRequest(char* outModel, size_t modelLen, int& outGateCount);

    // Port-role change (POST /api/config/port-role). outRole is a PortRole value.
    bool consumePortRoleRequest(int& outIndex, int& outRole);

    // Servo jog (POST /api/servo/jog). Setup-only: the gate configurator drives a
    // servo directly so the user can watch the valve and capture where it lands. One
    // pending slot — a jog is a single discrete nudge, and a newer one supersedes an
    // unread older one rather than queueing. outDetach = de-energize instead of move.
    // outController is the topology controllerId the jog is addressed to, or ""
    // for this board. A gate on a secondary must be jogged on the board that
    // actually drives it — without this the configurator's arrows moved the
    // PRIMARY's servo on the same channel number, so assigning a gate to a node
    // looked like it did nothing (or worse, twitched an unrelated valve).
    bool consumeServoJogRequest(int& outChannel, int& outAngle, bool& outDetach,
                                String& outController);

    // True once after PUT /api/topology stores a new document. The main loop
    // re-adopts it into the TopologyRuntime — the handler can't, since parsing
    // and swapping the live topology must not happen under the AsyncTCP task
    // while the loop is mid-transition.
    bool consumeTopologyChanged();

    // Live routing status, published by the main loop each pass and served verbatim
    // by GET /api/status. Same reason as _lastStatusJson: the routing state
    // lives in main-loop-owned std::maps, and an async handler must never walk
    // them while they're being mutated. Empty string = no topology loaded (404).
    void publishTopologyStatus(const String& json);

    // ------------------------------------------------------------------
    // NodeLink (ws://<us>/nodelink) — the SECONDARY side of the star. A primary
    // dials in and sends already-resolved SET frames; this board just moves the
    // channel to the number it was given. See control/NodeLink.h.
    //
    // Every board exposes this, not just servo-only builds: it's what lets a
    // second DevKitC act as an actuator bank on the bench without a special
    // firmware, and it costs one WS route when nobody connects.
    // ------------------------------------------------------------------

    // Pending SET from a linked primary — one slot, since the primary serializes
    // moves anyway (a newer SET supersedes an unread one rather than queueing).
    bool consumeNodeSet(topo::nodelink::SetCommand& out);

    // Report progress back to the primary. Called from the main loop once the
    // actuator has been commanded (moving=true) and again when it settles.
    void reportNodeState(const char* selectorId, const char* stateId, bool moving);

    // True while some primary holds a NodeLink connection to this board. The
    // main loop uses it to know it's acting as a secondary right now.
    bool nodeLinkConnected() const { return _nodeLinkClients > 0; }

    // ------------------------------------------------------------------
    // Node discovery + link state (the primary side of Stage 4)
    // ------------------------------------------------------------------

    // GET /api/nodes/discover — true once per request. Like the outlet
    // discover route, the mDNS sweep MUST run on the main loop task (see that
    // route's comment), so the loop calls this, does the scan, then calls
    // respondNodeDiscover() with the built JSON.
    bool consumeNodeDiscoverRequest();
    void respondNodeDiscover(const String& json);

    // Live per-controller link state, published by the main loop and served
    // verbatim by GET /api/nodes. Cached for the same reason as the routing status:
    // the link objects are main-loop/WS-task owned, not async-handler owned.
    void publishNodeStatus(const String& json);

    // True once after POST /api/nodes/pair. The main loop owns the registry
    // write and the redial — see the route for why.
    // outTakeover = the user confirmed taking this node from another primary.
    bool consumeNodePairRequest(String& outHost, String& outName, bool& outRemove,
                                bool& outTakeover);

    // Manual tool switch from the Live view (POST /api/tool). Consumed on the
    // main loop, which owns the routing brain.
    bool consumeToolManualRequest(String& outToolId, bool& outOn);
    // POST /api/collector — run ONE system's blower by hand. Empty systemId means
    // "the first system", which is what a single-blower shop always meant.
    bool consumeCollectorManualRequest(String& outSystemId, bool& outOn);

    // Home-side answer (POST /api/config/orientation {homedLeft}). Consumed by the
    // main loop, which ensures the home datum is the user's left endstop (re-homing
    // if it came up on the right). outHomedLeft is the reported side.
    bool consumeOrientationRequest(bool& outHomedLeft);

    // Seconds of no move/home activity before the driver is powered off
    // (0 = never). Loaded from NVS; the main loop polls this directly each
    // iteration to decide whether to sleep — see firmware.ino.
    int idleTimeoutSec() const { return _idleTimeoutSec; }

#ifdef CONTROL_SMART_OUTLET
    // Outlet configuration commands — consumed by main loop, forwarded to
    // SmartOutletControl.
    struct OutletConfigCmd {
        int   slot;
        int   generation;
        char  ip[16];
        char  host[40];   // mDNS hostname, if known — empty for manual IP entry
        char  name[32];
        int   stopIndex;
        float thresholdW;
    };
    bool consumeOutletConfigRequest(OutletConfigCmd& out);
    bool consumeOutletDeleteRequest(int& outSlot);
    bool consumeOutletSaveRequest();

    // Dust collector plug config — consumed by main loop, forwarded to
    // SmartOutletControl.configureDustCollector() / removeDustCollector().
    struct DustCollectorCmd {
        int  generation;
        char ip[16];
        char host[40];   // mDNS hostname, if known — empty for manual IP entry
    };
    bool consumeDustCollectorConfigRequest(DustCollectorCmd& out);
    bool consumeDustCollectorDeleteRequest();
    // Manual dashboard on/off. outOn = requested state.
    bool consumeDustCollectorSwitchRequest(bool& outOn);

    // Outlet discovery — true once when GET /api/outlets/discover is received.
    // The actual mDNS query MUST run on the main loop task (see .cpp for why),
    // so the main loop calls this, does the scan itself, then calls
    // respondDiscover() with the built JSON to actually reply to the client.
    bool consumeDiscoverRequest();
    void respondDiscover(const String& json);

    // Outlet ping — like discover, the probe runs on the main loop rather than
    // a spawned task. Not for mDNS thread-safety (ping takes an IP, no mDNS),
    // but because the probe blocks for up to a couple seconds on an
    // unreachable host: holding a raw AsyncWebServerRequest* across that in a
    // detached task risked a use-after-free if the browser disconnected or
    // retried mid-probe. consumePingRequest() hands the requested IP to the
    // main loop; respondPing() replies once the probe is done.
    bool consumePingRequest(char* outIp, size_t ipLen);
    void respondPing(const String& json);

    // Plug TAKEOVER (RFC §8) — POST /api/outlets/takeover {"ip":"..."}.
    //
    // Its own endpoint, not a flag on save, and that is the point: taking a plug
    // away from another controller is a different act from pairing one, breaks
    // something on a machine the user isn't looking at, and must be impossible
    // to do by accident. Nothing else in the firmware can set the approval this
    // records — see SmartOutlet::approveTakeover().
    bool consumeTakeoverRequest(char* outIp, size_t ipLen);

    // Plug RENAME — POST /api/outlets/name {"ip":"...","label":"Table Saw"}.
    //
    // Writes the plug's own app-visible name (Switch.SetConfig), with our owner
    // suffix reattached by the main loop. Deferred and request-holding for the
    // same reason ping is: the write blocks on a device that may not answer, and
    // holding a raw request across that on a detached task is a use-after-free
    // waiting to happen. respondOutletName() replies once it has landed.
    //
    // Takes an IP rather than a slot deliberately — a plug can be renamed before
    // it is paired with anything, which is when a shelf of identical
    // "shellyplug-s-…" most needs telling apart.
    // `outTakeover` carries the user's explicit "rename it anyway" for a plug
    // another controller owns. A NAME write, and only that — it does not touch
    // the plug's push config, so nobody's automation breaks; it just stops the
    // refusal being absolute, since a person who wants this can always do it in
    // the Shelly app and we would rather it happened where there is a record.
    bool consumeOutletNameRequest(char* outIp, size_t ipLen,
                                  char* outLabel, size_t labelLen,
                                  bool& outTakeover);
    void respondOutletName(const String& json);

    // Plug RELEASE — POST /api/outlets/release {"ip":"..."}.
    //
    // The device half of unpairing: take our owner suffix off the plug's name
    // and hand its push target back. The LAYOUT half (dropping sensor.outlet) is
    // an ordinary topology write and does not come through here — which is why
    // this endpoint failing must not block an unpair. See firmware.ino.
    bool consumeOutletReleaseRequest(char* outIp, size_t ipLen);
    void respondOutletRelease(const String& json);
#endif

    // Expose the API key for the front-end / serial display
    const String& apiKey() const { return _apiKey; }

private:
    AsyncWebServer    _server;
    AsyncWebSocket    _ws;
#ifdef CONTROL_SMART_OUTLET
    // Inbound WebSocket for Gen2 plugs' Outbound-WebSocket connections. Plugs
    // are told (via Ws.SetConfig) to dial ws://<us>/shelly-rpc and stream their
    // status here; the handler routes each frame to SmartOutletControl by the
    // plug's source IP. Set _outletControl (from update()) so the async callback
    // can reach the control object.
    AsyncWebSocket        _shellyWs;
    SmartOutletControl*   _outletControl = nullptr;
#endif
    // Created in begin(). Explicitly null so that calling a consumeXxx() before
    // begin() fails deterministically rather than taking whatever a garbage
    // handle points at.
    SemaphoreHandle_t _mutex = nullptr;
    String            _apiKey;
    // Last serialised status — cached for GET /api/motion
    String            _lastStatusJson;
    // Fingerprint of trigger fields — avoids WS pushes on positionSteps jitter
    uint32_t          _lastStatusHash;
    bool              _statusHashValid;
    // Throttled position-change push — positionSteps is excluded from the
    // fingerprint above (it changes every loop during any real move and would
    // flood the socket), but that also means a raw jog — which never touches
    // currentStop/targetStop/state — produced zero WS pushes at all, leaving
    // clients with a frozen position for the whole jog. This tracks the last
    // pushed position and forces an extra push when it has moved meaningfully,
    // no more often than every POSITION_PUSH_MIN_MS.
    long              _lastPushedPositionSteps;
    unsigned long     _lastPositionPushMs;
    // A client has connected and has been told NOTHING yet.
    //
    // Status is pushed ON CHANGE ONLY — there is no heartbeat — so a client that
    // connects after the interesting change already happened hears nothing until
    // the next one. That is what left the calibration wizard's button reading
    // "Homing…" forever on 2026-08-28: the socket dropped during a home (a long
    // run of blocking waits), reconnected once the board was already IDLE and
    // homed, and no field ever changed again.
    //
    // Set from the AsyncTCP task, consumed by the main-loop push, because
    // textAll() belongs on the main loop here — the same discipline the node
    // socket's push follows.
    volatile bool     _statusPushForced = false;

    // Pending commands (written by request handlers, read by main loop)
    bool  _estopPending;
    bool  _homePending;
    bool  _movePending;    int   _moveStop;
    bool  _jogPending;     float _jogMM;
    bool  _clearCalPending;
    bool  _setStopPending;         int  _setStopIndex;
    bool  _setNumGatesPending;     int  _newNumGates;
    bool  _calibratePending;       char _calModel[16];  int _calGateCount;
    bool  _portRolePending;        int  _portRoleIndex; int _portRoleValue;
    bool  _orientationPending;     bool _orientationValue;    // POST /api/config/orientation {homedLeft}
    bool  _servoJogPending;        int  _servoJogChannel; int _servoJogAngle; bool _servoJogDetach;
    String _servoJogController;    // "" = this board; else a secondary's controllerId
    int   _cachedNumActiveStops;   // from ApiStatus.numActiveStops; returned in /api/info
    int   _idleTimeoutSec;         // persisted idle power-off timeout; see idleTimeoutSec()

    // Accumulator for the PUT /api/topology body, which (unlike every other
    // endpoint's tiny single-frame payload) can span multiple onBody chunks.
    // Single-client device, so one shared buffer is sufficient; reset on the
    // first chunk (index == 0).
    String _topoUploadBuf;

    // topology runtime hand-off (see consumeTopologyChanged / publishTopologyStatus)
    bool   _topoChangedPending = false;
    String _topoStatusJson;

    // NodeLink secondary endpoint (see consumeNodeSet / reportNodeState)
    AsyncWebSocket            _nodeWs;
    volatile int              _nodeLinkClients = 0;
    bool                      _nodeSetPending  = false;
    topo::nodelink::SetCommand _nodeSetCmd;

    // ------------------------------------------------------------------
    // Replies the MAIN LOOP produces, for work that blocks (an mDNS sweep, an
    // HTTP write to a plug). The loop owns them because doing them on the async
    // TCP task is worse; the question is how the request waits.
    //
    // It USED TO WAIT AS A POINTER: the handler stashed the
    // AsyncWebServerRequest* and returned without answering, and the loop called
    // send() on it later. That worked on me-no-dev/ESPAsyncWebServer 1.2.3 and is
    // a hard error on the ESP32Async 3.x fork the C5 rides, which answers a
    // handler that returned without responding:
    //
    //     HTTP 501  "Handler did not handle the request"
    //
    // — immediately, and then the loop's later send() is writing to a request the
    // library has already finished with. On a C5 primary that broke BOTH scans
    // and the plug rename/release, all with the same 501, while the serial log
    // showed the work completing perfectly (found 2026-08-22 from /boards).
    //
    // So the request now waits AS AN OPEN RESPONSE instead. The handler answers
    // immediately with a chunked response whose filler returns RESPONSE_TRY_AGAIN
    // until the loop fills the body in — legal on both libraries, and the request
    // is never held across a loop iteration by anything but the library itself.
    // Long enough for three 800ms mDNS attempts plus slack; short enough that a
    // loop that never answers fails the request instead of hanging the page. A
    // timed-out reply is a normal JSON error, not a dropped connection.
    static const uint32_t kDeferredTimeoutMs = 15000;

    struct Deferred {
        // volatile, and set LAST: the filler runs on the async task and reads
        // this without a lock, so the body must be complete before it turns true.
        volatile bool ready = false;
        String        body;
        uint32_t      startedMs = 0;
        // Spaces emitted while waiting — see beginDeferred() for why they exist.
        // They are part of the response, so the body's offset is index - pad.
        size_t        pad = 0;

        // True while a reply is still owed on this slot. A second request must
        // not reset it: doing so clears the body the in-flight filler is about
        // to read, and that request then waits forever.
        bool busy() const {
            return startedMs != 0 && !ready &&
                   (millis() - startedMs) < kDeferredTimeoutMs;
        }
    };
    void beginDeferred(AsyncWebServerRequest* req, Deferred& slot);
    void finishDeferred(Deferred& slot, const String& json);

    // Node discovery + link state (see consumeNodeDiscoverRequest / publishNodeStatus)
    bool                   _nodeDiscoverPending = false;
    Deferred               _nodeDiscoverReply;
    String                 _nodeStatusJson;
    bool                   _nodePairPending = false;
    String                 _nodePairHost;
    String                 _nodePairName;
    bool                   _toolManualPending = false;
    String                 _toolManualId;
    bool                   _collectorManualPending = false;
    String                 _collectorManualSystem;
    bool                   _collectorManualOn = false;
    bool                   _toolManualOn = false;
    bool                   _nodePairRemove = false;
    // User-confirmed: adopt a node that another primary owns (RFC §8 for boards).
    bool                   _nodePairTakeover = false;

#ifdef CONTROL_SMART_OUTLET
    bool            _outletConfigPending;
    OutletConfigCmd _outletConfigCmd;
    bool            _outletDeletePending;  int _outletDeleteSlot;
    bool            _outletSavePending;
    bool            _dcConfigPending;
    DustCollectorCmd _dcConfigCmd;
    bool            _dcDeletePending;
    bool            _dcSwitchPending;  bool _dcSwitchOn;
    bool            _discoverPending;
    Deferred        _discoverReply;
    bool            _pingPending;
    Deferred        _pingReply;
    char            _pingIp[40];
    // Rename / release both hold their request across a blocking device write,
    // exactly as ping does.
    bool            _outletNamePending;
    Deferred        _outletNameReply;
    char            _outletNameIp[40];
    char            _outletNameLabel[48];
    bool            _outletNameTakeover;
    bool            _outletReleasePending;
    Deferred        _outletReleaseReply;
    char            _outletReleaseIp[40];
    bool            _takeoverPending;
    char            _takeoverIp[40];
#endif

    // Helpers
    bool   loadOrGenerateKey();
    bool   checkAuth(AsyncWebServerRequest* req);
    void   sendOk(AsyncWebServerRequest* req);
    void   sendError(AsyncWebServerRequest* req, int code, const char* msg);
    String buildStatusJson(const ApiStatus& status
#ifdef CONTROL_SMART_OUTLET
                           , SmartOutletControl* outlets
#endif
    );

    // Route registration
    void registerRoutes();
};

#endif // ENABLE_HTTP_API

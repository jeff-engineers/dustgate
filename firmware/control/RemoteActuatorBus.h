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

    // --- ActuatorBus ------------------------------------------------------
    bool online() const override;
    bool busy()   const override;
    bool setState(const char* selectorId, JsonObjectConst sel, const char* stateId) override;
    void update() override {}   // all pumping happens on the WS task

    // --- Setup-time jog (NOT part of ActuatorBus) -------------------------
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
    bool jog(int channel, int angle);

    // --- Reporting (for GET /api/nodes) --------------------------------
    struct NodeInfo {
        bool     connected;
        uint32_t lastSeenMs;    // millis() of the last frame from this node
        char     board[24];
        char     fw[24];
        int      capServos;
        int      capLinear;
    };
    NodeInfo info() const;

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
    char     _board[24]    = "";
    char     _fw[24]       = "";
    int      _capServos    = 0;
    int      _capLinear    = 0;
};

} // namespace topo

// =============================================================================
// RemoteActuatorBus.cpp — see the header for the threading + link-loss contract.
// =============================================================================

#include "RemoteActuatorBus.h"
#include <ESPmDNS.h>

#ifndef DEBUG_PRINT
  #define DEBUG_PRINT(x)   Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
#endif

namespace topo {

// Small stack: this task only parses ≤320-byte frames and calls into the WS
// library. Matches the sizing style of the Shelly poll task.
static const uint32_t kNodeLinkTaskStack = 4096;
static const UBaseType_t kNodeLinkTaskPrio = 1;

void RemoteActuatorBus::begin(const char* nodeId, const char* primaryId,
                              const char* host, uint16_t port) {
    if (_running) end();

    if (!_mutex) _mutex = xSemaphoreCreateMutex();
    nodelink::strlcpy_(_nodeId,    nodeId    ? nodeId    : "", sizeof(_nodeId));
    nodelink::strlcpy_(_primaryId, primaryId ? primaryId : "", sizeof(_primaryId));
    nodelink::strlcpy_(_host,      host      ? host      : "", sizeof(_host));
    _port = port;
    if (_host[0] == '\0') {
        DEBUG_PRINT(F("[NODE] No link.host for node ")); DEBUG_PRINTLN(_nodeId);
        return;                        // nothing to dial; stays permanently offline
    }

    // Is this already a literal address? Then skip name resolution entirely.
    { IPAddress probe; _hostIsIp = probe.fromString(_host); }

    // Resolve the name OURSELVES rather than handing "<name>.local" to the socket
    // and hoping.
    //
    // WebSocketsClient resolves through lwIP's hostByName(), whose mDNS fallback
    // for ".local" names is unreliable in practice: on this bench it failed twice
    // during boot and succeeded a minute later, with the node advertising
    // perfectly the whole time and a laptop resolving it instantly. Each failure
    // costs a reconnect cycle, and at boot it means the shop comes up with its
    // gates unreachable for no visible reason.
    //
    // ESP-IDF's mDNS querier is the same machinery that already finds Shelly
    // plugs and DustGate nodes reliably, so use that and dial the resulting IP.
    // The NAME stays the source of truth (DHCP can move the board); the IP is
    // just this attempt's answer, re-resolved whenever the link is down.
    if (!resolveAndDial()) {
        // Fall back to letting the socket try the name — sometimes lwIP does
        // manage it, and a dead link retries on its own from taskLoop().
        snprintf(_dialing, sizeof(_dialing), "%s%s", _host,
                 (_hostIsIp || strchr(_host, '.')) ? "" : ".local");
        DEBUG_PRINT(F("[NODE] mDNS didn't answer for ")); DEBUG_PRINT(_host);
        DEBUG_PRINT(F(" — dialling ")); DEBUG_PRINT(_dialing);
        DEBUG_PRINTLN(F(" and will retry"));
        _ws.begin(_dialing, _port, "/nodelink");
    }
    _ws.onEvent([this](WStype_t t, uint8_t* p, size_t l) { onEvent(t, p, l); });
    // Library-level auto-reconnect handles the common case; the backoff bounds
    // come from the shared contract so the mock secondary can expect the same.
    _ws.setReconnectInterval(nodelink::kReconnectMinMs);
    _ws.enableHeartbeat(nodelink::kPingIntervalMs, nodelink::kPongTimeoutMs, 2);

    _running = true;
    // Checked, because the failure is otherwise completely silent: no task means
    // nothing ever pumps _ws.loop(), so the socket never opens and the node sits
    // at "paired but offline" forever — indistinguishable from a dead board.
    BaseType_t ok = xTaskCreatePinnedToCore(taskTrampoline, "nodelink", kNodeLinkTaskStack,
                                            this, kNodeLinkTaskPrio, &_task, 0);
    if (ok != pdPASS) {
        _running = false;
        _task    = nullptr;
        DEBUG_PRINT(F("[NODE] FAILED to start link task for ")); DEBUG_PRINT(_nodeId);
        DEBUG_PRINT(F(" — free heap ")); DEBUG_PRINTLN(ESP.getFreeHeap());
        return;
    }
    DEBUG_PRINT(F("[NODE] Linking to ")); DEBUG_PRINT(_nodeId);
    DEBUG_PRINT(F(" at ws://")); DEBUG_PRINT(_dialing); DEBUG_PRINTLN(F("/nodelink"));
}

bool RemoteActuatorBus::resolveAndDial() {
    _lastResolveMs = millis();

    if (_hostIsIp) {
        nodelink::strlcpy_(_dialing, _host, sizeof(_dialing));
        _ws.begin(_dialing, _port, "/nodelink");
        return true;
    }

    // MDNS.queryHost() wants the BARE label — ESP-IDF resolves "<label>.local"
    // internally and rejects a name that already carries the suffix.
    char label[64];
    nodelink::strlcpy_(label, _host, sizeof(label));
    size_t n = strlen(label);
    if (n > 6 && strcasecmp(label + n - 6, ".local") == 0) label[n - 6] = '\0';

    IPAddress ip = MDNS.queryHost(label, 1500);
    if (ip == IPAddress((uint32_t)0)) return false;

    String s = ip.toString();
    if (s.length() >= sizeof(_dialing)) return false;
    nodelink::strlcpy_(_dialing, s.c_str(), sizeof(_dialing));
    DEBUG_PRINT(F("[NODE] ")); DEBUG_PRINT(label);
    DEBUG_PRINT(F(" resolved to ")); DEBUG_PRINTLN(_dialing);
    _ws.begin(_dialing, _port, "/nodelink");
    return true;
}

void RemoteActuatorBus::end() {
    _running = false;
    if (_task) {
        // Let the task observe _running and exit on its own rather than
        // vTaskDelete-ing it mid-send with the socket half-written.
        for (int i = 0; i < 50 && _task; i++) delay(10);
        if (_task) { vTaskDelete(_task); _task = nullptr; }
    }
    _ws.disconnect();
    if (_mutex) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _connected = false; _moveOutstanding = false; _txPending = false;
        xSemaphoreGive(_mutex);
    }
}

void RemoteActuatorBus::taskLoop() {
    unsigned long lastNagMs = millis();
    while (_running) {
        _ws.loop();

        // Say something while a link is stuck DOWN. Every other message here fires
        // on a transition — connect, WELCOME, link lost — so a link that never
        // came up at all produced total silence, which is the one case where the
        // log most needs to distinguish "task isn't running" from "task is running
        // and the far end won't answer".
        // Re-resolve on its OWN cadence, not the nag's. A name that didn't answer
        // during boot is the common case — the querier comes up moments before
        // the first attempt — and waiting a full nag interval to try again left
        // the shop unreachable for no reason. Fast while it's fresh, backing off
        // once it's clearly not a startup race.
        if (!_connected && !_hostIsIp) {
            unsigned long since = millis() - _lastResolveMs;
            unsigned long every = (millis() < 60000UL) ? 3000UL : 15000UL;
            if (since > every) {
                char prev[64];
                nodelink::strlcpy_(prev, _dialing, sizeof(prev));
                if (resolveAndDial() && strcmp(prev, _dialing) != 0) {
                    DEBUG_PRINT(F("[NODE] Now dialling ")); DEBUG_PRINT(_dialing);
                    DEBUG_PRINT(F(" (was ")); DEBUG_PRINT(prev); DEBUG_PRINTLN(F(")"));
                }
            }
        }

        if (!_connected && (millis() - lastNagMs) > 10000) {
            lastNagMs = millis();
            DEBUG_PRINT(F("[NODE] Still dialling ")); DEBUG_PRINT(_dialing);
            DEBUG_PRINT(F(" (")); DEBUG_PRINT(_host); DEBUG_PRINT(F(")"));
            DEBUG_PRINT(F(" — heap ")); DEBUG_PRINTLN(ESP.getFreeHeap());
        }

        // Drain a pending SET. Sending from HERE (not from setState()) is what
        // keeps the socket single-threaded.
        if (_mutex && xSemaphoreTake(_mutex, 0) == pdTRUE) {
            if (_txPending && _connected) {
                _ws.sendTXT(_txFrame);
                _txPending = false;
            }
            // A move whose STATE report never arrived: give up rather than let
            // the primary's move queue block forever behind a lost frame.
            if (_moveOutstanding &&
                (millis() - _moveStartedMs) > nodelink::kMoveTimeoutMs) {
                _moveOutstanding = false;
                DEBUG_PRINT(F("[NODE] Move timed out on ")); DEBUG_PRINTLN(_nodeId);
            }
            xSemaphoreGive(_mutex);
        }
        delay(5);
    }
    _task = nullptr;
    vTaskDelete(NULL);
}

void RemoteActuatorBus::onEvent(WStype_t type, uint8_t* payload, size_t len) {
    switch (type) {
        case WStype_CONNECTED: {
            // Socket is up but the node hasn't identified itself yet — stay
            // offline until WELCOME lands so we never command an unknown board.
            StaticJsonDocument<192> doc;
            nodelink::buildHello(doc.to<JsonObject>(), _primaryId, _nodeId);
            String s; serializeJson(doc, s);
            _ws.sendTXT(s);
            break;
        }
        case WStype_DISCONNECTED:
            if (_mutex) {
                xSemaphoreTake(_mutex, portMAX_DELAY);
                _connected = false;
                // Drop any outstanding move: we can't know whether it landed,
                // and holding busy() forever would stall every other gate.
                _moveOutstanding = false;
                _txPending = false;
                xSemaphoreGive(_mutex);
            }
            DEBUG_PRINT(F("[NODE] Link lost: ")); DEBUG_PRINTLN(_nodeId);
            break;
        case WStype_TEXT:
            handleFrame(reinterpret_cast<const char*>(payload), len);
            break;

        // Heartbeat traffic counts as liveness. enableHeartbeat() above drives
        // PING/PONG at the WebSocket protocol level, so on an idle link — which
        // is the NORMAL state, since a gate only moves when a tool starts — the
        // last TEXT frame is the WELCOME at connect time.
        //
        // Without this, _lastRxMs froze there and online()'s
        // `millis() - _lastRxMs < kPongTimeoutMs` went false 6 seconds later,
        // marking a perfectly healthy node "not answering" forever. The socket
        // was fine the whole time: PONGs were arriving every 2s, they just
        // landed in `default: break;`.
        case WStype_PING:
        case WStype_PONG:
            if (_mutex) {
                xSemaphoreTake(_mutex, portMAX_DELAY);
                _lastRxMs = millis();
                xSemaphoreGive(_mutex);
            }
            break;

        default:
            break;
    }
}

void RemoteActuatorBus::handleFrame(const char* json, size_t len) {
    StaticJsonDocument<384> doc;
    if (deserializeJson(doc, json, len)) return;   // malformed → ignore
    JsonObjectConst f = doc.as<JsonObjectConst>();
    const char* t = f["t"].as<const char*>();
    if (!t) return;

    xSemaphoreTake(_mutex, portMAX_DELAY);
    _lastRxMs = millis();

    if (strcmp(t, "WELCOME") == 0) {
        // Refuse a node speaking a different protocol version rather than
        // half-understanding it. It stays offline and its gates unreachable.
        if ((f["v"] | 0) != nodelink::kVersion) {
            _connected = false;
            xSemaphoreGive(_mutex);
            DEBUG_PRINT(F("[NODE] Version mismatch from ")); DEBUG_PRINTLN(_nodeId);
            return;
        }
        nodelink::strlcpy_(_board, f["board"] | "", sizeof(_board));
        nodelink::strlcpy_(_fw,    f["fw"]    | "", sizeof(_fw));
        _capServos = f["caps"]["servos"] | 0;
        _capLinear = f["caps"]["linear"] | 0;
        _connected = true;
    } else if (strcmp(t, "ACK") == 0) {
        bool ok = f["ok"] | false;
        if (!ok) _moveOutstanding = false;                  // refused → stop waiting
        xSemaphoreGive(_mutex);
        DEBUG_PRINT(F("[NODE←] ACK seq=")); DEBUG_PRINT(f["seq"] | 0);
        DEBUG_PRINT(ok ? F(" ok") : F(" REFUSED: "));
        if (!ok) DEBUG_PRINT(f["err"] | "(no reason given)");
        DEBUG_PRINTLN();
        return;
    } else if (strcmp(t, "STATE") == 0) {
        bool moving = f["moving"] | false;
        if (!moving) _moveOutstanding = false;
        xSemaphoreGive(_mutex);
        DEBUG_PRINT(F("[NODE←] STATE ")); DEBUG_PRINT(f["selectorId"] | "?");
        DEBUG_PRINT(F(" -> ")); DEBUG_PRINT(f["stateId"] | "?");
        DEBUG_PRINTLN(moving ? F(" (moving)") : F(" (arrived)"));
        return;
    }
    xSemaphoreGive(_mutex);
}

bool RemoteActuatorBus::online() const {
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool up = _connected && (millis() - _lastRxMs) < nodelink::kPongTimeoutMs;
    xSemaphoreGive(_mutex);
    return up;
}

bool RemoteActuatorBus::busy() const {
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool b = _moveOutstanding || _txPending;
    xSemaphoreGive(_mutex);
    return b;
}

bool RemoteActuatorBus::setState(const char* selectorId, JsonObjectConst sel,
                                 const char* stateId) {
    if (!online()) return false;

    // Resolve to a concrete angle / mm HERE, on the primary. The secondary gets
    // a number, never a state name it would have to interpret — see NodeLink.h.
    StaticJsonDocument<320> doc;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    uint32_t seq = ++_seq;
    xSemaphoreGive(_mutex);

    if (!nodelink::buildSetFrame(doc.to<JsonObject>(), seq, selectorId, sel, stateId)) {
        return false;   // uncalibrated — refuse rather than send a guess
    }

    String s; serializeJson(doc, s);
    if (s.length() >= sizeof(_txFrame)) return false;

    xSemaphoreTake(_mutex, portMAX_DELAY);
    nodelink::strlcpy_(_txFrame, s.c_str(), sizeof(_txFrame));
    _txPending       = true;
    _moveOutstanding = true;
    _moveStartedMs   = millis();
    xSemaphoreGive(_mutex);

    // The whole frame, not a summary. When a gate doesn't move, the question is
    // always "which of us dropped it" — this line and the node's matching one
    // answer it in one comparison.
    DEBUG_PRINT(F("[NODE→] ")); DEBUG_PRINT(_nodeId);
    DEBUG_PRINT(F(" ")); DEBUG_PRINTLN(s);
    return true;
}

bool RemoteActuatorBus::jog(int channel, int angle) {
    if (!online()) return false;
    if (channel < 0 || channel > 15 || angle < 0 || angle > 180) return false;

    // Hand-built rather than routed through buildSetFrame(): that resolves a
    // stateId against a selector's calibration, and a jog is what you do BEFORE
    // there is any calibration to resolve against.
    StaticJsonDocument<256> doc;
    JsonObject o = doc.to<JsonObject>();
    xSemaphoreTake(_mutex, portMAX_DELAY);
    uint32_t seq = ++_seq;
    xSemaphoreGive(_mutex);

    o["t"]          = "SET";
    o["seq"]        = seq;
    o["selectorId"] = "__jog";
    o["stateId"]    = "__jog";
    o["drive"]      = "servo";
    o["channel"]    = channel;
    o["angle"]      = angle;
    o["holdAtRest"] = false;

    String s; serializeJson(doc, s);
    if (s.length() >= sizeof(_txFrame)) return false;

    xSemaphoreTake(_mutex, portMAX_DELAY);
    nodelink::strlcpy_(_txFrame, s.c_str(), sizeof(_txFrame));
    _txPending = true;
    // Deliberately NOT setting _moveOutstanding: a jog is a setup-time nudge, not
    // a routed move. Marking the bus busy() would stall the move queue behind a
    // gate someone is calibrating by hand.
    xSemaphoreGive(_mutex);
    return true;
}

RemoteActuatorBus::NodeInfo RemoteActuatorBus::info() const {
    NodeInfo n;
    if (!_mutex) {
        n.connected = false; n.lastSeenMs = 0;
        n.board[0] = '\0'; n.fw[0] = '\0'; n.capServos = 0; n.capLinear = 0;
        return n;
    }
    xSemaphoreTake(_mutex, portMAX_DELAY);
    n.connected  = _connected && (millis() - _lastRxMs) < nodelink::kPongTimeoutMs;
    n.lastSeenMs = _lastRxMs;
    nodelink::strlcpy_(n.board, _board, sizeof(n.board));
    nodelink::strlcpy_(n.fw,    _fw,    sizeof(n.fw));
    n.capServos = _capServos;
    n.capLinear = _capLinear;
    xSemaphoreGive(_mutex);
    return n;
}

} // namespace topo

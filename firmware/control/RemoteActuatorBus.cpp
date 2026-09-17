// =============================================================================
// RemoteActuatorBus.cpp — see the header for the threading + link-loss contract.
// =============================================================================

#include "RemoteActuatorBus.h"
#include "NodeBus.h"          // bareHost() — ONE spelling of a host, everywhere
#include <ESPmDNS.h>
#include "../utils/MdnsLock.h"   // one mDNS search at a time, across every task

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

    // A stable offset per board, so the re-resolve cadences interleave rather
    // than lock-stepping. Any cheap spread does; this one needs no state and is
    // the same across reboots, which keeps the log readable.
    _hostHash = 0;
    for (const char* p = _host; *p; ++p) _hostHash = _hostHash * 31u + (unsigned char)*p;

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
    //
    // bareHost(), NOT a suffix strip written here. This was its own copy until
    // 2026-09-17, and the copy was the weaker one: it missed the trailing dot on
    // a fully-qualified "host.local." and did not lower-case the result, so a
    // board entered with either would resolve here and then fail to match in
    // NodeBus, which normalises properly. Two spellings of one rule is how a
    // board gets dialled but never routed.
    char label[64];
    nodelink::strlcpy_(label, bareHost(_host).c_str(), sizeof(label));

    // SERIALISED — see utils/MdnsLock.h. Each node re-resolves from its own
    // task on the same cadence, so with more than one board these collide
    // constantly and every one of them reads the empty result as "gone".
    IPAddress ip((uint32_t)0);
    {
        mdnslock::Guard lock(label);
        if (lock.held()) ip = MDNS.queryHost(label, 1500);
    }
    if (ip == IPAddress((uint32_t)0)) {
        // ── MDNS WENT QUIET; WE STILL KNOW WHERE IT WAS ────────────────────
        //
        // Falling through to the `.local` name hands lwIP a name it will ask a
        // DNS server about, which correctly refuses it — the "DNS Failed for
        // 'x.local' with error -54" line in every one of these logs. That is a
        // guaranteed-failed lookup, and losing the board for it is worse than
        // trying the address it answered on five seconds ago.
        //
        // CLAUDE.md flags this exact path as one that "degrades to nothing",
        // which is the failure that turns up months later on a router reboot.
        // It degrades to the last known address now. The NAME stays the source
        // of truth — a re-resolve keeps running on its own cadence and replaces
        // this the moment mDNS answers — so DHCP moving a board still recovers,
        // it just takes a reconnect rather than a reboot.
        if (_lastIp[0]) {
            if (strcmp(_dialing, _lastIp) != 0) {
                DEBUG_PRINT(F("[NODE] ")); DEBUG_PRINT(label);
                DEBUG_PRINT(F(" — mDNS quiet, falling back to last known "));
                DEBUG_PRINTLN(_lastIp);
            }
            nodelink::strlcpy_(_dialing, _lastIp, sizeof(_dialing));
            _ws.begin(_dialing, _port, "/nodelink");
            return true;
        }
        return false;
    }

    String s = ip.toString();
    if (s.length() >= sizeof(_dialing)) return false;
    nodelink::strlcpy_(_dialing, s.c_str(), sizeof(_dialing));
    // Remembered for the fallback above, and readable by the sketch so it can be
    // persisted — a board that has resolved once should survive a power cut with
    // a silent querier.
    nodelink::strlcpy_(_lastIp, s.c_str(), sizeof(_lastIp));
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
            // STAGGERED, so N boards do not queue on the same tick forever. The
            // lock (utils/MdnsLock.h) makes a collision harmless, but without an
            // offset every task still wakes together, and the ones at the back
            // of the queue spend their whole interval waiting rather than
            // resolving. Derived from the host so it is stable across reboots
            // and needs nothing passed in.
            every += (unsigned long)(_hostHash % 7) * 400UL;
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
            // CONFIG rides the same single-threaded send. Sent BEFORE nothing
            // in particular — order against a SET does not matter, because a
            // node ACKs each independently and neither depends on the other.
            if (_cfgPending && _connected) {
                _ws.sendTXT(_cfgFrame);
                _cfgPending = false;
                DEBUG_PRINT(F("[NODE→] CONFIG to ")); DEBUG_PRINTLN(_nodeId);
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
            // The HELLO carries our claim. `_takeover` is one-shot and only ever
            // set by an explicit user action (see requestTakeover), so a
            // reconnect loop can never escalate itself into a theft.
            bool takeover = _takeover;
            _takeover = false;
            nodelink::buildHello(doc.to<JsonObject>(), _primaryId, _nodeId, takeover);
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
                // FORGET THE READINGS, KEEP THE CONFIG. A link that has dropped
                // tells us nothing about the tool any more, and a stale "on"
                // left lying here would keep a collector running for a machine
                // nobody can see (RFC §5.6a: absent is OFF). The CONFIG is the
                // opposite — it is ours, not the node's, and the node will have
                // forgotten it across the reboot.
                _senseCount = 0;
                _cfgPending = _cfgValid;
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
        // Absent means none: a board flashed before clamps existed answers
        // exactly as it always did rather than being read as broken.
        _capClamps = f["caps"]["ct"] | 0;

        // Did it accept our claim? A refusal leaves us OFFLINE rather than
        // half-connected: every caller already treats offline as "don't command
        // this board", which is exactly the required behaviour, and the socket
        // stays open so the user can be told who owns it.
        if (!nodelink::welcomeAccepted(f)) {
            nodelink::strlcpy_(_refusedBy, f["claimedBy"] | "another primary", sizeof(_refusedBy));
            _connected = false;
            xSemaphoreGive(_mutex);
            DEBUG_PRINT(F("[NODE] ")); DEBUG_PRINT(_nodeId);
            DEBUG_PRINT(F(" REFUSED us — it belongs to ")); DEBUG_PRINTLN(_refusedBy);
            DEBUG_PRINTLN(F("       Take it over from the boards screen if that is what you want."));
            return;
        }
        _refusedBy[0] = '\0';
        _connected = true;
        // Re-arm the CONFIG on every accepted handshake: this node may have just
        // rebooted, and a node that has not been configured reports nothing.
        if (_cfgValid) _cfgPending = true;
    } else if (strcmp(t, "ACK") == 0) {
        bool ok = f["ok"] | false;
        if (!ok) _moveOutstanding = false;                  // refused → stop waiting
        xSemaphoreGive(_mutex);
        DEBUG_PRINT(F("[NODE←] ACK seq=")); DEBUG_PRINT(f["seq"] | 0);
        DEBUG_PRINT(ok ? F(" ok") : F(" REFUSED: "));
        if (!ok) DEBUG_PRINT(f["err"] | "(no reason given)");
        DEBUG_PRINTLN();
        return;
    } else if (strcmp(t, "SENSE") == 0) {
        const char* sid = f["sensorId"].as<const char*>();
        const bool  on  = f["on"] | false;
        if (sid && *sid) {
            size_t i = 0;
            for (; i < _senseCount; i++) if (strcmp(_senses[i].sensorId, sid) == 0) break;
            // A node reporting more sensors than it was configured for is a
            // node out of step with us; keep the ones we know and drop the
            // rest rather than growing past the array.
            if (i == _senseCount && _senseCount < nodelink::kMaxSensorsPerNode) {
                nodelink::strlcpy_(_senses[i].sensorId, sid, sizeof(_senses[i].sensorId));
                _senseCount++;
            }
            if (i < nodelink::kMaxSensorsPerNode && i < _senseCount) {
                const bool changed = (_senses[i].on != on) || _senses[i].atMs == 0;
                _senses[i].on    = on;
                _senses[i].atMs  = millis();
                _senses[i].level = f["level"] | -1.0f;
                // Telemetry for the UI. Absent stays NEGATIVE rather than
                // becoming 0, because 0 A is a real reading from an idle tool
                // and "the node did not say" is not. Nothing branches on these.
                _senses[i].amps   = f["amps"]   | -1.0f;
                _senses[i].floorA = f["floorA"] | -1.0f;
                _senses[i].tripA  = f["tripA"]  | -1.0f;
                _senses[i].fault  = f["fault"]  | false;
                xSemaphoreGive(_mutex);
                // Logged on CHANGE only: this frame repeats every
                // kSenseRepeatMs, and a line per repeat would bury everything
                // else on the console within a minute.
                if (changed) {
                    DEBUG_PRINT(F("[NODE←] SENSE ")); DEBUG_PRINT(sid);
                    DEBUG_PRINTLN(on ? F(" ON") : F(" off"));
                }
                return;
            }
        }
        xSemaphoreGive(_mutex);
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

bool RemoteActuatorBus::jog(int channel, int angle, bool detach) {
    // A detach has no counterpart on the wire and needs none: holdAtRest is
    // false on a jog, so the node's ServoActuator de-energises on its own once
    // the sweep settles. Reported as HANDLED rather than refused — the caller
    // asked for a de-energised servo and that is what it gets.
    if (detach) return true;
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

void RemoteActuatorBus::configureSensors(JsonArrayConst sensors) {
    // Built here rather than by the caller so the WIRE SHAPE lives in one place
    // — nodelink.js's CONFIG, mirrored by parseConfigFrame() on the node.
    // 512, not 320: kMaxSensorsPerNode is 4 and a sensorId may be 48 chars, so
    // a legitimate full config is ~420 bytes. The old size would have refused
    // one — loudly, but still refused.
    StaticJsonDocument<512> doc;
    JsonObject f = doc.to<JsonObject>();
    f["t"] = "CONFIG";
    JsonArray arr = f.createNestedArray("sensors");
    for (JsonObjectConst sen : sensors) {
        JsonObject o = arr.createNestedObject();
        o["sensorId"] = sen["sensorId"] | "";
        o["kind"]     = "ct";
        o["channel"]  = sen["channel"] | 0;
    }

    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    f["seq"] = ++_seq;
    String s; serializeJson(doc, s);
    if (s.length() >= sizeof(_cfgFrame)) {
        xSemaphoreGive(_mutex);
        DEBUG_PRINT(F("[NODE] CONFIG too large for ")); DEBUG_PRINTLN(_nodeId);
        return;
    }
    nodelink::strlcpy_(_cfgFrame, s.c_str(), sizeof(_cfgFrame));
    _cfgValid   = true;
    _cfgPending = true;
    // Readings from the OLD configuration are not readings under the new one:
    // a sensorId that was just removed must stop answering immediately rather
    // than keep a tool switched on until it ages out.
    _senseCount = 0;
    xSemaphoreGive(_mutex);
}

bool RemoteActuatorBus::senseOf(const char* sensorId, bool& on, uint32_t& atMs) const {
    if (!sensorId || !*sensorId || !_mutex) return false;
    // const_cast: the mutex is a lock, not part of the logical value, and every
    // other const accessor on this class takes it the same way.
    SemaphoreHandle_t m = _mutex;
    xSemaphoreTake(m, portMAX_DELAY);
    bool found = false;
    for (size_t i = 0; i < _senseCount; i++) {
        if (strcmp(_senses[i].sensorId, sensorId) != 0) continue;
        // atMs == 0 means the slot exists but nothing has landed in it.
        if (_senses[i].atMs) { on = _senses[i].on; atMs = _senses[i].atMs; found = true; }
        break;
    }
    xSemaphoreGive(m);
    return found;
}

size_t RemoteActuatorBus::senseCount() const {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const size_t n = _senseCount;
    xSemaphoreGive(_mutex);
    return n;
}

bool RemoteActuatorBus::senseAt(size_t i, SenseView& v) const {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const bool ok = i < _senseCount;
    if (ok) {
        // Points into _senses[], which outlives the call — see SenseView.
        v.id = _senses[i].sensorId;
        // CONFIGURED BUT NEVER HEARD FROM is the state this endpoint exists to
        // make visible, and it is NOT the same as "off". A clamp the layout
        // names, on a board that is online, that has never sent a SENSE, means
        // the chain is broken somewhere between CONFIG and the ADC — which is
        // exactly the question being asked on a bench.
        v.reported = _senses[i].atMs != 0;
        v.on       = _senses[i].on;
        // AGE, not the raw timestamp: this becomes a JSON body a phone reads,
        // and millis() on this board means nothing at the other end.
        v.ageMs    = v.reported ? (uint32_t)(millis() - _senses[i].atMs) : 0;
        v.level    = _senses[i].level;
        v.amps     = _senses[i].amps;
        v.floorA   = _senses[i].floorA;
        v.tripA    = _senses[i].tripA;
        v.fault    = _senses[i].fault;
    }
    xSemaphoreGive(_mutex);
    return ok;
}

RemoteActuatorBus::NodeInfo RemoteActuatorBus::info() const {
    NodeInfo n;
    if (!_mutex) {
        n.connected = false; n.lastSeenMs = 0;
        n.board[0] = '\0'; n.fw[0] = '\0'; n.capServos = 0; n.capLinear = 0; n.capClamps = 0;
        return n;
    }
    xSemaphoreTake(_mutex, portMAX_DELAY);
    n.connected  = _connected && (millis() - _lastRxMs) < nodelink::kPongTimeoutMs;
    n.lastSeenMs = _lastRxMs;
    nodelink::strlcpy_(n.board, _board, sizeof(n.board));
    nodelink::strlcpy_(n.fw,    _fw,    sizeof(n.fw));
    n.capServos = _capServos;
    n.capLinear = _capLinear;
    n.capClamps = _capClamps;
    xSemaphoreGive(_mutex);
    return n;
}

} // namespace topo

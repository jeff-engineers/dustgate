// =============================================================================
// RemoteActuatorBus.cpp — see the header for the threading + link-loss contract.
// =============================================================================

#include "RemoteActuatorBus.h"
#include "esp_heap_caps.h"   // internal-DRAM reporting in the dialling nag
#include "NodeBus.h"          // bareHost() — ONE spelling of a host, everywhere
#include <ESPmDNS.h>
#include "../utils/MdnsLock.h"   // one mDNS search at a time, across every task
#include "../utils/Watchdog.h"   // end() waits on the main loop
#include "../utils/LinkLog.h"    // every link up/down/refusal, kept on flash for a shop soak

#ifndef DEBUG_PRINT
  #define DEBUG_PRINT(x)   Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
#endif

namespace topo {

// This task parses ≤384-byte frames, calls into the WS library, and since
// 2026-09-27 formats link-log lines (utils/LinkLog.h — a 240-byte line plus
// snprintf) from inside handleFrame, where a 384-byte JSON document is already on
// the stack. 4096 left too little margin for that, so 6144 — which costs 2 KB of
// internal DRAM per DIALLING node (10 max: +20 KB). The "Still dialling" nag
// prints the high-water mark; trim this from real numbers, not guesses.
// 5120, down from 6144 (2026-10-03): the nag below measured 2496 B never used at 6144
// on a bench with four nodes, so 1 KB per node comes back to the heap and ~1.4 KB of
// margin stays. Not lower: the figure is a high-water mark under the frames seen so
// far, and a CONFIG or SENSE burst is the worst case it has not met.
static const uint32_t kNodeLinkTaskStack = 5120;
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

    // NOTHING IS RESOLVED OR DIALLED HERE — the task's first act is to do both
    // (taskLoop). This used to resolve synchronously, on the CALLER: a Pair tap
    // runs syncPairedNodes() on the main loop, which re-begins EVERY paired
    // node, so it paid one mDNS query (up to 1.5 s, plus up to 8 s queueing for
    // the lock) per node — past the 10 s loop watchdog with a few boards dark,
    // which reset the primary for pressing a button.
    _dialing[0]    = '\0';
    _dialedOnce    = false;
    _downSinceMs   = millis();   // down until a WELCOME says otherwise
    _lastMdnsOkMs  = 0;
    _hollowDrops   = 0;
    _everLinked    = false;
    _sockUp        = false;
    _seenFailMs    = 0;
    _lastResolveMs = 0;

    _ws.onEvent([this](WStype_t t, uint8_t* p, size_t l) { onEvent(t, p, l); });
    // Library-level auto-reconnect handles the common case; the backoff bounds
    // come from the shared contract so the mock secondary can expect the same.
    // START AT THE MINIMUM AND BACK OFF FROM THERE — see _backoff() below.
    // This used to be the whole story, and kReconnectMaxMs was never read.
    _retryMs = nodelink::kReconnectMinMs;
    _ws.setReconnectInterval(_retryMs);
    _ws.enableHeartbeat(nodelink::kPingIntervalMs, nodelink::kPongTimeoutMs, 2);

    _running   = true;
    _taskAlive = true;
    // Checked, because the failure is otherwise completely silent: no task means
    // nothing ever pumps _ws.loop(), so the socket never opens and the node sits
    // at "paired but offline" forever — indistinguishable from a dead board.
    BaseType_t ok = xTaskCreatePinnedToCore(taskTrampoline, "nodelink", kNodeLinkTaskStack,
                                            this, kNodeLinkTaskPrio, &_task, 0);
    if (ok != pdPASS) {
        _running   = false;
        _taskAlive = false;
        _task      = nullptr;
        DEBUG_PRINT(F("[NODE] FAILED to start link task for ")); DEBUG_PRINT(_nodeId);
        DEBUG_PRINT(F(" — free heap ")); DEBUG_PRINTLN(ESP.getFreeHeap());
        return;
    }
}

// GROW THE RETRY INTERVAL, up to kReconnectMaxMs.
//
// THIS DID NOT EXIST UNTIL 2026-09-18, and its absence is a bug that took down a
// bench shop. setReconnectInterval() was called once with kReconnectMinMs and
// never again, so the library retried every SECOND forever — kReconnectMaxMs was
// declared, documented in nodelink.js as "reconnect backoff", mirrored in the
// pair table, and read by nothing.
//
// What that costs is not politeness, it is the shop. ESPAsyncWebServer on the
// node does not reap dead WebSocket clients on its own; a connection per second
// burns its client slots faster than cleanupClients() frees them, and a node with
// no slots left RESETS every incoming connection. The primary then retries a
// second later, forever. Observed on a real board as an endless "Link lost" loop
// with errno 104 (connection reset by peer), recoverable only by power-cycling
// the NODE — the primary could not fix it because the primary was causing it.
//
// The node's own loop() comment predicted the symptom exactly: "a node that
// answers a laptop fine while refusing the primary indefinitely."
//
// Doubling from 1s reaches the 15s ceiling after four failures, so a node that is
// briefly busy is still picked up quickly while one that is genuinely gone is
// polled four times a minute instead of sixty.
void RemoteActuatorBus::_backoff() {
    if (_retryMs >= nodelink::kReconnectMaxMs) return;   // already at the ceiling
    _retryMs *= 2;
    if (_retryMs > nodelink::kReconnectMaxMs) _retryMs = nodelink::kReconnectMaxMs;
    _ws.setReconnectInterval(_retryMs);
    DEBUG_PRINT(F("[NODE] backing off ")); DEBUG_PRINT(_nodeId);
    DEBUG_PRINT(F(" — retrying every ")); DEBUG_PRINT(_retryMs);
    DEBUG_PRINTLN(F(" ms"));
}

// Resolve the name OURSELVES rather than handing "<name>.local" to the socket
// and hoping.
//
// WebSocketsClient resolves through lwIP's hostByName(), whose mDNS fallback for
// ".local" names is unreliable in practice: on this bench it failed twice during
// boot and succeeded a minute later, with the node advertising perfectly the
// whole time and a laptop resolving it instantly. ESP-IDF's mDNS querier is the
// same machinery that already finds Shelly plugs and DustGate nodes reliably, so
// use that and dial the resulting IP. The NAME stays the source of truth (DHCP
// can move the board); the IP is just this attempt's answer, re-resolved
// whenever the link is down. LINK TASK ONLY — it can block for seconds.
bool RemoteActuatorBus::resolveAndDial() {
    _lastResolveMs = millis();

    if (_hostIsIp) {
        dialTo(_host);
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
    // A SHORT wait for the lock: see mdnslock::Guard. Worst case on this task
    // is then ~2 s queueing + 1.5 s querying + WEBSOCKETS_TCP_TIMEOUT dialling,
    // which is what end() has to be prepared to wait out.
    IPAddress ip((uint32_t)0);
    {
        mdnslock::Guard lock(label, 2000);
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
            dialTo(_lastIp);
            return true;
        }
        return false;
    }

    // A SIGN OF LIFE for the WiFi rejoin — see LinkHealth. Stamped whenever the
    // name answers, even when the address is unchanged and nothing is logged.
    _lastMdnsOkMs = millis() ? millis() : 1;

    String s = ip.toString();
    if (s.length() >= sizeof(_dialing)) return false;
    // Remembered for the fallback above, and readable by the sketch so it can be
    // persisted — a board that has resolved once should survive a power cut with
    // a silent querier.
    nodelink::strlcpy_(_lastIp, s.c_str(), sizeof(_lastIp));
    if (strcmp(_dialing, s.c_str()) != 0) {
        DEBUG_PRINT(F("[NODE] ")); DEBUG_PRINT(label);
        DEBUG_PRINT(F(" resolved to ")); DEBUG_PRINTLN(s);
    }
    dialTo(s.c_str());
    return true;
}

// RE-POINT THE SOCKET ONLY WHEN THE ADDRESS ACTUALLY CHANGED.
//
// WebSocketsClient::begin() is a constructor in disguise: it sets
// `_client.tcp = NULL` without closing or deleting what was there, and zeroes
// the reconnect timer. Called on an open socket it LEAKS the connection — the
// WiFiClient object and its lwIP socket on this board, and a live WebSocket
// client slot on the node that cleanupClients() never reaps, because to the
// node it is still established. The re-resolve used to call it every 3-15 s
// for as long as a link was not WELCOMEd, which includes a node that REFUSED
// our claim and keeps its socket open: one orphan per cycle, forever, until the
// node's pool filled and it reset every connection (errno 104) and this board
// ran short of sockets for its plugs.
//
// So: the same address is a no-op (the library's own reconnect keeps trying it,
// on the backed-off interval begin() would otherwise have reset), and a new one
// closes the old socket first.
void RemoteActuatorBus::dialTo(const char* target) {
    if (_dialedOnce && strcmp(target, _dialing) == 0) return;
    if (_dialedOnce) _ws.disconnect();   // a no-op unless something is open
    nodelink::strlcpy_(_dialing, target, sizeof(_dialing));
    _ws.begin(_dialing, _port, "/nodelink");
    if (!_dialedOnce) {
        DEBUG_PRINT(F("[NODE] Linking to ")); DEBUG_PRINT(_nodeId);
        DEBUG_PRINT(F(" at ws://")); DEBUG_PRINT(_dialing); DEBUG_PRINTLN(F("/nodelink"));
    }
    _dialedOnce = true;
}

// WAIT FOR THE TASK TO LEAVE; DO NOT KILL IT.
//
// This used to give the task 500 ms and then vTaskDelete() it. The task spends
// most of an unreachable node's life inside a blocking call — an mDNS query
// holding the shop-wide mdnslock, or a TCP connect — and a task deleted there
// never runs the code after it: the lock is never given back (every later mDNS
// search on this board then waits 8 s and gives up, "[mDNS] busy — skipped"),
// or the socket is never closed. Deleting a task cannot release what it holds.
//
// The bound comes from the task's own worst case (resolveAndDial()'s comment):
// ~2 s for the lock, 1.5 s for the query, WEBSOCKETS_TCP_TIMEOUT to dial. Pet the
// loop watchdog while waiting, since this runs on the main loop during a Pair.
static const uint32_t kStopWaitMs = 8000;

void RemoteActuatorBus::end() {
    requestStop();
    const uint32_t t0 = millis();
    while (_taskAlive && (millis() - t0) < kStopWaitMs) {
        watchdog::pet();
        delay(10);
    }
    if (_taskAlive) {
        // Should not happen given the bounds above. If it does, the task is
        // wedged somewhere unbounded, and deleting it is the lesser evil — but
        // say so, because whatever it held is now held forever.
        DEBUG_PRINT(F("[NODE] ⚠ link task for ")); DEBUG_PRINT(_nodeId);
        DEBUG_PRINTLN(F(" did not stop — deleting it; mDNS or a socket may now be leaked"));
        if (_task) vTaskDelete(_task);
        _taskAlive = false;
    }
    _task = nullptr;
    _ws.disconnect();
    _sockUp = false;
    if (_mutex) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _connected = false; _moveOutstanding = false; _txPending = false;
        xSemaphoreGive(_mutex);
    }
}

void RemoteActuatorBus::taskLoop() {
    // Staggered start — see setStartDelay(). Re-checks _running so end() is not kept
    // waiting by a link that has not started yet.
    for (uint32_t waited = 0; _running && waited < _startDelayMs; waited += 50) delay(50);

    // FIRST DIAL, here rather than in begin() — see begin() for why.
    if (_running && !resolveAndDial()) {
        // Fall back to letting the socket try the name — sometimes lwIP does
        // manage it — and let the re-resolve below replace it when mDNS answers.
        char name[64];
        snprintf(name, sizeof(name), "%s%s", _host, strchr(_host, '.') ? "" : ".local");
        DEBUG_PRINT(F("[NODE] mDNS didn't answer for ")); DEBUG_PRINT(_host);
        DEBUG_PRINT(F(" — dialling ")); DEBUG_PRINT(name);
        DEBUG_PRINTLN(F(" and will retry"));
        dialTo(name);
    }

    unsigned long lastNagMs = millis();
    while (_running) {
        // A node that dialled us IS the link. Leave the dial-out socket closed: a
        // second one to the same node only teaches it that "another link" exists, and
        // that is exactly the churn that fills a node's connection slots.
        if (_inId) {
            if (_sockUp || _ws.isConnected()) { _ws.disconnect(); _sockUp = false; }
            lastNagMs = millis();
            delay(50);
            continue;
        }
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
        // Back off on EVERY failure the library records — a connect that did not
        // happen (no event at all, see NodeLinkClient) as well as a link that
        // dropped (whose clientDisconnect() stamps the same field). Zero means
        // "connected" or "freshly begun", neither of which is a failure.
        {
            const unsigned long f = _ws.lastConnectFailMs();
            if (f != _seenFailMs) {
                _seenFailMs = f;
                if (f) _backoff();
            }
        }

        // Only while the SOCKET is down, not merely un-WELCOMEd: see dialTo().
        if (!_sockUp && !_connected && !_hostIsIp) {
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
                if (resolveAndDial() && prev[0] && strcmp(prev, _dialing) != 0) {
                    DEBUG_PRINT(F("[NODE] Now dialling ")); DEBUG_PRINT(_dialing);
                    DEBUG_PRINT(F(" (was ")); DEBUG_PRINT(prev); DEBUG_PRINTLN(F(")"));
                }
            }
        }

        if (!_connected && (millis() - lastNagMs) > 10000) {
            lastNagMs = millis();
            DEBUG_PRINT(F("[NODE] Still dialling ")); DEBUG_PRINT(_dialing);
            DEBUG_PRINT(F(" (")); DEBUG_PRINT(_host); DEBUG_PRINT(F(")"));
            // INTERNAL DRAM, NOT ESP.getFreeHeap(). This printed the total,
            // which on a PSRAM board counts PSRAM — and PSRAM cannot back a
            // task stack, a DMA descriptor or a WiFi buffer. Every per-node cost
            // that scales here (a 4 KB nodelink task stack, the socket's
            // buffers) and every mDNS search comes out of INTERNAL, so the
            // number that was being watched could look healthy while the one
            // that matters fell off a cliff. bootTrace() in firmware.ino already
            // made this distinction; this line had not caught up.
            //
            // `largest` is printed too because early allocation failures are
            // usually FRAGMENTATION rather than exhaustion, and the two look
            // identical if you only watch the free total.
            DEBUG_PRINT(F(" — internal "));
            DEBUG_PRINT((unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            DEBUG_PRINT(F(" largest "));
            DEBUG_PRINTLN((unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            // The two signs of life the WiFi rejoin weighs, on the line people
            // already read — so a capture says which one fired, or that neither
            // did and the node is simply off.
            const LinkHealth h = health();
            DEBUG_PRINT(F("       signs of life: mDNS "));
            if (h.mdnsAgeMs == UINT32_MAX) DEBUG_PRINT(F("never"));
            else { DEBUG_PRINT(h.mdnsAgeMs / 1000); DEBUG_PRINT(F("s ago")); }
            DEBUG_PRINT(F(", hollow drops ")); DEBUG_PRINT(h.hollowDrops);
            DEBUG_PRINT(F("; task stack never below "));
            DEBUG_PRINT((unsigned)uxTaskGetStackHighWaterMark(NULL));
            DEBUG_PRINTLN(F(" B free"));
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
            if (_otaPending && _connected) {
                _ws.sendTXT(_otaFrame);
                _otaPending = false;
                DEBUG_PRINT(F("[NODE→] OTA to ")); DEBUG_PRINTLN(_nodeId);
            }
            // A move whose STATE report never arrived: give up rather than let
            // the primary's move queue block forever behind a lost frame.
            if (_moveOutstanding &&
                (millis() - _moveStartedMs) > nodelink::kMoveTimeoutMs) {
                _moveOutstanding = false;
                _moveFault = "The board never reported its move finished (timed out).";
                DEBUG_PRINT(F("[NODE] Move timed out on ")); DEBUG_PRINTLN(_nodeId);
            }
            xSemaphoreGive(_mutex);
        }
        delay(5);
    }
    // LAST, and only here: end() reads this to know the task has let go of
    // everything it might have been holding.
    _taskAlive = false;
    vTaskDelete(NULL);
}

// The HELLO. `_takeover` is one-shot and only ever set by an explicit user action
// (see requestTakeover), so a reconnect loop can never escalate itself into a theft.
void RemoteActuatorBus::buildHelloString(String& out) {
    StaticJsonDocument<192> doc;
    const bool takeover = _takeover;
    _takeover = false;
    nodelink::buildHello(doc.to<JsonObject>(), _primaryId, _nodeId, takeover);
    serializeJson(doc, out);
}

// What the link going away means, whichever way it was dialled.
void RemoteActuatorBus::markDown(bool wasUp) {
    (void)wasUp;
    // Logged only for a link that was UP. A retry that fails again is not news, and
    // at one line per retry it would bury the log.
    bool logDown = false;
    if (_mutex) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        if (_connected) { _downSinceMs = millis(); logDown = true; }
        _connected = false;
        // Drop any outstanding move: we can't know whether it landed, and holding
        // busy() forever would stall every other gate.
        if (_moveOutstanding) _moveFault = "The link dropped mid-move \xE2\x80\x94 the gate may not have finished moving.";
        _moveOutstanding = false;
        _txPending = false;
        // FORGET THE READINGS, KEEP THE CONFIG. A link that has dropped tells us
        // nothing about the tool any more, and a stale "on" left lying here would
        // keep a collector running for a machine nobody can see (RFC §5.6a: absent
        // is OFF). The CONFIG is the opposite — it is ours, not the node's, and the
        // node will have forgotten it across the reboot.
        _senseCount = 0;
        _cfgPending = _cfgValid;
        xSemaphoreGive(_mutex);
    }
    DEBUG_PRINT(F("[NODE] Link lost: ")); DEBUG_PRINTLN(_nodeId);
    if (logDown) linklog::event("link_down", _host);
}

// ── node-initiated links ─────────────────────────────────────────────────────
bool RemoteActuatorBus::attachInbound(AsyncWebSocketClient* c, String& helloOut) {
    if (!_mutex || !c) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const bool fresh = _connected && (millis() - _lastRxMs) < nodelink::kPongTimeoutMs;
    if (fresh) { xSemaphoreGive(_mutex); return false; }   // one healthy link only
    _inClient   = c;
    _inId       = c->id();
    _lastRxMs   = millis();         // the node just spoke; do not call it overdue before the WELCOME
    _lastPingMs = millis();
    // Whatever was half-open on the dial-out side is stale now.
    _connected  = false;
    _txPending  = false;
    xSemaphoreGive(_mutex);
    buildHelloString(helloOut);
    DEBUG_PRINT(F("[NODE] ")); DEBUG_PRINT(_nodeId); DEBUG_PRINTLN(F(" dialled in"));
    return true;
}

void RemoteActuatorBus::detachInbound(uint32_t clientId) {
    if (!_inId || _inId != clientId) return;
    _inClient = nullptr;
    _inId     = 0;
    markDown(true);
}

void RemoteActuatorBus::onInboundPong() {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _lastRxMs = millis();
    xSemaphoreGive(_mutex);
}

void RemoteActuatorBus::pumpInbound() {
    AsyncWebSocketClient* c = _inClient;
    if (!c || !_inId) return;
    if (c->status() != WS_CONNECTED) return;
    char tx[sizeof(_txFrame)] = ""; char cfg[sizeof(_cfgFrame)] = ""; char ota[sizeof(_otaFrame)] = "";
    bool haveTx = false, haveCfg = false, haveOta = false;
    if (xSemaphoreTake(_mutex, 0) != pdTRUE) return;      // try again next loop
    if (_txPending && _connected)  { strlcpy(tx,  _txFrame,  sizeof(tx));  _txPending  = false; haveTx  = true; }
    if (_cfgPending && _connected) { strlcpy(cfg, _cfgFrame, sizeof(cfg)); _cfgPending = false; haveCfg = true; }
    if (_otaPending && _connected) { strlcpy(ota, _otaFrame, sizeof(ota)); _otaPending = false; haveOta = true; }
    if (_moveOutstanding && (millis() - _moveStartedMs) > nodelink::kMoveTimeoutMs) {
        _moveOutstanding = false;
        _moveFault = "The board never reported its move finished (timed out).";
        DEBUG_PRINT(F("[NODE] Move timed out on ")); DEBUG_PRINTLN(_nodeId);
    }
    xSemaphoreGive(_mutex);
    if (haveTx)  c->text(tx);
    if (haveCfg) { c->text(cfg); DEBUG_PRINT(F("[NODE→] CONFIG to ")); DEBUG_PRINTLN(_nodeId); }
    if (haveOta) { c->text(ota); DEBUG_PRINT(F("[NODE→] OTA to "));    DEBUG_PRINTLN(_nodeId); }
}

void RemoteActuatorBus::update() {
    if (!_inId) return;
    pumpInbound();
    // The primary pings a node it dialled the way the dial-out path always did — at
    // the shared interval — and a PONG (onInboundPong) is what keeps online() true.
    if (millis() - _lastPingMs >= nodelink::kPingIntervalMs) {
        _lastPingMs = millis();
        AsyncWebSocketClient* c = _inClient;
        if (c && c->status() == WS_CONNECTED) c->ping();
    }
}

void RemoteActuatorBus::onEvent(WStype_t type, uint8_t* payload, size_t len) {
    switch (type) {
        case WStype_CONNECTED: {
            _sockUp = true;
            // The node dialled us in the meantime and that socket is THE link: say
            // nothing on this one and let the stand-down in taskLoop close it.
            if (_inId) break;
            // The backoff is NOT reset here any more — see the WELCOME branch of
            // handleFrame(). Resetting on a bare socket kept the storm it was
            // written for at 1-2 s: connect (reset to 1 s), drop (double to
            // 2 s), connect again.
            //
            // Socket is up but the node hasn't identified itself yet — stay
            // offline until WELCOME lands so we never command an unknown board.
            String s; buildHelloString(s);
            _ws.sendTXT(s);
            break;
        }
        case WStype_DISCONNECTED: {
            // HOLLOW: the socket never reached WStype_CONNECTED — TCP was
            // accepted and then nothing answered the upgrade. See LinkHealth.
            const bool hollow = !_sockUp;
            _sockUp = false;
            // Not this bus's link any more — the socket that carries it is the node's
            // own. Marking the bus down for the death of a spare would drop a good link.
            if (_inId) break;
            if (hollow && _hollowDrops < 0xFFFF) _hollowDrops++;
            markDown(!hollow);
            // No _backoff() here: the drop stamped _lastConnectionFail, and the
            // task loop backs off once per stamp. Calling it here as well would
            // count every drop twice.
            break;
        }
        case WStype_TEXT:
            if (_inId) break;     // a spare socket's frames are not the link's
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

    // Filled in the WELCOME branch under the lock, logged after it is released.
    bool logUp = false;
    char upExtra[160] = "";

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
        // Absent means NO: a node that predates plug polling must stay
        // brain-polled, or it is handed a CONFIG it refuses whole — clamp and all.
        _capPlugs  = f["caps"]["plug"] | 0;
        // Absent means NO: a board that predates node-initiated links is dialled.
        _capJoin   = f["caps"]["join"] | 0;

        // Did it accept our claim? A refusal leaves us OFFLINE rather than
        // half-connected: every caller already treats offline as "don't command
        // this board", which is exactly the required behaviour, and the socket
        // stays open so the user can be told who owns it.
        if (!nodelink::welcomeAccepted(f)) {
            nodelink::strlcpy_(_refusedBy, f["claimedBy"] | "another primary", sizeof(_refusedBy));
            _connected = false;
            xSemaphoreGive(_mutex);
            {
                char safe[40], extra[80];
                linklog::safeCopy(safe, sizeof(safe), _refusedBy);
                snprintf(extra, sizeof(extra), "\"owner\":\"%s\"", safe);
                linklog::event("refused", _host, extra);
            }
            DEBUG_PRINT(F("[NODE] ")); DEBUG_PRINT(_nodeId);
            DEBUG_PRINT(F(" REFUSED us — it belongs to ")); DEBUG_PRINTLN(_refusedBy);
            DEBUG_PRINTLN(F("       Take it over from the boards screen if that is what you want."));
            return;
        }
        _refusedBy[0] = '\0';
        // A node that has come back has, by definition, finished (or abandoned)
        // whatever update it was running — the fw in this WELCOME is the verdict.
        // A refusal ("fail") is kept: nothing about the node changed.
        if (strcmp(_otaState, "fail") != 0) { _otaState[0] = '\0'; _otaPct = -1; _otaErr[0] = '\0'; }
        // LINK LOG: how long it was down and how it looked while it was, taken
        // BEFORE the reset below, plus the node's own account of its boot
        // (withBootInfo) — an upS shorter than the outage means the NODE
        // rebooted, and `rst` says whether that was power or a crash.
        const uint32_t upDownMs  = _downSinceMs ? (millis() - _downSinceMs) : 0;
        const uint16_t upHollow  = _hollowDrops;
        const bool     upFirst   = !_everLinked;
        const long     upNodeUpS = f.containsKey("upS") ? (long)(f["upS"] | 0UL) : -1L;
        char upRst[nodelink::kMaxRstLen + 1];
        linklog::safeCopy(upRst, sizeof(upRst), f["rst"] | "");
        _everLinked = true;
        logUp = true;
        _connected = true;
        _downSinceMs = 0;
        _hollowDrops = 0;
        snprintf(upExtra, sizeof(upExtra),
                 "\"downMs\":%lu,\"hollow\":%u,\"first\":%s,\"nodeUpS\":%ld,\"nodeRst\":\"%s\"",
                 (unsigned long)upDownMs, (unsigned)upHollow, upFirst ? "true" : "false",
                 upNodeUpS, upRst);
        // A HEALTHY LINK RESETS THE BACKOFF, and an accepted WELCOME is the
        // first moment we know it is one. (This task is the only caller of
        // setReconnectInterval, and handleFrame runs on it.)
        _retryMs = nodelink::kReconnectMinMs;
        _ws.setReconnectInterval(_retryMs);
        // Re-arm the CONFIG on every accepted handshake: this node may have just
        // rebooted, and a node that has not been configured reports nothing.
        if (_cfgValid) _cfgPending = true;
    } else if (strcmp(t, "ACK") == 0) {
        bool ok = f["ok"] | false;
        if (!ok) {
            if (_moveOutstanding) _moveFault = "The board refused the move.";
            _moveOutstanding = false;                       // refused → stop waiting
        }
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
                _senses[i].isPlug = f["plug"]   | false;
                _senses[i].watts  = f["watts"]  | -1.0f;
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
    } else if (strcmp(t, "OTASTATE") == 0) {
        const char* st = f["state"] | "";
        nodelink::strlcpy_(_otaState, st, sizeof(_otaState));
        _otaTouchedMs = millis();
        _otaPct = f.containsKey("pct") ? (int)(f["pct"] | 0) : _otaPct;
        nodelink::strlcpy_(_otaErr, f["err"] | "", sizeof(_otaErr));
        char extra[120];
        char safe[48];
        linklog::safeCopy(safe, sizeof(safe), _otaErr);
        snprintf(extra, sizeof(extra), "\"state\":\"%s\",\"pct\":%d,\"err\":\"%s\"", _otaState, _otaPct, safe);
        const bool log = strcmp(st, "progress") != 0;   // a start/done/fail is an event; progress is noise
        xSemaphoreGive(_mutex);
        DEBUG_PRINT(F("[NODE←] OTA ")); DEBUG_PRINT(_nodeId); DEBUG_PRINT(' '); DEBUG_PRINT(st);
        DEBUG_PRINT(' '); DEBUG_PRINTLN(_otaPct);
        if (log) linklog::event("ota", _host, extra);
        return;
    } else if (strcmp(t, "STATE") == 0) {
        bool moving = f["moving"] | false;
        if (!moving) { _moveOutstanding = false; _moveFault = nullptr; }
        xSemaphoreGive(_mutex);
        DEBUG_PRINT(F("[NODE←] STATE ")); DEBUG_PRINT(f["selectorId"] | "?");
        DEBUG_PRINT(F(" -> ")); DEBUG_PRINT(f["stateId"] | "?");
        DEBUG_PRINTLN(moving ? F(" (moving)") : F(" (arrived)"));
        return;
    }
    xSemaphoreGive(_mutex);
    if (logUp) linklog::event("link_up", _host, upExtra);
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
    // 1024 since plug sensors (2026-10-03): four of them are four objects of
    // five members, and overflow DROPS members silently.
    StaticJsonDocument<1024> doc;
    JsonObject f = doc.to<JsonObject>();
    f["t"] = "CONFIG";
    JsonArray arr = f.createNestedArray("sensors");
    for (JsonObjectConst sen : sensors) {
        JsonObject o = arr.createNestedObject();
        o["sensorId"] = sen["sensorId"] | "";
        if (strcmp(sen["kind"] | "ct", "plug") == 0) {
            o["kind"]       = "plug";
            o["ip"]         = sen["ip"] | "";
            o["plug"]       = sen["plug"] | "shelly";
            o["thresholdW"] = sen["thresholdW"] | 0.0f;
        } else {
            o["kind"]     = "ct";
            o["channel"]  = sen["channel"] | 0;
        }
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

bool RemoteActuatorBus::plugReading(const char* sensorId, float& watts, bool& fault, uint32_t& atMs) const {
    if (!sensorId || !*sensorId || !_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool found = false;
    for (size_t i = 0; i < _senseCount; i++) {
        if (strcmp(_senses[i].sensorId, sensorId) != 0) continue;
        if (_senses[i].atMs && _senses[i].isPlug) {
            watts = _senses[i].watts < 0.0f ? 0.0f : _senses[i].watts;
            fault = _senses[i].fault;
            atMs  = _senses[i].atMs;
            found = true;
        }
        break;
    }
    xSemaphoreGive(_mutex);
    return found;
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
        v.isPlug   = _senses[i].isPlug;
        v.watts    = _senses[i].watts;
    }
    xSemaphoreGive(_mutex);
    return ok;
}

RemoteActuatorBus::LinkHealth RemoteActuatorBus::health() const {
    LinkHealth h{false, false, 0, UINT32_MAX, 0, nullptr};
    if (!_mutex) return h;
    const uint32_t now = millis();
    const uint32_t mdns = _lastMdnsOkMs;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    h.linked      = _connected && (now - _lastRxMs) < nodelink::kPongTimeoutMs;
    h.refused     = _refusedBy[0] != '\0';
    h.downForMs   = (h.linked || !_downSinceMs) ? 0 : (now - _downSinceMs);
    h.hollowDrops = _hollowDrops;
    h.moveFault   = _moveFault;
    xSemaphoreGive(_mutex);
    h.mdnsAgeMs   = mdns ? (now - mdns) : UINT32_MAX;
    return h;
}

RemoteActuatorBus::NodeInfo RemoteActuatorBus::info() const {
    NodeInfo n;
    if (!_mutex) {
        n.connected = false; n.lastSeenMs = 0;
        n.board[0] = '\0'; n.fw[0] = '\0'; n.capServos = 0; n.capLinear = 0; n.capClamps = 0;
        n.ota[0] = '\0'; n.otaPct = -1; n.otaErr[0] = '\0';
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
    nodelink::strlcpy_(n.ota,    _otaState, sizeof(n.ota));
    n.otaPct = _otaPct;
    nodelink::strlcpy_(n.otaErr, _otaErr,   sizeof(n.otaErr));
    // An update that stopped reporting is a failure, not a progress bar that never
    // moves — most likely a node whose firmware predates OTA, which ignores the
    // frame (an unknown frame is ignored, not refused; see nodelink.js).
    if ((strcmp(_otaState, "start") == 0 || strcmp(_otaState, "progress") == 0) &&
        millis() - _otaTouchedMs > 30000UL) {
        nodelink::strlcpy_(n.ota, "fail", sizeof(n.ota));
        nodelink::strlcpy_(n.otaErr, "no answer - this board's firmware predates updates", sizeof(n.otaErr));
        n.otaPct = -1;
    }
    xSemaphoreGive(_mutex);
    return n;
}

bool RemoteActuatorBus::requestOta(const char* path, uint32_t size, const char* md5,
                                   const char* fw, const char*& why) {
    if (!_mutex) { why = "not started"; return false; }
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const bool up = _connected && (millis() - _lastRxMs) < nodelink::kPongTimeoutMs;
    const bool busy = ((strcmp(_otaState, "start") == 0 || strcmp(_otaState, "progress") == 0) &&
                       millis() - _otaTouchedMs < 30000UL) || _otaPending;
    if (!up)   { xSemaphoreGive(_mutex); why = "the board is offline"; return false; }
    if (busy)  { xSemaphoreGive(_mutex); why = "an update is already running"; return false; }
    StaticJsonDocument<384> d;
    nodelink::buildOta(d.to<JsonObject>(), ++_otaSeq, path, size, md5, fw);
    if (serializeJson(d, _otaFrame, sizeof(_otaFrame)) >= sizeof(_otaFrame)) {
        xSemaphoreGive(_mutex); why = "frame too long"; return false;
    }
    _otaPending = true;
    _otaTouchedMs = millis();
    nodelink::strlcpy_(_otaState, "start", sizeof(_otaState));
    _otaPct = 0;
    _otaErr[0] = '\0';
    xSemaphoreGive(_mutex);
    return true;
}

void RemoteActuatorBus::noteOtaRefused(const char* why) {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    nodelink::strlcpy_(_otaState, "fail", sizeof(_otaState));
    _otaPct = -1;
    nodelink::strlcpy_(_otaErr, why ? why : "refused", sizeof(_otaErr));
    xSemaphoreGive(_mutex);
}

} // namespace topo

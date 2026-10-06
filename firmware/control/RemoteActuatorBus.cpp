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
// How long a node that dials in is left to do it before this side dials too. Longer
// than a node takes to boot and find us (a few seconds, beacon every 5 s while it is
// down), shorter than anyone waits for a gate.
static const uint32_t kDialInGraceMs = 10000;

// The session is given its clock rather than reading one.
static uint32_t clockMs() { return (uint32_t)millis(); }

void RemoteActuatorBus::begin(const char* nodeId, const char* primaryId,
                              const char* host, uint16_t port) {
    if (_running) end();

    if (!_mutex) _mutex = xSemaphoreCreateMutex();
    // What the session learned about its node survives a restart of the SAME node (a pause and a
    // resume, a takeover) — in particular `caps.join`, which is what lets the primary give a node
    // that dials in the first move instead of racing it. A slot reused for a DIFFERENT node must
    // not inherit any of it; NodeSession::configure() knows the difference.
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _s.configure(nodeId, primaryId);
    xSemaphoreGive(_mutex);
    nodelink::strlcpy_(_host,      host      ? host      : "", sizeof(_host));
    _port = port;
    if (_host[0] == '\0') {
        DEBUG_PRINT(F("[NODE] No link.host for node ")); DEBUG_PRINTLN(_s.nodeId());
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
    _lastMdnsOkMs  = 0;
    _sockUp        = false;
    _seenFailMs    = 0;
    _lastResolveMs = 0;
    _inClient      = nullptr;
    _inId          = 0;

    _ws.onEvent([this](WStype_t t, uint8_t* p, size_t l) { onEvent(t, p, l); });
    // Library-level auto-reconnect handles the common case; the backoff bounds
    // come from the shared contract so the mock secondary can expect the same.
    // START AT THE MINIMUM AND BACK OFF FROM THERE — see _backoff() below.
    // This used to be the whole story, and kReconnectMaxMs was never read.
    _retryMs = nodelink::kReconnectMinMs;
    _ws.setReconnectInterval(_retryMs);
    _ws.enableHeartbeat(nodelink::kPingIntervalMs, nodelink::kPongTimeoutMs, 2);

    _running   = true;
    // A node that dials in gets no task until the grace has passed (update() starts it);
    // anything else is dialled at once, as it always was.
    if (!_s.dialsIn() && millis() >= _bootGraceUntilMs) ensureTask();
}

bool RemoteActuatorBus::ensureTask() {
    if (_taskAlive) return true;
    _retire    = false;
    _taskAlive = true;
    // Checked, because the failure is otherwise completely silent: no task means
    // nothing ever pumps _ws.loop(), so the socket never opens and the node sits
    // at "paired but offline" forever — indistinguishable from a dead board.
    BaseType_t ok = xTaskCreatePinnedToCore(taskTrampoline, "nodelink", kNodeLinkTaskStack,
                                            this, kNodeLinkTaskPrio, &_task, 0);
    if (ok != pdPASS) {
        _taskAlive = false;
        _task      = nullptr;
        DEBUG_PRINT(F("[NODE] FAILED to start link task for ")); DEBUG_PRINT(_s.nodeId());
        DEBUG_PRINT(F(" — free heap ")); DEBUG_PRINTLN(ESP.getFreeHeap());
        return false;
    }
    return true;
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
    DEBUG_PRINT(F("[NODE] backing off ")); DEBUG_PRINT(_s.nodeId());
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
        DEBUG_PRINT(F("[NODE] Linking to ")); DEBUG_PRINT(_s.nodeId());
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
        DEBUG_PRINT(F("[NODE] ⚠ link task for ")); DEBUG_PRINT(_s.nodeId());
        DEBUG_PRINTLN(F(" did not stop — deleting it; mDNS or a socket may now be leaked"));
        if (_task) vTaskDelete(_task);
        _taskAlive = false;
    }
    _task = nullptr;
    _ws.disconnect();
    _sockUp = false;
    // A node-initiated link belongs to this bus too: close the node's socket and forget
    // it. Left set (as it was until 2026-10-04), the bus restarted after a pause or an
    // unpair still believed it owned that socket — so it never dialled, and never
    // re-sent the HELLO the node was waiting for — while the node, whose socket was
    // still open, thought it was linked. Both ends were wrong, for as long as the
    // socket stayed open.
    {
        AsyncWebSocketClient* c = _inClient;
        _inClient = nullptr;
        _inId = 0;
        if (c && c->status() == WS_CONNECTED) c->close();
    }
    if (_mutex) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _s.clearLink();      // silent: there is nothing to report about a link we ended on purpose
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
    while (_running && !_retire) {
        // A node that dialled us IS the link. Leave the dial-out socket closed: a
        // second one to the same node only teaches it that "another link" exists, and
        // that is exactly the churn that fills a node's connection slots.
        if (_inId) {
            if (_sockUp || _ws.isConnected()) { _ws.disconnect(); _sockUp = false; }
            lastNagMs = millis();
            // Held for a few seconds: the node's own link is the link, and this task has
            // nothing left to do. Leave — the stack goes back to the heap — and let
            // update() start another if the node ever drops.
            if (_s.dialsIn() && (millis() - _inSinceMs) > 3000) { _retire = true; continue; }
            delay(50);
            continue;
        }
        // A node that dials in gets the first move. Dialling it back at the same
        // instant only makes two sockets race for one link (the loser is closed, but
        // both cost the node a connection slot), and the node's own attempt — which
        // works whichever direction the network drops — is the one meant to carry
        // the shop. After the grace we dial as well: this is the fallback, and it
        // is what keeps a node whose beacon, name and cached address all fail
        // reachable. `caps.join` is only known once the node has WELCOMEd us, so the
        // first link after a primary boot is dialled at once, as it always was.
        if (_s.dialsIn() && !_s.connected() && !_inId && _s.downSinceMs() &&
            (millis() - _s.downSinceMs()) < kDialInGraceMs) {
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
        if (!_sockUp && !_s.connected() && !_hostIsIp) {
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

        if (!_s.connected() && (millis() - lastNagMs) > 10000) {
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

        // Drain whatever the session wants sent (a SET, a CONFIG, an OTA order, a PRESS), and let it
        // time out a move whose STATE never arrived. Sending from HERE (not from setState()) is what
        // keeps the socket single-threaded.
        if (_mutex) {
            for (int i = 0; i < 4; i++) {
                std::string frame;
                bool got = false;
                if (xSemaphoreTake(_mutex, 0) == pdTRUE) { got = _s.nextFrame(frame); xSemaphoreGive(_mutex); flushSink(); }
                if (!got) break;
                _ws.sendTXT(frame.c_str());
            }
        }
        delay(5);
    }
    // LAST, and only here: end() reads this to know the task has let go of
    // everything it might have been holding.
    _taskAlive = false;
    vTaskDelete(NULL);
}

// ── node-initiated links ─────────────────────────────────────────────────────
bool RemoteActuatorBus::attachInbound(AsyncWebSocketClient* c, String& helloOut) {
    if (!_mutex || !c) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    // One healthy link only: a node is refused as a DUPLICATE rather than allowed to replace one.
    if (!_s.onAttach()) { xSemaphoreGive(_mutex); return false; }
    _inClient   = c;
    _inId       = c->id();
    _inSinceMs  = millis();
    _lastPingMs = millis();
    const std::string hello = _s.helloFrame();
    xSemaphoreGive(_mutex);
    flushSink();
    helloOut = hello.c_str();
    DEBUG_PRINT(F("[NODE] ")); DEBUG_PRINT(_s.nodeId()); DEBUG_PRINTLN(F(" dialled in"));
    return true;
}

void RemoteActuatorBus::detachInbound(uint32_t clientId) {
    if (!_inId || _inId != clientId) return;
    _inClient = nullptr;
    _inId     = 0;
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _s.onDown();
    xSemaphoreGive(_mutex);
    flushSink();
}

void RemoteActuatorBus::onInboundPong() {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _s.onPong();
    xSemaphoreGive(_mutex);
}

void RemoteActuatorBus::pumpInbound() {
    AsyncWebSocketClient* c = _inClient;
    if (!c || !_inId) return;
    if (c->status() != WS_CONNECTED) return;
    // The same drain as the dial-out task's, to the other transport.
    for (int i = 0; i < 4; i++) {
        std::string frame;
        if (xSemaphoreTake(_mutex, 0) != pdTRUE) return;      // try again next loop
        const bool got = _s.nextFrame(frame);
        xSemaphoreGive(_mutex);
        flushSink();
        if (!got) break;
        c->text(frame.c_str());
    }
}

void RemoteActuatorBus::update() {
    // THE SUPERVISOR for the dial-out task. A node that dials in is not given one while
    // it is linked (it was ~5 KB of stack per node, resident, for a link that is
    // somebody else's job). When the link is down and the node has had its grace to
    // dial in, start one: this is the fallback that dials it back.
    if (_running && !_taskAlive && !_inId) {
        const bool inGrace = (_s.dialsIn() && _s.downSinceMs() && (millis() - _s.downSinceMs()) < kDialInGraceMs) ||
                             (int32_t)(_bootGraceUntilMs - millis()) > 0;
        if (!inGrace) ensureTask();
    }
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
            std::string hello;
            if (_mutex) { xSemaphoreTake(_mutex, portMAX_DELAY); hello = _s.helloFrame(); xSemaphoreGive(_mutex); }
            _ws.sendTXT(hello.c_str());
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
            if (_mutex) {
                xSemaphoreTake(_mutex, portMAX_DELAY);
                if (hollow) _s.noteHollow();
                _s.onDown();
                xSemaphoreGive(_mutex);
                flushSink();
            }
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
                _s.onPong();
                xSemaphoreGive(_mutex);
            }
            break;

        default:
            break;
    }
}

void RemoteActuatorBus::handleFrame(const char* json, size_t len) {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const bool linkedNow = _s.onFrame(json, len);
    xSemaphoreGive(_mutex);
    flushSink();
    // A HEALTHY LINK RESETS THE BACKOFF, and an accepted WELCOME is the first moment we know it is
    // one — which is a transport matter, so it is done here and not in the session.
    if (linkedNow) {
        _retryMs = nodelink::kReconnectMinMs;
        _ws.setReconnectInterval(_retryMs);
    }
}

// ── the ActuatorBus seam: thin delegates, each under the lock ────────────────
// The mutex is created in begin(); before that every question has the answer "nothing".
bool RemoteActuatorBus::online() const {
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const bool r = _s.online();
    xSemaphoreGive(_mutex);
    flushSink();
    return r;
}
bool RemoteActuatorBus::busy() const {
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const bool r = _s.busy();
    xSemaphoreGive(_mutex);
    flushSink();
    return r;
}
bool RemoteActuatorBus::setState(const char* selectorId, JsonObjectConst sel, const char* stateId) {
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const bool r = _s.setState(selectorId, sel, stateId);
    xSemaphoreGive(_mutex);
    flushSink();
    return r;
}
bool RemoteActuatorBus::jog(int channel, int angle, bool detach) {
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const bool r = _s.jog(channel, angle, detach);
    xSemaphoreGive(_mutex);
    flushSink();
    return r;
}
bool RemoteActuatorBus::pressRf(uint8_t address, uint8_t data, uint32_t tickUs, uint32_t repeats) {
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const bool r = _s.pressRf(address, data, tickUs, repeats);
    xSemaphoreGive(_mutex);
    flushSink();
    return r;
}
void RemoteActuatorBus::configureSensors(JsonArrayConst sensors) {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _s.configureSensors(sensors);
    xSemaphoreGive(_mutex);
    flushSink();
}
bool RemoteActuatorBus::pollsPlugs() const { return _s.pollsPlugs(); }   // a caps flag: a lone int, written once per WELCOME
bool RemoteActuatorBus::canPressRf() const { return _s.canPressRf(); }
bool RemoteActuatorBus::watchesBin() const { return _s.watchesBin(); }
bool RemoteActuatorBus::plugReading(const char* sensorId, float& watts, bool& fault, uint32_t& atMs) const {
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const bool r = _s.plugReading(sensorId, watts, fault, atMs);
    xSemaphoreGive(_mutex);
    flushSink();
    return r;
}
bool RemoteActuatorBus::senseOf(const char* sensorId, bool& on, uint32_t& atMs) const {
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const bool r = _s.senseOf(sensorId, on, atMs);
    xSemaphoreGive(_mutex);
    flushSink();
    return r;
}
size_t RemoteActuatorBus::senseCount() const {
    if (!_mutex) return 0;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const size_t n = _s.senseCount();
    xSemaphoreGive(_mutex);
    return n;
}
bool RemoteActuatorBus::senseAt(size_t i, SenseView& v) const {
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const bool r = _s.senseAt(i, v);
    xSemaphoreGive(_mutex);
    flushSink();
    return r;
}

RemoteActuatorBus::LinkHealth RemoteActuatorBus::health() const {
    LinkHealth h{false, false, 0, UINT32_MAX, 0, nullptr};
    if (!_mutex) return h;
    const uint32_t now  = millis();
    const uint32_t mdns = _lastMdnsOkMs;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const NodeSession::Health sh = _s.health();
    xSemaphoreGive(_mutex);
    h.linked      = sh.linked;
    h.refused     = sh.refused;
    h.downForMs   = sh.downForMs;
    h.hollowDrops = sh.hollowDrops;
    h.moveFault   = sh.moveFault;
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
    n = _s.info();
    xSemaphoreGive(_mutex);
    return n;
}

bool RemoteActuatorBus::requestOta(const char* path, uint32_t size, const char* md5, const char* fw, const char*& why) {
    if (!_mutex) { why = "not started"; return false; }
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const bool r = _s.requestOta(path, size, md5, fw, why);
    xSemaphoreGive(_mutex);
    flushSink();
    return r;
}

void RemoteActuatorBus::noteOtaRefused(const char* why) {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _s.noteOtaRefused(why);
    xSemaphoreGive(_mutex);
    flushSink();
}

// ── the session's voice: console and link log ────────────────────────────────
RemoteActuatorBus::RemoteActuatorBus() : _s(clockMs, &_sink) { _sink.bus = this; }

void RemoteActuatorBus::Sink::say(const char* l) {
    if (nLine < kLines) { nodelink::strlcpy_(line[nLine++], l ? l : "", kLineLen); }
    else if (dropped < 255) dropped++;
}

void RemoteActuatorBus::Sink::linkEvent(const char* event, const char* extraJson) {
    if (nEv < kEvents) {
        nodelink::strlcpy_(evName[nEv],  event ? event : "", sizeof(evName[0]));
        nodelink::strlcpy_(evExtra[nEv], extraJson ? extraJson : "", sizeof(evExtra[0]));
        nEv++;
    } else if (dropped < 255) dropped++;
}

void RemoteActuatorBus::flushSink() const {
    if (!_mutex) return;
    for (;;) {
        char buf[Sink::kLineLen];
        bool got = false;
        xSemaphoreTake(_mutex, portMAX_DELAY);
        if (_sink.nLine) {
            nodelink::strlcpy_(buf, _sink.line[0], sizeof(buf));
            for (int i = 1; i < _sink.nLine; i++) memcpy(_sink.line[i - 1], _sink.line[i], Sink::kLineLen);
            _sink.nLine--;
            got = true;
        }
        xSemaphoreGive(_mutex);
        if (!got) break;
        DEBUG_PRINTLN(buf);
    }
    for (;;) {
        char name[16], extra[Sink::kExtraLen];
        bool got = false;
        xSemaphoreTake(_mutex, portMAX_DELAY);
        if (_sink.nEv) {
            nodelink::strlcpy_(name,  _sink.evName[0],  sizeof(name));
            nodelink::strlcpy_(extra, _sink.evExtra[0], sizeof(extra));
            for (int i = 1; i < _sink.nEv; i++) {
                memcpy(_sink.evName[i - 1],  _sink.evName[i],  sizeof(name));
                memcpy(_sink.evExtra[i - 1], _sink.evExtra[i], sizeof(extra));
            }
            _sink.nEv--;
            got = true;
        }
        xSemaphoreGive(_mutex);
        if (!got) break;
        linklog::event(name, _host, extra[0] ? extra : nullptr);
    }
}

} // namespace topo

// =============================================================================
// control/NodeSession.h — the primary's side of ONE node link, with no socket in it.
//
// WHY THIS EXISTS. RemoteActuatorBus used to be everything at once: the frame handling (what a
// WELCOME, ACK, STATE, SENSE, OTASTATE or PRESS answer MEANS), the bookkeeping (move timeouts,
// the cached CONFIG, link health, OTA progress), and the transport (a WebSocketsClient on its own
// task, a second transport for node-initiated links, mDNS resolution, backoff). The first two are
// the node protocol, and they are identical wherever the brain runs; the third is the platform.
// Mixed into one 1,100-line class, the portable part could not be built for a second platform,
// could not be host-tested at all (it had no tests, only a hardware soak), and its two transports
// each re-implemented the bookkeeping around it — which is how a stale inbound socket survived a
// pause on 2026-10-04.
//
// WHAT IT IS. A pure state machine. Frames in (`onFrame`), frames out (`nextFrame`), questions
// asked of it (`online`, `senseOf`, `health`...), and a clock it is GIVEN. It owns no sockets, no
// tasks and no mutex. THE SHELL SERIALISES ACCESS: on the ESP32 RemoteActuatorBus holds its mutex
// around every call, because the transport runs on another task; a single-threaded shell needs
// nothing. It reports what it would print or log through a SessionSink, so it needs no Serial and
// no flash.
//
// WHAT IT DOES NOT KNOW: how bytes reach the node, how the node is found, when to redial, or
// which task is running. That asymmetry is the whole point.
//
// PURE — STL + ArduinoJson, no Arduino.h, so the host tests drive it (test_nodesession.cpp).
// =============================================================================
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <ArduinoJson.h>
#include "ActuatorBus.h"
#include "NodeLink.h"
#include "SenseReport.h"

namespace topo {

// Where the session reports what it would otherwise print or write to the link log.
class SessionSink {
public:
    virtual ~SessionSink() {}
    virtual void say(const char* /*line*/) {}                                  // one console line
    virtual void linkEvent(const char* /*event*/, const char* /*extraJson*/) {} // utils/LinkLog.h
};

class NodeSession {
public:
    using Clock = uint32_t (*)();

    // ── what the Boards screen and the link log read ────────────────────────
    struct NodeInfo {
        bool     connected;
        uint32_t lastSeenMs;    // the clock's value at the last frame from this node
        char     board[24];
        char     fw[24];
        int      capServos;
        int      capLinear;
        int      capClamps;     // caps.ct — 0 for every board flashed before 2026-09-15
        // An update this node was told to run (OTA / OTASTATE): "" when none, else
        // start | progress | done | fail. `otaPct` is -1 until reported.
        char     ota[9];
        int      otaPct;
        char     otaErr[65];
    };

    // Link health, for the WiFi rejoin heuristic and the board-offline problem (see
    // RemoteActuatorBus::LinkHealth in the shell, which adds the one field a session cannot know —
    // how recently the node's NAME answered mDNS — before handing this out).
    struct Health {
        bool        linked;       // accepted WELCOME, link up and fresh
        bool        refused;      // the node said no — a claim question, not a path one
        uint32_t    downForMs;    // 0 while linked
        uint16_t    hollowDrops;
        // Why the LAST move did not finish cleanly, or nullptr. Cleared by the next
        // STATE(moving=false). Surfaced as a problem in the app.
        const char* moveFault;
    };

    NodeSession(Clock clock = nullptr, SessionSink* sink = nullptr) : _clock(clock), _sink(sink) {}
    void setClock(Clock c)         { _clock = c; }
    void setSink(SessionSink* s)   { _sink = s; }

    // Who this session is for. Resets the link-health counters but NOT what the node told us
    // about itself, when it is the same node (a pause and a resume): `caps.join` in particular is
    // what lets the primary give a node that dials in the first move. A different node must not
    // inherit any of it.
    void configure(const char* nodeId, const char* primaryId) {
        const bool same = std::strcmp(_nodeId, nodeId ? nodeId : "") == 0;
        if (!same) {
            _capJoin = _capRf = _capBin = _capPlugs = _capClamps = _capServos = _capLinear = 0;
            _board[0] = _fw[0] = '\0';
        }
        nodelink::strlcpy_(_nodeId,    nodeId    ? nodeId    : "", sizeof(_nodeId));
        nodelink::strlcpy_(_primaryId, primaryId ? primaryId : "", sizeof(_primaryId));
        _downSinceMs = now();    // down until a WELCOME says otherwise
        _hollowDrops = 0;
        _everLinked  = false;
    }
    const char* nodeId() const { return _nodeId; }

    // ── transport events ────────────────────────────────────────────────────

    // The HELLO that carries our claim. `_takeover` is one-shot and only ever set by an explicit
    // user action (requestTakeover), so a reconnect loop can never escalate itself into a theft.
    std::string helloFrame() {
        StaticJsonDocument<192> doc;
        const bool takeover = _takeover;
        _takeover = false;
        nodelink::buildHello(doc.to<JsonObject>(), _primaryId, _nodeId, takeover);
        std::string out;
        serializeJson(doc, out);
        return out;
    }
    void requestTakeover() { _takeover = true; }
    const char* refusedBy() const { return _refusedBy; }
    bool wasRefused() const       { return _refusedBy[0] != '\0'; }

    // A node-initiated socket has just attached. False when a link is already up and fresh — the
    // caller refuses the newcomer as a duplicate. Otherwise the half-open state of any earlier
    // transport is stale, and a PRESS is dropped: it is an EDGE against a TOGGLE, and one queued
    // before this moment must never be replayed after it.
    bool onAttach() {
        if (online()) return false;
        _lastRxMs     = now();    // the node just spoke; do not call it overdue before the WELCOME
        _connected    = false;
        _txPending    = false;
        _pressPending = false; _pressSeq = 0;
        return true;
    }

    // The transport went away. `wasUp` is true when a socket that had upgraded is the thing that
    // died (the sink says "Link lost" only for a link that was actually up).
    void onDown() {
        bool logDown = false;
        if (_connected) { _downSinceMs = now(); logDown = true; }
        _connected = false;
        // Drop any outstanding move: we can't know whether it landed, and holding busy() forever
        // would stall every other gate.
        if (_moveOutstanding) _moveFault = "The link dropped mid-move \xE2\x80\x94 the gate may not have finished moving.";
        _moveOutstanding = false;
        _txPending = false;
        // A PRESS is an EDGE against a TOGGLE: one queued during an outage and sent on reconnect
        // would switch the blower the wrong way. Never replayed.
        _pressPending = false; _pressSeq = 0;
        // FORGET THE READINGS, KEEP THE CONFIG. A link that has dropped tells us nothing about the
        // tool any more, and a stale "on" left lying here would keep a collector running for a
        // machine nobody can see (RFC §5.6a: absent is OFF). The CONFIG is the opposite — it is
        // ours, not the node's, and the node will have forgotten it across the reboot.
        _senseCount = 0;
        _cfgPending = _cfgValid;
        char line[96];
        std::snprintf(line, sizeof(line), "[NODE] Link lost: %s", _nodeId);
        say(line);
        if (logDown && _sink) _sink->linkEvent("link_down", "");
    }
    // A teardown the shell asked for (a pause, an unpair): forget the link, SILENTLY — there is
    // nothing to report about a link we ended on purpose.
    void clearLink() {
        _connected = false; _moveOutstanding = false; _txPending = false;
        _pressPending = false; _pressSeq = 0;
    }
    // A socket that OPENED and then died before the WebSocket upgrade — see Health::hollowDrops.
    void noteHollow() { if (_hollowDrops < 0xFFFF) _hollowDrops++; }
    // Heartbeat traffic counts as liveness. On an idle link — the NORMAL state, since a gate only
    // moves when a tool starts — the last TEXT frame is the WELCOME at connect time, so without
    // this online() would go false six seconds later on a perfectly healthy node.
    void onPong() { _lastRxMs = now(); }

    // Everything that arrives. Returns true when this frame is the one that brought the link UP
    // (an accepted WELCOME), so the shell can reset its transport backoff — which is a transport
    // detail the session does not own.
    bool onFrame(const char* json, size_t len) {
        // 768, and on the HEAP rather than the stack. A WELCOME carrying identity, seven capabilities and
        // boot info is ~16 members plus ~200 bytes of copied strings, which sailed past the old 384: an
        // ArduinoJson parse that does not fit FAILS (NoMemory), so a node that gained two capability flags
        // would have read as "no frame" — never linking, with nothing anywhere saying why. Caught by the
        // host test of this class. The link task's stack has ~1.3 KB to spare, so it is not spent here.
        DynamicJsonDocument doc(768);
        if (deserializeJson(doc, json, len)) return false;   // malformed → ignore
        JsonObjectConst f = doc.as<JsonObjectConst>();
        const char* t = f["t"].as<const char*>();
        if (!t) return false;
        _lastRxMs = now();

        if (std::strcmp(t, "WELCOME") == 0)  return onWelcome(f);
        if (std::strcmp(t, "ACK") == 0)      { onAck(f); return false; }
        if (std::strcmp(t, "SENSE") == 0)    { onSense(f); return false; }
        if (std::strcmp(t, "OTASTATE") == 0) { onOtaState(f); return false; }
        if (std::strcmp(t, "STATE") == 0)    { onState(f); return false; }
        return false;   // unknown frame — ignore rather than guess
    }

    // ── what the shell sends ────────────────────────────────────────────────
    // Moves a due timeout on, then hands out the next pending frame, if any: a SET, then a CONFIG,
    // then an OTA order, then a PRESS. Nothing goes out while the link is not up.
    bool nextFrame(std::string& out) {
        tick();
        if (!_connected) return false;
        if (_txPending)    { out = _txFrame;    _txPending    = false; return true; }
        if (_cfgPending)   { out = _cfgFrame;   _cfgPending   = false; say(label("[NODE→] CONFIG to ")); return true; }
        if (_otaPending)   { out = _otaFrame;   _otaPending   = false; say(label("[NODE→] OTA to "));    return true; }
        if (_pressPending) { out = _pressFrame; _pressPending = false; return true; }
        return false;
    }
    // A move whose STATE report never arrived: give up rather than let the primary's move queue
    // block forever behind a lost frame.
    void tick() {
        if (_moveOutstanding && (now() - _moveStartedMs) > nodelink::kMoveTimeoutMs) {
            _moveOutstanding = false;
            _moveFault = "The board never reported its move finished (timed out).";
            say(label("[NODE] Move timed out on "));
        }
    }

    // ── ActuatorBus, as the session answers it ──────────────────────────────
    bool online() const { return _connected && (now() - _lastRxMs) < nodelink::kPongTimeoutMs; }
    bool busy()   const { return _moveOutstanding || _txPending; }
    // The raw "accepted WELCOME, and the transport has not dropped" flag — NOT freshness (that is
    // online()). The shell's redial logic asks whether a link exists at all, not whether it is
    // chatty.
    bool connected() const { return _connected; }
    uint32_t downSinceMs() const { return _downSinceMs; }

    bool setState(const char* selectorId, JsonObjectConst sel, const char* stateId) {
        if (!online()) return false;
        // Resolve to a concrete angle / mm HERE, on the primary. The secondary gets a number, never
        // a state name it would have to interpret — see NodeLink.h.
        StaticJsonDocument<320> doc;
        if (!nodelink::buildSetFrame(doc.to<JsonObject>(), ++_seq, selectorId, sel, stateId)) {
            return false;   // uncalibrated — refuse rather than send a guess
        }
        std::string s; serializeJson(doc, s);
        if (s.size() >= sizeof(_txFrame)) return false;
        nodelink::strlcpy_(_txFrame, s.c_str(), sizeof(_txFrame));
        _txPending       = true;
        _moveOutstanding = true;
        _moveStartedMs   = now();
        // The whole frame, not a summary. When a gate doesn't move, the question is always "which of
        // us dropped it" — this line and the node's matching one answer it in one comparison.
        say((std::string("[NODE→] ") + _nodeId + " " + s).c_str());
        return true;
    }

    bool jog(int channel, int angle, bool detach) {
        // A detach has no counterpart on the wire and needs none: holdAtRest is false on a jog, so
        // the node's ServoActuator de-energises on its own once the sweep settles. Reported as
        // HANDLED rather than refused — the caller asked for a de-energised servo and that is what
        // it gets.
        if (detach) return true;
        if (!online()) return false;
        if (channel < 0 || channel > 15 || angle < 0 || angle > 180) return false;
        // Hand-built rather than routed through buildSetFrame(): that resolves a stateId against a
        // selector's calibration, and a jog is what you do BEFORE there is any calibration.
        StaticJsonDocument<256> doc;
        JsonObject o = doc.to<JsonObject>();
        o["t"]          = "SET";
        o["seq"]        = ++_seq;
        o["selectorId"] = "__jog";
        o["stateId"]    = "__jog";
        o["drive"]      = "servo";
        o["channel"]    = channel;
        o["angle"]      = angle;
        o["holdAtRest"] = false;
        std::string s; serializeJson(doc, s);
        if (s.size() >= sizeof(_txFrame)) return false;
        nodelink::strlcpy_(_txFrame, s.c_str(), sizeof(_txFrame));
        _txPending = true;
        // Deliberately NOT setting _moveOutstanding: a jog is a setup-time nudge, not a routed move.
        // Marking the bus busy() would stall the move queue behind a gate someone is calibrating.
        return true;
    }

    // Ask the board to key its transmitter ONCE. Queued like a SET: true means the frame was
    // handed over (the board is linked and has a transmitter), NOT that the blower changed state.
    bool pressRf(uint8_t address, uint8_t data, uint32_t tickUs, uint32_t repeats) {
        if (!online() || _capRf <= 0) return false;
        StaticJsonDocument<192> doc;
        const uint32_t seq = ++_seq;
        nodelink::buildPress(doc.to<JsonObject>(), seq, address, data, tickUs, repeats);
        char buf[sizeof(_pressFrame)];
        if (serializeJson(doc, buf, sizeof(buf)) >= sizeof(buf)) return false;
        nodelink::strlcpy_(_pressFrame, buf, sizeof(_pressFrame));
        _pressSeq     = seq;
        _pressPending = true;
        say(label("[NODE→] PRESS to "));
        return true;
    }
    const char* pressFault() const { return _pressFault; }

    // Built here rather than by the caller so the WIRE SHAPE lives in one place — nodelink.js's
    // CONFIG, mirrored by parseConfigFrame() on the node.
    // 1024 since plug sensors (2026-10-03): four of them are four objects of five members, and
    // overflow DROPS members silently.
    void configureSensors(JsonArrayConst sensors) {
        StaticJsonDocument<1024> doc;
        JsonObject f = doc.to<JsonObject>();
        f["t"] = "CONFIG";
        JsonArray arr = f.createNestedArray("sensors");
        for (JsonObjectConst sen : sensors) {
            JsonObject o = arr.createNestedObject();
            o["sensorId"] = sen["sensorId"] | "";
            if (std::strcmp(sen["kind"] | "ct", "plug") == 0) {
                o["kind"]       = "plug";
                o["ip"]         = sen["ip"] | "";
                o["plug"]       = sen["plug"] | "shelly";
                o["thresholdW"] = sen["thresholdW"] | 0.0f;
            } else if (std::strcmp(sen["kind"] | "ct", "bin") == 0) {
                o["kind"]   = "bin";
                if (sen.containsKey("invert")) o["invert"] = sen["invert"] | true;
            } else {
                o["kind"]     = "ct";
                o["channel"]  = sen["channel"] | 0;
                // The CT tuning rides the wire only when the primary sent it (OMITTED, not zeroed:
                // an absent key means "keep your own").
                for (const char* k : {"tripRatio", "minCounts", "clearRatio"})
                    if (sen.containsKey(k)) o[k] = sen[k].as<float>();
            }
        }
        f["seq"] = ++_seq;
        std::string s; serializeJson(doc, s);
        if (s.size() >= sizeof(_cfgFrame)) {
            say(label("[NODE] CONFIG too large for "));
            return;
        }
        nodelink::strlcpy_(_cfgFrame, s.c_str(), sizeof(_cfgFrame));
        _cfgValid   = true;
        _cfgPending = true;
        // Readings from the OLD configuration are not readings under the new one: a sensorId that
        // was just removed must stop answering immediately rather than keep a tool switched on
        // until it ages out.
        _senseCount = 0;
    }

    bool pollsPlugs() const { return _capPlugs > 0; }
    bool canPressRf() const { return _capRf > 0; }
    bool watchesBin() const { return _capBin > 0; }
    bool dialsIn()    const { return _capJoin > 0; }

    bool plugReading(const char* sensorId, float& watts, bool& fault, uint32_t& atMs) const {
        if (!sensorId || !*sensorId) return false;
        for (size_t i = 0; i < _senseCount; i++) {
            if (std::strcmp(_senses[i].sensorId, sensorId) != 0) continue;
            if (_senses[i].atMs && _senses[i].isPlug) {
                watts = _senses[i].watts < 0.0f ? 0.0f : _senses[i].watts;
                fault = _senses[i].fault;
                atMs  = _senses[i].atMs;
                return true;
            }
            break;
        }
        return false;
    }
    bool senseOf(const char* sensorId, bool& on, uint32_t& atMs) const {
        if (!sensorId || !*sensorId) return false;
        for (size_t i = 0; i < _senseCount; i++) {
            if (std::strcmp(_senses[i].sensorId, sensorId) != 0) continue;
            // atMs == 0 means the slot exists but nothing has landed in it.
            if (_senses[i].atMs) { on = _senses[i].on; atMs = _senses[i].atMs; return true; }
            break;
        }
        return false;
    }
    size_t senseCount() const { return _senseCount; }
    bool senseAt(size_t i, SenseView& v) const {
        if (i >= _senseCount) return false;
        // Points into _senses[], which outlives the call — see SenseView.
        v.id = _senses[i].sensorId;
        // CONFIGURED BUT NEVER HEARD FROM is the state this endpoint exists to make visible, and it
        // is NOT the same as "off".
        v.reported = _senses[i].atMs != 0;
        v.on       = _senses[i].on;
        // AGE, not the raw timestamp: this becomes a JSON body a phone reads, and the board's clock
        // means nothing at the other end.
        v.ageMs    = v.reported ? (uint32_t)(now() - _senses[i].atMs) : 0;
        v.level    = _senses[i].level;
        v.amps     = _senses[i].amps;
        v.floorA   = _senses[i].floorA;
        v.tripA    = _senses[i].tripA;
        v.fault    = _senses[i].fault;
        v.isPlug   = _senses[i].isPlug;
        v.watts    = _senses[i].watts;
        return true;
    }

    Health health() const {
        Health h{false, false, 0, 0, nullptr};
        const uint32_t n = now();
        h.linked      = online();
        h.refused     = _refusedBy[0] != '\0';
        h.downForMs   = (h.linked || !_downSinceMs) ? 0 : (n - _downSinceMs);
        h.hollowDrops = _hollowDrops;
        h.moveFault   = _moveFault;
        return h;
    }

    NodeInfo info() const {
        NodeInfo n;
        n.connected  = online();
        n.lastSeenMs = _lastRxMs;
        nodelink::strlcpy_(n.board, _board, sizeof(n.board));
        nodelink::strlcpy_(n.fw,    _fw,    sizeof(n.fw));
        n.capServos = _capServos;
        n.capLinear = _capLinear;
        n.capClamps = _capClamps;
        nodelink::strlcpy_(n.ota,    _otaState, sizeof(n.ota));
        n.otaPct = _otaPct;
        nodelink::strlcpy_(n.otaErr, _otaErr,   sizeof(n.otaErr));
        // An update that stopped reporting is a failure, not a progress bar that never moves — most
        // likely a node whose firmware predates OTA, which ignores the frame (an unknown frame is
        // ignored, not refused; see nodelink.js).
        if (otaRunning() && now() - _otaTouchedMs > 30000UL) {
            nodelink::strlcpy_(n.ota, "fail", sizeof(n.ota));
            nodelink::strlcpy_(n.otaErr, "no answer - this board's firmware predates updates", sizeof(n.otaErr));
            n.otaPct = -1;
        }
        return n;
    }

    // Tell this node to pull and install an image. False (and why) when the node is not linked or
    // is already updating. The node does its own refusing for everything it alone can know (a gate
    // moving, no second slot) and answers in OTASTATE.
    bool requestOta(const char* path, uint32_t size, const char* md5, const char* fw, const char*& why) {
        const bool up   = online();
        const bool busy = (otaRunning() && now() - _otaTouchedMs < 30000UL) || _otaPending;
        if (!up)   { why = "the board is offline"; return false; }
        if (busy)  { why = "an update is already running"; return false; }
        StaticJsonDocument<384> d;
        nodelink::buildOta(d.to<JsonObject>(), ++_otaSeq, path, size, md5, fw);
        if (serializeJson(d, _otaFrame, sizeof(_otaFrame)) >= sizeof(_otaFrame)) { why = "frame too long"; return false; }
        _otaPending   = true;
        _otaTouchedMs = now();
        nodelink::strlcpy_(_otaState, "start", sizeof(_otaState));
        _otaPct = 0;
        _otaErr[0] = '\0';
        return true;
    }
    // Record a refusal made on the PRIMARY's side (a tool running, no image staged) so the Boards
    // screen has one place to read "why not" from.
    void noteOtaRefused(const char* why) {
        nodelink::strlcpy_(_otaState, "fail", sizeof(_otaState));
        _otaPct = -1;
        nodelink::strlcpy_(_otaErr, why ? why : "refused", sizeof(_otaErr));
    }

private:
    uint32_t now() const { return _clock ? _clock() : 0; }
    void say(const char* line) const { if (_sink) _sink->say(line); }
    const char* label(const char* prefix) const {
        std::snprintf(_lbl, sizeof(_lbl), "%s%s", prefix, _nodeId);
        return _lbl;
    }
    bool otaRunning() const { return std::strcmp(_otaState, "start") == 0 || std::strcmp(_otaState, "progress") == 0; }

    bool onWelcome(JsonObjectConst f) {
        // Refuse a node speaking a different protocol version rather than half-understanding it. It
        // stays offline and its gates unreachable.
        if ((f["v"] | 0) != nodelink::kVersion) {
            _connected = false;
            say(label("[NODE] Version mismatch from "));
            return false;
        }
        nodelink::strlcpy_(_board, f["board"] | "", sizeof(_board));
        nodelink::strlcpy_(_fw,    f["fw"]    | "", sizeof(_fw));
        _capServos = f["caps"]["servos"] | 0;
        _capLinear = f["caps"]["linear"] | 0;
        // Absent means none: a board flashed before clamps existed answers exactly as it always did
        // rather than being read as broken.
        _capClamps = f["caps"]["ct"] | 0;
        // Absent means NO: a node that predates plug polling must stay brain-polled, or it is handed
        // a CONFIG it refuses whole — clamp and all.
        _capPlugs  = f["caps"]["plug"] | 0;
        // Absent means NO: a board that predates node-initiated links is dialled.
        _capJoin   = f["caps"]["join"] | 0;
        // Absent means NO: never send a PRESS or a `bin` sensor to a board that did not say it has one.
        _capRf     = f["caps"]["rf"]  | 0;
        _capBin    = f["caps"]["bin"] | 0;

        // Did it accept our claim? A refusal leaves us OFFLINE rather than half-connected: every
        // caller already treats offline as "don't command this board", which is exactly the required
        // behaviour, and the socket stays open so the user can be told who owns it.
        if (!nodelink::welcomeAccepted(f)) {
            nodelink::strlcpy_(_refusedBy, f["claimedBy"] | "another primary", sizeof(_refusedBy));
            _connected = false;
            if (_sink) {
                char safe[40], extra[80];
                safeCopy(safe, sizeof(safe), _refusedBy);
                std::snprintf(extra, sizeof(extra), "\"owner\":\"%s\"", safe);
                _sink->linkEvent("refused", extra);
            }
            char line[160];
            std::snprintf(line, sizeof(line), "[NODE] %s REFUSED us — it belongs to %s\n"
                          "       Take it over from the boards screen if that is what you want.", _nodeId, _refusedBy);
            say(line);
            return false;
        }
        _refusedBy[0] = '\0';
        // A node that has come back has, by definition, finished (or abandoned) whatever update it
        // was running — the fw in this WELCOME is the verdict. A refusal ("fail") is kept: nothing
        // about the node changed.
        if (std::strcmp(_otaState, "fail") != 0) { _otaState[0] = '\0'; _otaPct = -1; _otaErr[0] = '\0'; }
        // LINK LOG: how long it was down and how it looked while it was, taken BEFORE the reset
        // below, plus the node's own account of its boot (withBootInfo) — an upS shorter than the
        // outage means the NODE rebooted, and `rst` says whether that was power or a crash.
        const uint32_t upDownMs  = _downSinceMs ? (now() - _downSinceMs) : 0;
        const uint16_t upHollow  = _hollowDrops;
        const bool     upFirst   = !_everLinked;
        const long     upNodeUpS = f.containsKey("upS") ? (long)(f["upS"] | 0UL) : -1L;
        char upRst[nodelink::kMaxRstLen + 1];
        safeCopy(upRst, sizeof(upRst), f["rst"] | "");
        _everLinked  = true;
        _connected   = true;
        _downSinceMs = 0;
        _hollowDrops = 0;
        // Re-arm the CONFIG on every accepted handshake: this node may have just rebooted, and a node
        // that has not been configured reports nothing.
        if (_cfgValid) _cfgPending = true;
        if (_sink) {
            char extra[160];
            std::snprintf(extra, sizeof(extra),
                          "\"downMs\":%lu,\"hollow\":%u,\"first\":%s,\"nodeUpS\":%ld,\"nodeRst\":\"%s\"",
                          (unsigned long)upDownMs, (unsigned)upHollow, upFirst ? "true" : "false", upNodeUpS, upRst);
            _sink->linkEvent("link_up", extra);
        }
        return true;
    }

    void onAck(JsonObjectConst f) {
        const bool ok = f["ok"] | false;
        // A PRESS is answered with an ACK on the same seq. It is not a move: it must not touch the
        // move bookkeeping below, and its failure is its own fault string.
        if (_pressSeq && (f["seq"] | 0u) == _pressSeq) {
            nodelink::strlcpy_(_pressFault, ok ? "" : (f["err"] | "the board refused the press"), sizeof(_pressFault));
            _pressSeq = 0;
            say(ok ? "[NODE←] PRESS ok" : "[NODE←] PRESS REFUSED");
            return;
        }
        if (!ok) {
            if (_moveOutstanding) _moveFault = "The board refused the move.";
            _moveOutstanding = false;                       // refused → stop waiting
        }
        char line[160];
        std::snprintf(line, sizeof(line), "[NODE←] ACK seq=%lu%s%s", (unsigned long)(f["seq"] | 0u),
                      ok ? " ok" : " REFUSED: ", ok ? "" : (f["err"] | "(no reason given)"));
        say(line);
    }

    void onSense(JsonObjectConst f) {
        const char* sid = f["sensorId"].as<const char*>();
        const bool  on  = f["on"] | false;
        if (!sid || !*sid) return;
        size_t i = 0;
        for (; i < _senseCount; i++) if (std::strcmp(_senses[i].sensorId, sid) == 0) break;
        // A node reporting more sensors than it was configured for is a node out of step with us;
        // keep the ones we know and drop the rest rather than growing past the array.
        if (i == _senseCount && _senseCount < nodelink::kMaxSensorsPerNode) {
            nodelink::strlcpy_(_senses[i].sensorId, sid, sizeof(_senses[i].sensorId));
            _senseCount++;
        }
        if (!(i < nodelink::kMaxSensorsPerNode && i < _senseCount)) return;
        const bool changed = (_senses[i].on != on) || _senses[i].atMs == 0;
        _senses[i].on    = on;
        _senses[i].atMs  = now();
        _senses[i].level = f["level"] | -1.0f;
        // Telemetry for the UI. Absent stays NEGATIVE rather than becoming 0, because 0 A is a real
        // reading from an idle tool and "the node did not say" is not. Nothing branches on these.
        _senses[i].amps   = f["amps"]   | -1.0f;
        _senses[i].floorA = f["floorA"] | -1.0f;
        _senses[i].tripA  = f["tripA"]  | -1.0f;
        _senses[i].fault  = f["fault"]  | false;
        _senses[i].isPlug = f["plug"]   | false;
        _senses[i].watts  = f["watts"]  | -1.0f;
        // Logged on CHANGE only: this frame repeats every kSenseRepeatMs, and a line per repeat would
        // bury everything else on the console within a minute.
        if (changed) {
            char line[96];
            std::snprintf(line, sizeof(line), "[NODE←] SENSE %s %s", sid, on ? "ON" : "off");
            say(line);
        }
    }

    void onOtaState(JsonObjectConst f) {
        const char* st = f["state"] | "";
        nodelink::strlcpy_(_otaState, st, sizeof(_otaState));
        _otaTouchedMs = now();
        _otaPct = f.containsKey("pct") ? (int)(f["pct"] | 0) : _otaPct;
        nodelink::strlcpy_(_otaErr, f["err"] | "", sizeof(_otaErr));
        char line[96];
        std::snprintf(line, sizeof(line), "[NODE←] OTA %s %s %d", _nodeId, st, _otaPct);
        say(line);
        // A start/done/fail is an event; progress is noise.
        if (_sink && std::strcmp(st, "progress") != 0) {
            char extra[120], safe[48];
            safeCopy(safe, sizeof(safe), _otaErr);
            std::snprintf(extra, sizeof(extra), "\"state\":\"%s\",\"pct\":%d,\"err\":\"%s\"", _otaState, _otaPct, safe);
            _sink->linkEvent("ota", extra);
        }
    }

    void onState(JsonObjectConst f) {
        const bool moving = f["moving"] | false;
        if (!moving) { _moveOutstanding = false; _moveFault = nullptr; }
        char line[128];
        std::snprintf(line, sizeof(line), "[NODE←] STATE %s -> %s (%s)", (const char*)(f["selectorId"] | "?"),
                      (const char*)(f["stateId"] | "?"), moving ? "moving" : "arrived");
        say(line);
    }

    // Free text that goes into a JSON string in the link log: quotes and control characters out.
    static void safeCopy(char* out, size_t cap, const char* in) {
        size_t n = 0;
        for (; in && *in && n + 1 < cap; in++) {
            const unsigned char c = (unsigned char)*in;
            out[n++] = (c < 0x20 || c == '"' || c == '\\') ? '_' : (char)c;
        }
        out[n] = '\0';
    }

    Clock        _clock;
    SessionSink* _sink;
    mutable char _lbl[96] = "";

    char     _nodeId[40]    = "";
    char     _primaryId[40] = "";

    bool     _connected    = false;   // an accepted WELCOME, until the transport drops
    uint32_t _lastRxMs     = 0;       // any frame or pong; drives the PONG timeout
    uint32_t _seq          = 0;
    bool     _moveOutstanding = false;
    const char* _moveFault    = nullptr;   // static string; see Health::moveFault
    uint32_t _moveStartedMs   = 0;
    char     _txFrame[320]    = "";   // one pending SET
    bool     _txPending       = false;
    char     _refusedBy[40]   = "";   // set from a WELCOME carrying accepted:false
    bool     _takeover        = false;   // one-shot, user-confirmed

    // The CONFIG is CACHED, not just sent, and that is the load-bearing part: the board this was built
    // for is powered from the tool it watches (RFC §5.6a), so it reboots every time someone switches
    // the planer off at the wall. A configuration sent once at adopt would be forgotten on the first
    // power cut. Re-sent on every accepted WELCOME instead, which costs one small frame per reconnect.
    char     _cfgFrame[768]   = "";
    bool     _cfgPending      = false;
    bool     _cfgValid        = false;   // have we ever been given one?
    // OTA order. See requestOta().
    bool     _otaPending      = false;
    char     _otaFrame[256]   = "";
    uint32_t _otaSeq          = 0;
    char     _otaState[9]     = "";
    int      _otaPct          = -1;
    char     _otaErr[65]      = "";
    uint32_t _otaTouchedMs    = 0;   // last time the order or a report moved the state — a silent node is not 'updating' for ever
    // One pending PRESS, sent like a SET. A PRESS is an EDGE against a TOGGLE: never replayed.
    char     _pressFrame[160] = "";
    bool     _pressPending    = false;
    uint32_t _pressSeq        = 0;
    char     _pressFault[64]  = "";

    struct SenseState {
        char     sensorId[nodelink::kMaxSensorIdLen] = "";
        bool     on           = false;
        uint32_t atMs         = 0;
        // Multiple of the node's trip point, straight off the wire. DIAGNOSTIC ONLY — nothing routes on
        // it — and kept because it is the one number that answers "is this clamp nearly tripping?"
        float    level        = -1.0f;
        // Telemetry for a human, in AMPS. Negative = the node omitted it. NOTHING BRANCHES ON THESE.
        float    amps         = -1.0f;
        float    floorA       = -1.0f;
        float    tripA        = -1.0f;
        bool     fault        = false;   // the node could not learn a floor (a plug: did not answer)
        bool     isPlug       = false;   // reported by a plug sensor, not a clamp
        float    watts        = -1.0f;   // a plug's reading; negative = none
    };
    SenseState _senses[nodelink::kMaxSensorsPerNode];
    size_t     _senseCount = 0;

    // Link health.
    uint32_t _downSinceMs = 0;
    uint16_t _hollowDrops = 0;
    bool     _everLinked  = false;   // first link_up of this configure() is marked "first"

    char     _board[24]  = "";
    char     _fw[24]     = "";
    int      _capServos  = 0;
    int      _capLinear  = 0;
    int      _capClamps  = 0;   // caps.ct
    int      _capPlugs   = 0;   // caps.plug — 1 if it polls plugs for us; absent = 0
    int      _capJoin    = 0;   // caps.join — 1 if it dials us itself
    int      _capRf      = 0;   // caps.rf   — 1 if it has a transmitter for the collector's remote
    int      _capBin     = 0;   // caps.bin  — 1 if it has a dust-bin pad
};

}  // namespace topo

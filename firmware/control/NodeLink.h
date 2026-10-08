// =============================================================================
// NodeLink.h — C++ side of the primary↔secondary node protocol.
//
// Mirrors shared/device-model/nodelink.js frame-for-frame. Read that file first;
// it carries the rationale. The short version:
//
//   The primary RESOLVES every state into a concrete realization before it goes
//   on the wire — a SET carries `angle` (already referenceAngle + offsetDeg,
//   clamped) or `positionMm`, never "put gate3 in the open state". A secondary
//   needs a channel and a number, not a topology. That is what makes a cheap
//   servo-only node possible, and it means a schema change never has to be
//   rolled out to every board in the shop.
//
// buildSetFrame() is the single place that resolution happens, and it reuses
// topo::servoCommandAngle() so the wire value can't disagree with what a local
// servo would have been given for the same state.
//
// PURE — ArduinoJson + STL only, NO Arduino.h. Both ends include this.
// =============================================================================

#pragma once
#include <ArduinoJson.h>
#include "TopologyRouter.h"   // topo::servoCommandAngle, topo::_eq
#include <cstdint>
#include <cstring>
#include <string>

namespace topo {
namespace nodelink {

static const int kVersion = 1;   // NODELINK_VERSION in nodelink.js

static const unsigned long kPingIntervalMs  = 2000;
static const unsigned long kPongTimeoutMs   = 6000;
static const unsigned long kReconnectMinMs  = 1000;
static const unsigned long kReconnectMaxMs  = 15000;

// How often a node REPEATS its current sensor reading, and how long the primary
// waits before calling that reading stale. SENSE_REPEAT_MS / SENSE_STALE_MS in
// nodelink.js — asserted literally on both sides.
//
// SENSE is sent on every CHANGE; that is what makes a tool switching on a
// sub-second event. The repeat only stops one dropped frame leaving the primary
// permanently wrong, which an edge-only protocol would. Same 3x ratio as
// PING/PONG, for the same reason.
static const unsigned long kSenseRepeatMs   = 5000;
static const unsigned long kSenseStaleMs    = 15000;

// Sensors one node will accept in a CONFIG. MAX_SENSORS_PER_NODE in nodelink.js
// — a PAIR, because this side parses into a fixed array and a primary that sent
// more to a board that kept four would leave it silently deaf to the rest.
static const size_t kMaxSensorsPerNode = 4;

// Bounds on a PLUG sensor — kMaxPlugThresholdW / kMaxPlugWatts / kMaxPlugIpLen are
// MAX_PLUG_THRESHOLD_W / MAX_PLUG_WATTS / MAX_PLUG_IP_LEN in nodelink.js, a PAIR
// asserted literally on both sides. A node refuses an out-of-range CONFIG WHOLE
// (its clamp with it), so the bounds must agree exactly.
static const float  kMaxPlugThresholdW = 10000.0f;
static const float  kMaxPlugWatts      = 20000.0f;
static const size_t kMaxPlugIpLen      = 15;

// The bounds on an OTA frame. MAX_OTA_PATH / MIN_OTA_BYTES / MAX_OTA_BYTES in
// nodelink.js — a PAIR, asserted literally on both sides. parseOtaFrame refuses
// outside them WHOLE: a primary sending a longer path than a node would parse
// would read as a node that never starts updating, with nothing on the wire to
// say why. MIN is "not a 404 page"; MAX is the node's app slot (1.9 MiB, see
// partitions-xiao-c5-node.csv), so a node says no before it has erased anything.
static const size_t   kMaxOtaPath  = 48;
static const uint32_t kMinOtaBytes = 100u * 1024u;
static const uint32_t kMaxOtaBytes = 0x1E0000u;

// Node-initiated links (JOIN / REFUSE / WHERE). MAX_WHERE_IP_LEN and REFUSE_REASONS in
// nodelink.js — a PAIR, asserted literally on both sides. parseWhereFrame refuses a
// bad frame WHOLE, and a node that dropped a WHERE it should have acted on would
// read as a node that never comes looking for its primary.
static const size_t kMaxWhereIpLen = 15;
// The UDP port a primary broadcasts "DGB1|<primaryId>|<ip>|<port>" on, for a node that
// has lost it. BEACON_PORT in nodelink.js — a pair. Nothing but this number and the
// "DGB1" prefix has to agree, which is exactly why it is written down twice.
static const unsigned short kBeaconPort = 41234;

// The collector's jobs on a node (PRESS, a `bin` sensor, caps.rf, caps.bin).
// MIN_RF_TICK_US / MAX_RF_TICK_US / MAX_RF_REPEATS in nodelink.js — a PAIR, asserted
// literally on both sides. parsePressFrame refuses outside them WHOLE, and a refused
// PRESS reads as a collector that never starts, so the bounds must agree exactly.
static const uint32_t kMinRfTickUs = 50;
static const uint32_t kMaxRfTickUs = 1000;
static const uint32_t kMaxRfRepeats = 60;
static const char* const kRefuseReasons[] = { "not-paired", "duplicate", "busy" };
static const size_t kRefuseReasonCount = 3;

// A move that takes longer than this without a STATE(moving=false) is assumed
// lost rather than left to wedge the move queue forever. Generously longer than
// SERVO_SWEEP_MS + SERVO_HOLD_MS, and longer than a full-span rack traverse.
//
// RAISED FROM 12s TO 90s ON 2026-08-28, because the second half of that sentence
// stopped being true. It was written against the stepper at 19mm/s; the ST3215
// slider crosses an 8-gate 2.5" span (582mm) at ~42mm/s in 14s, so 12s declared
// a perfectly healthy traverse lost. Worse, a slider NODE defers the move it was
// sent until a sweep finds the datum — and since 2026-09-03 that sweep is
// triggered BY the move (on-demand homing), so the primary's clock is running
// for the whole of it (see the CALIBRATION note in node/dustgate_node.cpp).
//
// RAISED 90s → 210s ON 2026-09-05, and it is not a comfort margin. The node's
// own homing timeout is derived in config.h as HOMING_TIMEOUT_MS — ~162 s, sized
// for the longest rack the design admits (8 gates on the 4" manifold, 891 mm).
// At 90 s the primary gave up while the node was still legitimately sweeping,
// so a 4" rack would have reported every first move as lost. 210 s covers that
// sweep plus a full-length traverse afterwards (~21 s at tracked top speed).
//
// THE INVARIANT: this must exceed HOMING_TIMEOUT_MS, or the node can never be
// the one to report a failed home — the primary would always call it first, and
// the node's far more specific diagnosis (switch never fired vs. carriage stuck)
// would never reach anyone. Asserted in node/dustgate_node.cpp, which is the one
// translation unit that sees both numbers; this header is PURE and does not
// include config.h.
//
// A timeout this long is only tolerable because it is not how a move normally
// ends: arrival is a STATE frame, and this fires only when one never comes. It
// is the "the node stopped answering" backstop, and sizing it for the slowest
// legitimate case is the whole job.
//
// C++-ONLY, despite living in the file that mirrors nodelink.js frame for frame:
// there is no MOVE_TIMEOUT_MS on the JS side, because the timeout is the
// primary's own bookkeeping and never goes on the wire. Not a pair — nothing to
// keep in step, and no row in CLAUDE.md's table.
static const unsigned long kMoveTimeoutMs   = 210000;

// -----------------------------------------------------------------------------
// Primary → secondary
// -----------------------------------------------------------------------------

// HELLO — and, with it, a CLAIM. A node belongs to ONE primary; `primaryId` is
// both our identity and our claim on the board (nodelink.js hello()).
//
// `takeover` is a USER-CONFIRMED demand to take the node from its current
// owner, and must never be set automatically: a primary that retried with
// takeover after a refusal would reduce the claim to "whoever asks twice".
inline void buildHello(JsonObject out, const char* primaryId, const char* nodeId,
                       bool takeover = false) {
    out["t"] = "HELLO";
    out["v"] = kVersion;
    out["primaryId"] = primaryId;
    out["nodeId"]    = nodeId;
    if (takeover) out["takeover"] = true;
}


// REFUSE — the primary declining a node-initiated socket. It closes right after, so
// the node reads the reason and goes back to waiting rather than hammering.
inline void buildRefuse(JsonObject out, const char* reason) {
    out["t"]      = "REFUSE";
    out["reason"] = reason;
}

// PRESS — key the node's transmitter ONCE (nodelink.js press()). A primitive: when to
// press and whether it worked are the primary's policy (control/CollectorPress.h).
inline void buildPress(JsonObject out, uint32_t seq, uint8_t address, uint8_t data,
                       uint32_t tickUs, uint32_t repeats) {
    out["t"]       = "PRESS";
    out["seq"]     = seq;
    out["address"] = address;
    out["data"]    = data;
    out["tickUs"]  = tickUs;
    out["repeats"] = repeats;
}

// WHERE — "the primary <primaryId> is at <ip>:<port> now". Sent on a short-lived
// connection to a node's own listener (nodelink.js where()).
inline void buildWhere(JsonObject out, const char* primaryId, const char* ip, int port = 80) {
    out["t"]         = "WHERE";
    out["primaryId"] = primaryId;
    out["ip"]        = ip;
    out["port"]      = port;
}

// Resolve `sel` + `stateId` into a wire-ready SET. Returns false when the
// selector CANNOT be resolved — an uncalibrated servo (no referenceAngle) or a
// linear state with no positionMm. Refusing here is the point: sending a SET
// with a guessed angle would drive a real valve to the wrong place.
inline bool buildSetFrame(JsonObject out, uint32_t seq, const char* selectorId,
                          JsonObjectConst sel, const char* stateId) {
    const char* kind = sel["kind"].as<const char*>();
    if (!kind) return false;
    bool isServo = (strcmp(kind, "servoGate") == 0 || strcmp(kind, "servoManifold") == 0);
    bool isLinear = (strcmp(kind, "linear") == 0);
    if (!isServo && !isLinear) return false;

    if (isServo) {
        if (!servoIsCalibrated(sel)) return false;      // never set up — don't send a guess
        int angle = servoCommandAngle(sel, stateId);
        if (angle == INT32_MIN) return false;          // no such state / no offsetDeg
        out["drive"]      = "servo";
        out["angle"]      = angle;
        out["channel"]    = sel["servo"]["channel"] | 0;
        out["holdAtRest"] = sel["servo"]["holdAtRest"] | false;
    } else {
        bool found = false;
        for (JsonObjectConst s : sel["states"].as<JsonArrayConst>()) {
            if (!_eq(s["id"], stateId)) continue;
            if (!s.containsKey("positionMm")) return false;   // uncalibrated
            out["positionMm"] = s["positionMm"].as<float>();
            found = true;
            break;
        }
        if (!found) return false;
        out["drive"]   = "linear";
        out["channel"] = sel["linear"]["channel"] | 0;
        // Set only by a setup move (TopologyRuntime::driveLinearTo), never by routing: find the datum again first.
        if (sel["homeFirst"] | false) out["home"] = true;
    }

    out["t"]          = "SET";
    out["seq"]        = seq;
    out["selectorId"] = selectorId;
    out["stateId"]    = stateId;
    return true;
}

// -----------------------------------------------------------------------------
// Secondary → primary
// -----------------------------------------------------------------------------

// ⚠️ LIFETIME: every const char* below is stored BY POINTER — ArduinoJson does
// not copy them. They must stay alive until the document is serialized, which is
// typically after the caller's if/else has ended. Passing `someString.c_str()`
// where `someString` is scoped tighter than the serialize call yields a frame
// containing freed heap, not an empty field, so it fails as garbage rather than
// as an obvious blank. Same applies to buildAck/buildState below.
// `claimedBy` names the primary that owns this board; `accepted=false` says the
// asker is not it, and its SETs will be refused.
//
// A REFUSAL DOES NOT CLOSE THE SOCKET. The refused primary has to be able to
// read `claimedBy` to tell its user who holds the board — and a closed socket
// is indistinguishable from a node that is simply offline, which is the one
// reading that sends someone hunting for a wiring fault.
// `clamps` is the one capability here that does not move anything, and it is why
// a CT can appear in the UI at all: a plug is DISCOVERED by a subnet sweep, but a
// clamp has no address and never will — somebody soldered it to this board. So
// the board is the only thing that can say it exists. Reported from the pin map,
// not chosen, exactly like the servo count, so it cannot disagree with the
// hardware. Defaulted to 0 so every existing call site is unchanged and a board
// with no clamp says nothing rather than saying zero.
//
// `pollsPlugs` says this board can poll smart plugs on the primary's behalf
// (CONFIG sensors of kind "plug"). Absent means NO, on both sides — it must, or
// every board flashed before 2026-10-03 is handed a CONFIG it refuses whole,
// clamp and all. Omitted when false, like `ct`.
inline void buildWelcome(JsonObject out, const char* nodeId, const char* board,
                         const char* fw, int servos, int linear,
                         const char* claimedBy = nullptr, bool accepted = true,
                         int clamps = 0, bool pollsPlugs = false, bool dialsIn = false,
                         bool hasRf = false, bool hasBin = false) {
    out["t"]      = "WELCOME";
    out["v"]      = kVersion;
    out["nodeId"] = nodeId;
    out["board"]  = board;
    out["fw"]     = fw;
    JsonObject caps = out.createNestedObject("caps");
    caps["servos"] = servos;
    caps["linear"] = linear;
    // OMITTED WHEN ZERO, on purpose: absent already means none, so writing it
    // would add a field to every board's answer to repeat what silence said.
    if (clamps > 0) caps["ct"] = clamps;
    if (pollsPlugs) caps["plug"] = 1;
    // `join`: this board dials its primary itself (JOIN). Absent means NO, on both
    // sides, so a board flashed before 2026-10-04 keeps being dialled.
    if (dialsIn) caps["join"] = 1;
    // `rf` / `bin`: this board has a transmitter for the collector's remote / a dust-bin
    // pad (2026-10-04). Absent means NO, on both sides, so a board that predates them
    // is never sent a PRESS or a `bin` sensor it would refuse.
    if (hasRf)  caps["rf"]  = 1;
    if (hasBin) caps["bin"] = 1;
    if (claimedBy && *claimedBy) out["claimedBy"] = claimedBy;
    if (!accepted) out["accepted"] = false;
}

// JOIN — a node dialling its primary. The primary needs only to know who is knocking.
inline void buildJoin(JsonObject out, const char* nodeId) {
    out["t"]      = "JOIN";
    out["v"]      = kVersion;
    out["nodeId"] = nodeId;
}

// A node's own account of its last boot, added to a WELCOME — withBootInfo() in
// nodelink.js, which says why it exists. `upS` whole seconds since boot, `rst`
// a short reset-reason word. Optional both ways; absent means UNKNOWN, and
// nothing routes on either. kMaxRstLen is MAX_RST_LEN in nodelink.js (a pair —
// see CLAUDE.md): the JS validator refuses a longer `rst`, so a node writing one
// would have its whole WELCOME refused by a JS-certified primary.
static const size_t kMaxRstLen = 16;

inline void addBootInfo(JsonObject w, uint32_t upS, const char* rst) {
    w["upS"] = upS;
    if (rst && *rst) {
        // std::string's own length cap, not strlcpy_ — that helper is defined
        // further down this header.
        w["rst"] = std::string(rst).substr(0, kMaxRstLen);   // COPIED (std::string, not a pointer): the caller's buffer need not outlive serialize
    }
}

// Does this WELCOME say we may drive the node? Absent means yes, so a node
// built before claims answers exactly as it always did. The safe reading is the
// default one: only an explicit `accepted:false` refuses.
inline bool welcomeAccepted(JsonObjectConst w) {
    return !w.containsKey("accepted") || w["accepted"].as<bool>();
}

inline void buildAck(JsonObject out, uint32_t seq, bool ok, const char* err = nullptr) {
    out["t"]   = "ACK";
    out["seq"] = seq;
    out["ok"]  = ok;
    if (err && *err) out["err"] = err;
}

inline void buildState(JsonObject out, const char* selectorId, const char* stateId, bool moving) {
    out["t"]          = "STATE";
    out["selectorId"] = selectorId;
    out["stateId"]    = stateId;
    out["moving"]     = moving;
}

inline void buildPong(JsonObject out) { out["t"] = "PONG"; }

// OTA — "pull this image and install it". See ota() in nodelink.js for why it is
// a pull rather than a push. `md5` is 32 LOWERCASE hex chars; one spelling on the
// wire, so neither side has to normalise.
inline void buildOta(JsonObject out, uint32_t seq, const char* path, uint32_t size,
                     const char* md5, const char* fw) {
    out["t"]    = "OTA";
    out["seq"]  = seq;
    out["path"] = path;
    out["size"] = size;
    out["md5"]  = md5;
    out["fw"]   = fw;
}

// OTASTATE — a node's account of an update it was told to run. pct < 0 omits it,
// err null omits it (the same omit-don't-zero rule as SENSE).
inline void buildOtaState(JsonObject out, const char* state, int pct = -1, const char* err = nullptr) {
    out["t"]     = "OTASTATE";
    out["state"] = state;
    if (pct >= 0) out["pct"] = pct > 100 ? 100 : pct;
    if (err && *err) out["err"] = std::string(err).substr(0, 64);
}

// SENSE — "this sensor says on, or off". ONE BIT, AND THIS BOARD DECIDES IT.
//
// Not a shortcut: RFC §5.4b. A CT measures current, watts need a voltage and a
// power factor it cannot give, and a woodworking tool's standby sits under the
// noise floor anyway — so there is no threshold worth putting on the wire and
// nothing for the primary to interpret. It is also the only arrangement with
// usable latency, since mains-frequency RMS cannot round-trip per sample.
//
// `level` is a MULTIPLE OF THE TRIP POINT — not amps, not watts, not a raw
// count. It exists so the board can be commissioned without a serial console at
// the machine ("2.8" is comfortable, "1.05" says move the clamp), and its units
// are self-evidently not a measurement so nothing can mistake it for one.
// NOTHING MAY BRANCH ON IT. Pass a negative value to omit it, which is what a
// board with no trip point to divide by should do rather than send a zero that
// reads like a reading.
// TELEMETRY RIDES IN AMPS, NOT COUNTS (2026-09-17, jeff: "nobody but you and I
// care about counts"). The bench rule — log rmsCounts, not amps — is about the
// SERIAL CONSOLE, where counts are the raw measurement and amps are an
// interpretation that has been wrong before. It does not extend to a screen a
// woodworker reads. The node converts, using its own board's measured
// amps-per-count (CtTrip::Tick::aPerCount), which is exactly where a hardware
// constant belongs: nothing downstream owns a scale factor.
//
// All three are OPTIONAL and omitted rather than zeroed. Zero amps is a real
// reading; "this board has no floor yet" is not, and the two must not look alike.
inline void buildSense(JsonObject out, const char* sensorId, bool on,
                       float level = -1.0f, float amps = -1.0f,
                       float floorA = -1.0f, float tripA = -1.0f,
                       bool fault = false, float watts = -1.0f, bool plug = false) {
    out["t"]        = "SENSE";
    out["sensorId"] = sensorId ? sensorId : "";
    out["on"]       = on;
    if (level  >= 0.0f) out["level"]  = level;
    if (amps   >= 0.0f) out["amps"]   = amps;
    if (floorA >= 0.0f) out["floorA"] = floorA;
    if (tripA  >= 0.0f) out["tripA"]  = tripA;
    // A PLUG's reading. Negative omits it, as with every other telemetry field:
    // 0 W is a real reading (the tool is off) and "no reading" is not.
    if (watts  >= 0.0f) out["watts"]  = watts;
    // Says WHAT reported — see sense() in nodelink.js. Omitted for a clamp.
    if (plug)           out["plug"]   = true;
    // Omitted when false: a board that is fine says nothing, which keeps the
    // common frame small and makes the fault legible when it does appear.
    if (fault)          out["fault"]  = true;
}

// -----------------------------------------------------------------------------
// Decoding (secondary side)
// -----------------------------------------------------------------------------

// Local strlcpy so this header stays free of Arduino.h (host builds don't have
// the Arduino one, and glibc doesn't provide strlcpy at all).
inline void strlcpy_(char* dst, const char* src, size_t n) {
    if (n == 0) return;
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) dst[i] = src[i];
    dst[i] = '\0';
}

struct SetCommand {
    uint32_t seq;
    char     selectorId[48];
    char     stateId[32];
    bool     isServo;
    int      channel;
    int      angle;        // isServo
    float    positionMm;   // !isServo
    bool     holdAtRest;
    bool     home;         // !isServo: find the datum again before this move (SetFrame.home in nodelink.js)
};

// Parse + VALIDATE a SET frame. A secondary must never act on a malformed
// frame: the whole safety story here is that it moves only when told exactly
// where, so a missing or out-of-range field is a refusal, not a default.
inline bool parseSetFrame(JsonObjectConst f, SetCommand& out, const char*& err) {
    if (!_eq(f["t"], "SET"))                  { err = "not a SET frame"; return false; }
    const char* sid = f["selectorId"].as<const char*>();
    const char* st  = f["stateId"].as<const char*>();
    if (!sid || !*sid)                        { err = "missing selectorId"; return false; }
    if (!st  || !*st)                         { err = "missing stateId";    return false; }
    if (!f.containsKey("seq"))                { err = "missing seq";        return false; }
    if (!f.containsKey("channel"))            { err = "missing channel";    return false; }

    const char* drive = f["drive"].as<const char*>();
    if (!drive)                               { err = "missing drive";      return false; }

    out.seq     = f["seq"].as<uint32_t>();
    out.channel = f["channel"].as<int>();
    if (out.channel < 0 || out.channel > 15)  { err = "channel out of range"; return false; }
    strlcpy_(out.selectorId, sid, sizeof(out.selectorId));
    strlcpy_(out.stateId,    st,  sizeof(out.stateId));

    if (strcmp(drive, "servo") == 0) {
        if (!f.containsKey("angle"))          { err = "missing angle";      return false; }
        out.angle = f["angle"].as<int>();
        if (out.angle < 0 || out.angle > 180) { err = "angle out of range"; return false; }
        if (f.containsKey("home") && (f["home"] | false)) { err = "home is for a linear drive"; return false; }
        out.isServo    = true;
        out.holdAtRest = f["holdAtRest"] | false;
        out.positionMm = 0.0f;
        out.home       = false;
        return true;
    }
    if (strcmp(drive, "linear") == 0) {
        if (!f.containsKey("positionMm"))     { err = "missing positionMm"; return false; }
        // TYPE FIRST, and it is not pedantry. ArduinoJson's as<float>() on a
        // string returns 0.0f rather than failing, so {"positionMm":"far"} came
        // through the range check below as a perfectly valid request to move to
        // 0mm — the datum. A garbled field asking for a real move to the end of
        // the rail is the worst possible reading of it. validateFrame() in
        // nodelink.js rejects it on `typeof !== 'number'`; this is that.
        if (!f["positionMm"].is<float>())     { err = "positionMm must be a number"; return false; }
        const float mm = f["positionMm"].as<float>();
        // BOUNDED, exactly as validateFrame() bounds it in nodelink.js — this
        // was the one field on the wire that was not.
        //
        // The angle path has clamped 0..180 since it was written; positionMm
        // took whatever arrived. The node then multiplies it by counts/mm and
        // casts to long, so a NaN is undefined behaviour in the conversion and
        // a wild value becomes a command clamped to kMaxStepsPerCommand and
        // re-issued chunk after chunk until the runaway guard stops it. A
        // node's whole safety story is that it moves only when told exactly
        // where, and an unchecked number is not that.
        //
        // The NaN test is `mm != mm` rather than std::isnan: this header is
        // deliberately free of <cmath> and is compiled for both the host tests
        // and the ESP32.
        if (mm != mm)                         { err = "positionMm is not a number"; return false; }
        if (mm < -10000.0f || mm > 10000.0f)  { err = "positionMm out of range";    return false; }
        // HOME FIRST (2026-10-07), additive: absent is no. Typed like everything else here — a string "yes" would read
        // as false through `| false` and silently skip the home the asker wanted.
        if (f.containsKey("home") && !f["home"].is<bool>()) { err = "home must be true|false"; return false; }
        out.isServo    = false;
        out.positionMm = mm;
        out.angle      = 0;
        out.holdAtRest = false;
        out.home       = f["home"] | false;
        return true;
    }
    err = "drive must be servo|linear";
    return false;
}

// ── CONFIG ─────────────────────────────────────────────────────────────────
//
// What this board is WIRED TO — the one thing it cannot work out for itself.
//
// THIS IS NOT TOPOLOGY, and that is the only reason a node may hold it. It
// carries no elements, no routing, no states and no thresholds: nothing whose
// MEANING the primary could change under a board that was not reflashed.
// `sensorId` is opaque and only ever echoed back, `channel` is a pad. The
// invariant at the top of nodelink.js stays intact — a node owns loops, never
// interpretation.
// How much of an element id a board stores. Named rather than spelled 48 in
// each place that keeps one, because LocalActuatorBus keeps them too and a
// silent truncation on one side only would look like a sensor that never
// reports.
static const size_t kMaxSensorIdLen = 48;

struct SensorSpec {
    char sensorId[kMaxSensorIdLen];   // OPAQUE. Echoed in SENSE, never parsed.
    // What is watched. A PLUG is a smart plug on the network that this board polls
    // for the primary (2026-10-03); it uses ip/plugTasmota/thresholdW and NOT
    // channel, tripRatio, minCounts or clearRatio, which are a clamp's.
    bool  isPlug = false;
    // The dust-bin beam (2026-10-04): its pad is the board's own, so there is no channel,
    // and the only thing the layout says is which way it reads. The node reports it as a
    // SENSE bit — `on` means the bin is FULL — after debouncing (utils/BinSensor.h).
    bool  isBin = false;
    bool  binInvert = true;           // bin: true = the pin reads LOW when full (the optocoupler's sense)
    int   channel;                    // which input on THIS board (ct)
    char  ip[kMaxPlugIpLen + 1] = {0};   // plug
    bool  plugTasmota = false;        // plug: false = Shelly Gen2, true = Tasmota
    float thresholdW = 0.0f;          // plug: watts at or above which the tool is ON

    // HOW HARD TO SQUEEZE, sent by the primary since 2026-09-17 so that retuning
    // a shop is a primary reflash and nobody climbs to a node. See TripParams in
    // sensing/CtTrip.h for what each one means.
    //
    // ZERO MEANS "NOT SENT" and the board falls back to its own compiled-in
    // value. Zero is safe as a sentinel because it is illegal for all three —
    // parseConfigFrame refuses a ratio at or below 1, a guard at or below 0, and
    // a release ratio outside (0,1) — so a real value can never look absent.
    //
    // They are FLOATS HERE rather than a sensing::TripParams because this header
    // compiles against g++ and ArduinoJson alone for the host tests, and CtTrip
    // needs <Arduino.h>. The node does the conversion, which is also the only
    // place that knows its own fallbacks.
    float tripRatio;                  // 0 = use the board's own
    float minCounts;                  // 0 = use the board's own
    float clearRatio;                 // 0 = use the board's own
};

// Parse + VALIDATE a CONFIG frame into a fixed array.
//
// ALL OR NOTHING. A malformed entry rejects the WHOLE frame rather than
// applying the good ones, because the list is a whole new list and a
// half-applied configuration is a state nobody should have to reason about —
// the primary would believe it configured two sensors while the board reported
// on one, forever, with no frame saying so. Refusing is loud; partial success
// is silent.
//
// An EMPTY list is valid and means "report nothing" — the same state as a board
// that has never been configured, so there is no third case to handle.
inline bool parseConfigFrame(JsonObjectConst f, SensorSpec* out, size_t maxOut,
                             size_t& countOut, const char*& err) {
    if (!_eq(f["t"], "CONFIG"))    { err = "not a CONFIG frame"; return false; }
    if (!f.containsKey("seq"))     { err = "missing seq";         return false; }
    JsonArrayConst arr = f["sensors"];
    if (arr.isNull())              { err = "sensors must be an array"; return false; }
    if (arr.size() > maxOut)       { err = "too many sensors";    return false; }

    size_t n = 0;
    for (JsonObjectConst sen : arr) {
        const char* id = sen["sensorId"].as<const char*>();
        if (!id || !*id)           { err = "missing sensorId";    return false; }
        // Two entries under one id would make SENSE ambiguous in the only
        // direction that matters: which tool just started.
        for (size_t i = 0; i < n; i++) {
            if (strcmp(out[i].sensorId, id) == 0) { err = "duplicate sensorId"; return false; }
        }
        if (_eq(sen["kind"], "plug")) {
            // A plug has an address and a protocol and a threshold, and none of a
            // clamp's fields. TYPE FIRST on each, as everywhere in this parser.
            const char* ip = sen["ip"].as<const char*>();
            if (!ip || !*ip || strlen(ip) > kMaxPlugIpLen) { err = "plug ip must be a dotted quad"; return false; }
            int dots = 0;
            for (const char* c = ip; *c; c++) {
                if (*c == '.') dots++;
                else if (*c < '0' || *c > '9') { err = "plug ip must be a dotted quad"; return false; }
            }
            if (dots != 3) { err = "plug ip must be a dotted quad"; return false; }
            const char* pk = sen["plug"].as<const char*>();
            const bool tas = pk && strcmp(pk, "tasmota") == 0;
            if (!tas && !(pk && strcmp(pk, "shelly") == 0)) { err = "plug must be shelly or tasmota"; return false; }
            if (!sen["thresholdW"].is<float>()) { err = "plug thresholdW must be a number"; return false; }
            const float th = sen["thresholdW"].as<float>();
            if (th <= 0.0f || th > kMaxPlugThresholdW) { err = "plug thresholdW out of range"; return false; }
            strlcpy_(out[n].sensorId, id, sizeof(out[n].sensorId));
            out[n].isPlug = true;
            out[n].channel = 0;
            strlcpy_(out[n].ip, ip, sizeof(out[n].ip));
            out[n].plugTasmota = tas;
            out[n].thresholdW = th;
            out[n].tripRatio = out[n].minCounts = out[n].clearRatio = 0.0f;
            n++;
            continue;
        }
        out[n].isPlug = false;
        out[n].isBin = false;
        out[n].ip[0] = '\0';
        if (_eq(sen["kind"], "bin")) {
            // The bin beam: no channel, no tuning. `invert` is optional and must be a
            // boolean when present — TYPE FIRST, like every field here.
            bool inv = true;
            if (sen.containsKey("invert")) {
                if (!sen["invert"].is<bool>()) { err = "bin invert must be a boolean"; return false; }
                inv = sen["invert"].as<bool>();
            }
            strlcpy_(out[n].sensorId, id, sizeof(out[n].sensorId));
            out[n].isBin = true;
            out[n].binInvert = inv;
            out[n].channel = 0;
            out[n].tripRatio = out[n].minCounts = out[n].clearRatio = 0.0f;
            n++;
            continue;
        }
        if (!_eq(sen["kind"], "ct")) { err = "sensor kind must be ct, plug or bin"; return false; }
        if (!sen.containsKey("channel")) { err = "missing channel"; return false; }
        // TYPE FIRST, for the reason spelled out on positionMm above:
        // as<int>() on a string yields 0, which is a real pad on every board.
        if (!sen["channel"].is<int>())   { err = "channel must be a number"; return false; }
        const int ch = sen["channel"].as<int>();
        if (ch < 0 || ch > 15)           { err = "channel out of range"; return false; }

        // TUNING — all three OPTIONAL, and each validated only if present.
        //
        // Absent is the normal case for a primary older than 2026-09-17, so it
        // cannot be an error. A PRESENT but nonsense value must be, and loudly:
        // silently clamping a bad ratio would leave the primary believing it had
        // retuned a board that ignored it, which is the same class of failure as
        // a half-applied sensor list and is refused the same way — whole frame.
        // Bounds are EXCLUSIVE at both ends and mirror the `tune` table in
        // nodelink.js validateFrame(), value for value.
        //
        //   tripRatio  at or below 1 trips on the learned floor itself, or on
        //              nothing at all; 100x a real floor is not a tuning, it is
        //              a typo
        //   minCounts  4095 is full scale on a 12-bit ADC — a guard there can
        //              never be exceeded, so the board goes DEAF rather than
        //              merely insensitive
        //   clearRatio 1 is no hysteresis, the defect this was added to fix;
        //              above 1 releases ABOVE the trip point, so a tool could
        //              never read as stopped
        struct TuneRule { const char* key; float lo; float hi; };
        static const TuneRule kTune[3] = {
            {"tripRatio", 1.0f, 100.0f}, {"minCounts", 0.0f, 4095.0f},
            {"clearRatio", 0.0f, 1.0f},
        };
        float tune[3] = {0.0f, 0.0f, 0.0f};
        for (int k = 0; k < 3; k++) {
            // VALIDATED ON PRESENCE, not on being non-zero. The two differ in
            // exactly one case and it is the one that matters: an explicit 0.
            // Zero is the sentinel for "not sent", so a value-based check would
            // wave `"minCounts":0` through as silence and leave the primary
            // believing it had set a guard the board never applied. Caught by
            // the JS half of this pair, which had it right.
            if (!sen.containsKey(kTune[k].key)) continue;
            // TYPE FIRST, same reason as channel: as<float>() on a string is 0,
            // which here would read as "not sent" and quietly ignore the value.
            if (!sen[kTune[k].key].is<float>()) { err = "tuning must be a number"; return false; }
            const float v = sen[kTune[k].key].as<float>();
            if (v <= kTune[k].lo || v >= kTune[k].hi) { err = "tuning out of range"; return false; }
            tune[k] = v;
        }

        strlcpy_(out[n].sensorId, id, sizeof(out[n].sensorId));
        out[n].channel    = ch;
        out[n].tripRatio  = tune[0];
        out[n].minCounts  = tune[1];
        out[n].clearRatio = tune[2];
        n++;
    }
    countOut = n;
    return true;
}

// An OTA order, decoded. Fixed buffers — a node has no business allocating to
// receive an instruction to replace itself.
struct OtaOrder {
    uint32_t seq  = 0;
    char     path[kMaxOtaPath + 1] = {0};
    uint32_t size = 0;
    char     md5[33] = {0};
    char     fw[24]  = {0};
};

// Refuses WHOLE, with a reason a log can print, and mirrors validateFrame's `OTA`
// case in nodelink.js check for check. TYPE FIRST on every field, for the reason
// positionMm explains above: as<int>() on a string is 0, which here would read as
// a size that fails the bound — right answer, wrong reason, and a log that blames
// the wrong thing.
inline bool parseOtaFrame(JsonObjectConst f, OtaOrder& out, const char*& err) {
    if (!_eq(f["t"], "OTA"))               { err = "not an OTA frame"; return false; }
    if (!f.containsKey("seq") || !f["seq"].is<uint32_t>()) { err = "missing seq"; return false; }
    const char* path = f["path"].as<const char*>();
    if (!path || path[0] != '/' || strlen(path) > kMaxOtaPath) { err = "bad path"; return false; }
    if (!f["size"].is<uint32_t>())         { err = "size must be a number"; return false; }
    const uint32_t size = f["size"].as<uint32_t>();
    if (size < kMinOtaBytes || size > kMaxOtaBytes) { err = "size out of range"; return false; }
    const char* md5 = f["md5"].as<const char*>();
    if (!md5 || strlen(md5) != 32)         { err = "md5 must be 32 hex chars"; return false; }
    for (int i = 0; i < 32; i++) {
        const char c = md5[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { err = "md5 must be lowercase hex"; return false; }
    }
    const char* fw = f["fw"].as<const char*>();
    if (!fw || !*fw)                       { err = "missing fw"; return false; }
    out.seq  = f["seq"].as<uint32_t>();
    strlcpy_(out.path, path, sizeof(out.path));
    out.size = size;
    strlcpy_(out.md5, md5, sizeof(out.md5));
    strlcpy_(out.fw, fw, sizeof(out.fw));
    return true;
}

// WHERE, parsed. Refuses WHOLE like every frame here, and mirrors validateFrame's
// `WHERE` case in nodelink.js check for check. The address is checked as a dotted
// quad of digits only: it is about to be dialled, and a hostname here would be a
// lookup a network is allowed to block.
struct WhereOrder {
    char primaryId[40] = {0};
    char ip[kMaxWhereIpLen + 1] = {0};
    int  port = 80;
};

inline bool parseWhereFrame(JsonObjectConst f, WhereOrder& out, const char*& err) {
    if (!_eq(f["t"], "WHERE"))             { err = "not a WHERE frame"; return false; }
    const char* pid = f["primaryId"].as<const char*>();
    if (!pid || !*pid)                     { err = "missing primaryId"; return false; }
    const char* ip = f["ip"].as<const char*>();
    if (!ip || strlen(ip) > kMaxWhereIpLen || !*ip) { err = "bad ip"; return false; }
    int dots = 0, digits = 0;
    for (const char* c = ip; *c; ++c) {
        if (*c == '.') { if (!digits) { err = "bad ip"; return false; } dots++; digits = 0; }
        else if (*c >= '0' && *c <= '9') { if (++digits > 3) { err = "bad ip"; return false; } }
        else { err = "bad ip"; return false; }
    }
    if (dots != 3 || !digits)              { err = "bad ip"; return false; }
    if (!f["port"].is<int>())              { err = "port must be a number"; return false; }
    const int port = f["port"].as<int>();
    if (port < 1 || port > 65535)          { err = "port out of range"; return false; }
    strlcpy_(out.primaryId, pid, sizeof(out.primaryId));
    strlcpy_(out.ip, ip, sizeof(out.ip));
    out.port = port;
    return true;
}

// A PRESS, decoded. Refuses WHOLE with a reason a log can print, and mirrors
// validateFrame's `PRESS` case in nodelink.js check for check. TYPE FIRST on every
// field: as<int>() on a string is 0, which for `address` is a real address.
struct PressOrder {
    uint32_t seq = 0;
    uint8_t  address = 0;
    uint8_t  data = 0;
    uint32_t tickUs = 0;
    uint32_t repeats = 0;
};

inline bool parsePressFrame(JsonObjectConst f, PressOrder& out, const char*& err) {
    if (!_eq(f["t"], "PRESS"))               { err = "not a PRESS frame"; return false; }
    if (!f["seq"].is<uint32_t>())            { err = "missing seq"; return false; }
    if (!f["address"].is<int>() || f["address"].as<int>() < 0 || f["address"].as<int>() > 255)
                                              { err = "address must be 0-255"; return false; }
    if (!f["data"].is<int>() || f["data"].as<int>() < 0 || f["data"].as<int>() > 15)
                                              { err = "data must be 0-15"; return false; }
    if (!f["tickUs"].is<uint32_t>() || f["tickUs"].as<uint32_t>() < kMinRfTickUs || f["tickUs"].as<uint32_t>() > kMaxRfTickUs)
                                              { err = "tickUs out of range"; return false; }
    if (!f["repeats"].is<uint32_t>() || f["repeats"].as<uint32_t>() < 1 || f["repeats"].as<uint32_t>() > kMaxRfRepeats)
                                              { err = "repeats out of range"; return false; }
    out.seq     = f["seq"].as<uint32_t>();
    out.address = (uint8_t)f["address"].as<int>();
    out.data    = (uint8_t)f["data"].as<int>();
    out.tickUs  = f["tickUs"].as<uint32_t>();
    out.repeats = f["repeats"].as<uint32_t>();
    return true;
}

inline bool isRefuseReason(const char* r) {
    if (!r) return false;
    for (size_t i = 0; i < kRefuseReasonCount; i++) if (strcmp(r, kRefuseReasons[i]) == 0) return true;
    return false;
}

} // namespace nodelink
} // namespace topo

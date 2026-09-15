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

inline void buildPing(JsonObject out) { out["t"] = "PING"; }

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
inline void buildWelcome(JsonObject out, const char* nodeId, const char* board,
                         const char* fw, int servos, int linear,
                         const char* claimedBy = nullptr, bool accepted = true,
                         int clamps = 0) {
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
    if (claimedBy && *claimedBy) out["claimedBy"] = claimedBy;
    if (!accepted) out["accepted"] = false;
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
inline void buildSense(JsonObject out, const char* sensorId, bool on,
                       float level = -1.0f) {
    out["t"]        = "SENSE";
    out["sensorId"] = sensorId ? sensorId : "";
    out["on"]       = on;
    if (level >= 0.0f) out["level"] = level;
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
        out.isServo    = true;
        out.holdAtRest = f["holdAtRest"] | false;
        out.positionMm = 0.0f;
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
        out.isServo    = false;
        out.positionMm = mm;
        out.angle      = 0;
        out.holdAtRest = false;
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
struct SensorSpec {
    char sensorId[48];   // OPAQUE. Echoed in SENSE, never parsed.
    int  channel;        // which input on THIS board
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
        if (!_eq(sen["kind"], "ct")) { err = "sensor kind must be ct"; return false; }
        if (!sen.containsKey("channel")) { err = "missing channel"; return false; }
        // TYPE FIRST, for the reason spelled out on positionMm above:
        // as<int>() on a string yields 0, which is a real pad on every board.
        if (!sen["channel"].is<int>())   { err = "channel must be a number"; return false; }
        const int ch = sen["channel"].as<int>();
        if (ch < 0 || ch > 15)           { err = "channel out of range"; return false; }
        strlcpy_(out[n].sensorId, id, sizeof(out[n].sensorId));
        out[n].channel = ch;
        n++;
    }
    countOut = n;
    return true;
}

} // namespace nodelink
} // namespace topo

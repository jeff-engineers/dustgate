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

// A move that takes longer than this without a STATE(moving=false) is assumed
// lost rather than left to wedge the move queue forever. Generously longer than
// SERVO_SWEEP_MS + SERVO_HOLD_MS, and longer than a full-span rack traverse.
//
// RAISED FROM 12s TO 90s ON 2026-08-28, because the second half of that sentence
// stopped being true. It was written against the stepper at 19mm/s; the ST3215
// slider crosses an 8-gate 2.5" span (582mm) at ~42mm/s in 14s, so 12s declared
// a perfectly healthy traverse lost. Worse, a slider NODE homes at boot and
// defers the move it was sent until the sweep finds the datum — a sweep that can
// legitimately take the better part of a minute (see the CALIBRATION note in
// node/dustgate_node.cpp).
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
static const unsigned long kMoveTimeoutMs   = 90000;

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
inline void buildWelcome(JsonObject out, const char* nodeId, const char* board,
                         const char* fw, int servos, int linear,
                         const char* claimedBy = nullptr, bool accepted = true) {
    out["t"]      = "WELCOME";
    out["v"]      = kVersion;
    out["nodeId"] = nodeId;
    out["board"]  = board;
    out["fw"]     = fw;
    JsonObject caps = out.createNestedObject("caps");
    caps["servos"] = servos;
    caps["linear"] = linear;
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
        out.isServo    = false;
        out.positionMm = f["positionMm"].as<float>();
        out.angle      = 0;
        out.holdAtRest = false;
        return true;
    }
    err = "drive must be servo|linear";
    return false;
}

} // namespace nodelink
} // namespace topo

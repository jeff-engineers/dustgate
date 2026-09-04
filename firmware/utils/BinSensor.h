// =============================================================================
// utils/BinSensor.h — dust-bin level: debounce, and whose bin it is.
//
// One input pin behind an optocoupler (docs/shop-schema-rfc.md §7.5). This
// header holds the two parts of that which are worth testing, and neither of
// them touches Arduino.h: the debouncer, and the rule for deciding whether a
// given collector's bin sensor is wired to THIS board.
//
// ⚠️ THE OPTOCOUPLER INVERTS THE SENSE. The raw pin reads LOW when the bin is
// FULL. Nothing in this file knows that — callers pass `raw` already in
// bin-full terms, because the inversion is a property of the WIRING and the
// schema carries it as `bin.sensor.invert`. Someone who wires the sensor
// straight to a pull-up instead (simpler, less isolation, rejected in §7.4)
// gets the opposite polarity and should not need a reflash to say so.
//
// PURE — STL + ArduinoJson only, NO Arduino.h, so the host tests can drive it.
// =============================================================================

#pragma once
#include <ArduinoJson.h>
#include <cstdint>
#include <string>

namespace topo {

// How long the sensor must hold a reading before we believe it.
//
// A diffuse beam over a dust bin is not a clean switch: chips swirl, the level
// slumps, and a bare read would chatter. Two seconds is long against how fast a
// bin actually fills (minutes) and short against how long someone would tolerate
// a stale "full" after emptying it.
//
// NOT A JS↔C++ PAIR, and saying so is part of the job (see CLAUDE.md). This
// never goes on the wire and no JS model simulates a flickering beam — the mock
// and demo stage bin state directly, the way they stage a plug fault. It is the
// same shape as kMoveTimeoutMs: the primary's own bookkeeping. Change it alone.
//
// UNVERIFIED: picked from how the bin behaves in the author's head, not from a
// beam over a real bin. It is the first number to revisit once one is wired.
static const uint32_t kBinDebounceMs = 2000;

// Believe a reading only after it has held still for kBinDebounceMs.
//
// DELIBERATELY NOT A LATCH. A trip follows the sensor back down when the bin is
// emptied, rather than holding until acknowledged. Latching needs somewhere to
// acknowledge FROM, and no such surface exists yet (§7.5 lists it open). A latch
// added later belongs above this class, not inside it.
class BinDebounce {
public:
    // `raw` is already in bin-full terms — true means full. See the inversion
    // note at the top of this file.
    void sample(bool raw, uint32_t nowMs) {
        if (!_seeded) {                 // first sample wins immediately: a board
            _stable = _candidate = raw; // that boots with a full bin should say so
            _sinceMs = nowMs;           // rather than claim OK for two seconds.
            _seeded = true;
            return;
        }
        if (raw != _candidate) {        // reading moved — restart the clock
            _candidate = raw;
            _sinceMs   = nowMs;
            return;
        }
        if (raw != _stable && (uint32_t)(nowMs - _sinceMs) >= kBinDebounceMs)
            _stable = raw;
    }

    bool full()   const { return _stable; }
    bool seeded() const { return _seeded; }   // false = never sampled, report nothing

private:
    bool     _stable    = false;
    bool     _candidate = false;
    bool     _seeded    = false;
    uint32_t _sinceMs   = 0;
};

// Which system's collector has its bin sensor wired to THIS board, if any.
// Returns an empty string for "none of them".
//
// The controllerId rule MIRRORS NodeBus's, deliberately — absent, or equal to
// this board's own id, means local — because "which board is this thing on" is
// one question and answering it two ways is how a link goes green while nothing
// on it works (see the bareHost note in NodeBus.h). The comparison is exact
// here rather than host-normalised: a controllerId is a name the UI chose, and
// unlike a link's host it never arrives with a `.local` suffix.
//
// A collector with no `bin` at all is not local, not remote, just absent — the
// caller reports nothing rather than reporting "not full", because an unwatched
// bin and an empty bin are different claims.
inline std::string localBinSystemId(JsonObjectConst topology, const char* ownId) {
    const std::string own = ownId ? ownId : "";
    JsonArrayConst systems = topology["systems"];
    // v1 documents have no `systems` array — the whole document IS one system,
    // and its id is the one viewOf() gives it. Handled by the caller, which has
    // the SystemView; this function only walks a v2 document.
    for (JsonObjectConst sys : systems) {
        for (JsonObjectConst e : sys["elements"].as<JsonArrayConst>()) {
            const char* type = e["type"];
            if (!type || strcmp(type, "collector") != 0) continue;
            JsonObjectConst sensor = e["bin"]["sensor"];
            if (sensor.isNull()) continue;
            const char* cid = sensor["controllerId"];
            if (!cid || own.empty() || own == cid) return std::string(sys["id"] | "");
        }
    }
    return std::string();
}

}  // namespace topo

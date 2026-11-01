// BusBank.h — the bus board's valves and manifolds: up to kMaxBusServos ST3215-style servos on one serial bus, moved ONE AT
// A TIME, each move watched to its end and its peak load kept (2026-11-01).
//
// Pure (no Arduino.h): the bus is behind Port, so host tests drive the whole policy — the queue, arrival, the stopped-short
// fault, the peak, the report — against a fake. The real Port is in dustgate_node.cpp, over ST3215Bus.
//
// WHY ONE AT A TIME. The servos share the board's one 12 V brick, and the wire-length reasoning (TODO, bus-servo chains)
// assumes only the moving servo draws heavy current: a stalled ST3215 takes 2.7 A, and two at once on a long chain shift
// the ground under the data line. The brain already moves one gate at a time shop-wide; this is the board keeping the
// rule itself, so a second SET that arrives mid-move waits rather than starts.
//
// WHAT IT DOES NOT DO: decide anything about the layout. It moves a number to an angle and reports what the servo said
// (nodelink.js, the invariant at the top). The slider is not here: it shares the bus but has its own driver and homing.
#pragma once
#include <cstdint>
#include <cstring>
#include "../../control/NodeLink.h"

namespace busbank {

using topo::nodelink::kMaxBusServos;
using topo::nodelink::BusServoReport;

/** One reading of a servo's registers. */
struct Reading {
    uint16_t pos = 0;       // 0..4095, one turn
    int      load = 0;      // percent of full drive, signed
    float    v = 0.0f;      // supply at the servo
    int      temp = 0;      // °C
    bool     moving = false;
};

/** The bus, as the bank needs it. Every call is one exchange; false = it did not answer (or answered nonsense). */
class Port {
public:
    virtual ~Port() {}
    enum class Heard { None, One, Garbled };
    /** Who answers as `id`: nobody, one servo, or several at once (their replies collided — a bad frame, not silence). */
    virtual Heard ping(uint8_t id) = 0;
    virtual bool read(uint8_t id, Reading& r) = 0;
    virtual bool moveTo(uint8_t id, uint16_t pos, uint16_t speed) = 0;
    virtual bool torque(uint8_t id, bool on) = 0;
    /** Position mode, limits a whole turn, through the servo's EEPROM lock. A servo carries its mode across power cycles
     *  and boards, so a servo once a slider's (stepping mode) would otherwise spin when sent an angle. */
    virtual bool positionMode(uint8_t id) = 0;
    /** Give the servo answering as `from` the number `to`, saved in it. */
    virtual bool renumber(uint8_t from, uint8_t to) = 0;
    /** Wait, keeping the board alive (the watchdog). Only the rare SERVO ops wait; a move never does. */
    virtual void pause(uint32_t ms) = 0;
};

// The speed every valve move runs at, in counts/s (4096 a turn): a quarter turn in about 1.7 s. Slow enough that the load
// it reads is the valve rather than the servo's own gearbox (the bench, 2026-11-01: ~72% load just spinning at full
// speed, 13% uncoupled at 300); a compromise, not measured as the best one.
static const uint16_t kMoveSpeed     = 600;
static const int      kArriveCounts  = 30;     // within this of the goal is there (~2.6°); the bench lands within 4
static const uint32_t kPollMs        = 20;     // how often a moving servo is read
static const uint32_t kSlackMs       = 3000;   // added to the time the move should take before it is called stopped short
static const uint32_t kSilentMs      = 1000;   // a moving servo that has not answered for this long is not answering

inline uint16_t degToCounts(int deg) {
    long c = ((long)deg * 4096L + 180L) / 360L;
    return (uint16_t)(c < 0 ? 0 : c > 4095 ? 4095 : c);
}
inline float countsToDeg(uint16_t c) { return (float)c * 360.0f / 4096.0f; }

/** What a finished move says: where it was going and, if it did not get there, why — in a sentence for a person. */
struct Ended {
    bool        any = false;
    char        sel[48] = {0};
    char        state[32] = {0};
    const char* err = nullptr;   // nullptr = arrived; always a string literal
};

class Bank {
public:
    explicit Bank(Port& p) : _p(p) {}

    /** Queue a move. A second move for the same servo replaces the first (the latest is the one that matters); false
     *  when the board already knows kMaxBusServos other servos. */
    bool want(uint8_t id, int deg, const char* sel, const char* state) {
        Known* k = know(id);
        if (!k) return false;
        for (size_t i = 0; i < _n; i++) if (_queue[i].id == id) { fill(_queue[i], id, deg, sel, state); return true; }
        if (_n >= (size_t)kMaxBusServos) return false;
        fill(_queue[_n++], id, deg, sel, state);
        return true;
    }
    /** A servo the board should report on though no move has named it yet (its failsafe close names it). */
    void note(uint8_t id) { know(id); }

    bool busy() const { return _moving || _n > 0; }
    bool moving() const { return _moving; }
    uint8_t movingId() const { return _moving ? _cur.id : 0; }

    /** Run the queue. Returns the move that ended on this tick, if one did. Call it from loop(), never a socket task. */
    Ended tick(uint32_t now) {
        Ended out;
        if (!_moving) { if (_n) start(now, out); return out; }
        if (now - _lastPollMs < kPollMs) return out;
        _lastPollMs = now;
        Known* k = find(_cur.id);
        Reading r;
        k->reads++;
        if (!_p.read(_cur.id, r)) {
            k->missed++;
            if (now - _lastHeardMs >= kSilentMs) finish(out, "the valve's servo stopped answering - check its cable and the board's 12 V supply");
            return out;
        }
        _lastHeardMs = now;
        k->last = r; k->lastOk = true;
        const int mag = r.load < 0 ? -r.load : r.load;
        if (mag > _peak) _peak = mag;
        const int err = (int)r.pos - (int)_cur.goal;
        if ((err < 0 ? -err : err) <= kArriveCounts && !r.moving) { finish(out, nullptr); return out; }
        if ((int32_t)(now - _deadlineMs) >= 0) finish(out, "the valve's servo stopped short of where it was sent - check the valve isn't jammed");
        return out;
    }

    /** Read every servo the board knows and fill `out` (kMaxBusServos long). Idle only: a read mid-move would come
     *  between the moving servo's polls, and the moving one is being read already. Returns how many. */
    size_t report(BusServoReport* out) {
        for (size_t i = 0; i < _nk; i++) {
            Known& k = _known[i];
            if (!(_moving && _cur.id == k.id)) {
                Reading r; k.reads++;
                if (_p.read(k.id, r)) { k.last = r; k.lastOk = true; } else { k.missed++; k.lastOk = false; }
            }
            BusServoReport& o = out[i];
            o = BusServoReport();
            o.id = k.id; o.ok = k.lastOk; o.missed = k.missed; o.reads = k.reads; o.peak = k.peak; o.moves = k.moves;
            if (k.lastOk) { o.deg = countsToDeg(k.last.pos); o.load = k.last.load; o.v = k.last.v; o.temp = k.last.temp; }
        }
        return _nk;
    }

    // ── numbering and the other SERVO ops (idle only; the caller waits for !busy()) ──
    /** find: who answers as 1. If exactly one, turn it a little so a person sees which. */
    Port::Heard findNew() {
        const Port::Heard h = _p.ping(topo::nodelink::kBusIdUnassigned);
        if (h == Port::Heard::One) wiggleRaw(topo::nodelink::kBusIdUnassigned);
        return h;
    }
    /** assign: the one new servo becomes `to`. Refused if `to` already answers (two servos, one number) or if the new
     *  one is not alone. The err is a sentence for a person. */
    bool assign(uint8_t to, const char*& err) {
        if (_p.ping(to) != Port::Heard::None) { err = "a servo already answers to that number on this board"; return false; }
        const Port::Heard h = _p.ping(topo::nodelink::kBusIdUnassigned);
        if (h == Port::Heard::None)    { err = "no new servo answered - check it is plugged in and the board's 12 V is on"; return false; }
        if (h == Port::Heard::Garbled) { err = "more than one new servo is plugged in - unplug all but one"; return false; }
        if (!_p.renumber(topo::nodelink::kBusIdUnassigned, to) || _p.ping(to) != Port::Heard::One) {
            err = "the servo did not take its new number - try again"; return false;
        }
        Known* k = find(to);
        if (k) k->modeSet = false;   // a servo new to this number gets its mode set on its first move
        return true;
    }
    bool wiggle(uint8_t id) { return wiggleRaw(id); }
    bool torque(uint8_t id, bool on) { return _p.torque(id, on); }
    /** reset: back to "not yet assigned", and forgotten here. */
    bool reset(uint8_t id) {
        if (!_p.renumber(id, topo::nodelink::kBusIdUnassigned)) return false;
        forget(id);
        return true;
    }
    /** scan: every number 1..kMaxBusId that answers, into `ids` (at most `max`). Slow — about a second a hundred. */
    size_t scan(uint8_t* ids, size_t max) {
        size_t n = 0;
        for (int id = 1; id <= topo::nodelink::kMaxBusId && n < max; id++)
            if (_p.ping((uint8_t)id) != Port::Heard::None) ids[n++] = (uint8_t)id;
        return n;
    }

private:
    struct Move { uint8_t id = 0; uint16_t goal = 0; char sel[48] = {0}; char state[32] = {0}; };
    struct Known { uint8_t id = 0; bool modeSet = false; int peak = 0; uint32_t reads = 0, missed = 0, moves = 0; Reading last; bool lastOk = false; };

    static void copy(char* d, const char* s, size_t n) { size_t i = 0; for (; s && s[i] && i + 1 < n; i++) d[i] = s[i]; d[i] = 0; }
    static void fill(Move& m, uint8_t id, int deg, const char* sel, const char* state) {
        m.id = id; m.goal = degToCounts(deg); copy(m.sel, sel, sizeof(m.sel)); copy(m.state, state, sizeof(m.state));
    }
    Known* find(uint8_t id) { for (size_t i = 0; i < _nk; i++) if (_known[i].id == id) return &_known[i]; return nullptr; }
    Known* know(uint8_t id) {
        if (Known* k = find(id)) return k;
        if (_nk >= (size_t)kMaxBusServos) return nullptr;
        _known[_nk] = Known(); _known[_nk].id = id;
        return &_known[_nk++];
    }
    void forget(uint8_t id) {
        for (size_t i = 0; i < _nk; i++) if (_known[i].id == id) { _known[i] = _known[--_nk]; break; }
        for (size_t i = 0; i < _n; i++) if (_queue[i].id == id) { for (size_t j = i + 1; j < _n; j++) _queue[j - 1] = _queue[j]; _n--; break; }
    }
    void start(uint32_t now, Ended& out) {
        _cur = _queue[0];
        for (size_t j = 1; j < _n; j++) _queue[j - 1] = _queue[j];
        _n--;
        Known* k = find(_cur.id);
        Reading r;
        const bool heard = _p.read(_cur.id, r);
        k->reads++; if (!heard) k->missed++;
        if (!heard || (!k->modeSet && !_p.positionMode(_cur.id)) || !_p.torque(_cur.id, true) ||
            !_p.moveTo(_cur.id, _cur.goal, kMoveSpeed)) {
            _moving = true;   // so finish() reports it as this move's end
            finish(out, "the valve's servo is not answering - check its cable and the board's 12 V supply");
            return;
        }
        k->modeSet = true;
        const long dist = r.pos > _cur.goal ? r.pos - _cur.goal : _cur.goal - r.pos;
        _deadlineMs  = now + (uint32_t)(dist * 1000L / kMoveSpeed) + kSlackMs;
        _lastPollMs  = now;
        _lastHeardMs = now;
        _peak        = 0;
        _moving      = true;
    }
    void finish(Ended& out, const char* err) {
        out.any = true; copy(out.sel, _cur.sel, sizeof(out.sel)); copy(out.state, _cur.state, sizeof(out.state)); out.err = err;
        if (Known* k = find(_cur.id)) { k->peak = _peak; k->moves++; }
        _moving = false;
    }
    bool wiggleRaw(uint8_t id) {
        // A small turn and back, at a gentle speed, from wherever it is: far enough to see, not far enough to matter.
        Reading r;
        if (!_p.read(id, r) || !_p.torque(id, true)) return false;
        const uint16_t there = (uint16_t)(r.pos > 4095 - 140 ? r.pos - 140 : r.pos + 140);
        if (!_p.moveTo(id, there, 800)) return false;
        _p.pause(500);
        return _p.moveTo(id, r.pos, 800);
    }

    Port&    _p;
    Move     _queue[kMaxBusServos];
    size_t   _n = 0;
    Known    _known[kMaxBusServos];
    size_t   _nk = 0;
    bool     _moving = false;
    Move     _cur;
    uint32_t _deadlineMs = 0, _lastPollMs = 0, _lastHeardMs = 0;
    int      _peak = 0;
};

}  // namespace busbank

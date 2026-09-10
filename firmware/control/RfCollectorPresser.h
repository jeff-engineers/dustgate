// =============================================================================
// RfCollectorPresser.h — press the collector's button by transmitting its frame.
//
// The first CollectorPresser (control/CollectorPress.h). It does not touch the
// fob at all: it keys a 315 MHz transmitter with the same 12-bit HT12E frame the
// fob sends, so the receiver cannot tell the difference. Chosen ahead of the
// servo path (docs/tool-sensing-rfc.md §4.2a) because it needs no printed
// fixture and no mechanical alignment — it can be tested tonight.
//
// EVERY NUMBER BELOW WAS MEASURED, on `collector-rf-bench`, against the real
// Rockler receiver. `firmware/bench/ht12e_bench.cpp` is the console that found
// them and remains the place to re-derive them for a different remote:
//
//   ADDRESS   0b01011110 — the fob's DIP has rockers 1, 6, 8 CLOSED, which
//             grounds A0, A5 and A7. Bit i = A_i, 1 = "left open" = logic 1.
//             PER-FOB: a different remote is a different address, which is
//             exactly why this is a constructor argument and not a constant.
//   DATA      0b1110 — AD8 asserted, the rest open. A full 16-value sweep keyed
//             the receiver on every EVEN word and nothing odd, so the rule is
//             "AD8 low" and AD9..AD11 are don't-cares. Earlier guesses at this
//             were wrong twice; the sweep settled it.
//   TICK      270 us, from RCSwitch protocol 11 — { 270, {36,1}, {1,2}, {2,1},
//             true }. A tick sweep found the receiver accepts roughly 85 us to
//             beyond 400 us, so 270 sits comfortably mid-window rather than on
//             an edge. It is NOT this chip's own rate: our encoder measured
//             ~3.5 kHz where Holtek's example says 3.0.
//   REPEATS   24 frames, ~500 ms. The 500 is measured too, and replaced a
//             derived 120 ms that keyed the receiver only intermittently while
//             the fob was rock solid. The arithmetic behind the 120 was never
//             trustworthy — the word rate comes off fOSC through a ÷3 divider
//             and the datasheet gives no word duration directly.
//
// WHY RMT AND NOT delayMicroseconds(). This is option B from §4.2, rejected
// there on timing grounds: the primary runs WiFi, and FreeRTOS will preempt a
// bit-banging loop and stretch a pulse. The RMT peripheral clocks the train out
// in HARDWARE from (duration, level) pairs, so nothing the CPU does afterwards
// can disturb it. That single difference is what makes this a reimplementation
// of the protocol rather than a bad imitation of one.
//
// WHAT IT STILL CANNOT DO: know whether the collector heard it. A frame is an
// EDGE against a TOGGLE — see the header of CollectorPress.h — so press() can
// only report that it TRANSMITTED. Whether the blower changed state is a
// question only the plug reading answers, and the retry policy is what closes
// the gap.
//
// ⚠️ REGULATORY, for a product rather than a shop. §11 flags this: transmitting
// on 315 MHz from our own hardware is an intentional radiator and needs testing
// and certification. Jeff's own shop is not the product; shipping this is a
// different conversation, and the servo path (§4.2a) avoids it entirely by
// operating a fob that is already certified.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "CollectorPress.h"

class RfCollectorPresser : public topo::CollectorPresser {
public:
    // Measured against the Rockler receiver; see the header. Defaults are that
    // remote's values, and every one of them is overridable because a different
    // fob is a different address and possibly a different data rule.
    static const uint8_t  kRocklerAddress = 0b01011110;
    static const uint8_t  kRocklerData    = 0b1110;
    static const uint32_t kDefaultTickUs  = 270;
    static const uint16_t kDefaultRepeats = 24;

    RfCollectorPresser(int pin,
                       uint8_t  address = kRocklerAddress,
                       uint8_t  data    = kRocklerData,
                       uint32_t tickUs  = kDefaultTickUs,
                       uint16_t repeats = kDefaultRepeats,
                       bool     inverted = true)
        : _pin(pin), _addr(address), _data(data),
          _tickUs(tickUs), _repeats(repeats), _inv(inverted) {}

    const char* kind() const override { return "rf"; }

    bool press() override {
        if (_pin < 0) return false;
        if (!ensureRmt()) return false;

        // 12 bits — A0..A7 then AD8..AD11, transmission order per the datasheet
        // — plus the sync. A repeating frame is cyclic, so sync-LAST is
        // equivalent to sync-first and saves shuffling the buffer.
        rmt_data_t frame[13];
        for (int i = 0; i < 8; i++) bitSym(frame[i],     (_addr >> i) & 1);
        for (int i = 0; i < 4; i++) bitSym(frame[8 + i], (_data >> i) & 1);
        sym(frame[12], 36, 1);                       // sync / pilot

        for (uint16_t r = 0; r < _repeats; r++) {
            // BLOCKING FOR ~500 ms, and the caller has to know it. The sketch
            // runs this from loop(), which is acceptable only because a press
            // happens on a STATE CHANGE rather than on a tick, and the watchdog
            // is at 10 s — it pets either side of the call regardless. Do not
            // move this onto a path that runs every pass.
            if (!rmtWrite(_pin, frame, 13, RMT_WAIT_FOR_EVER)) return false;
        }
        return true;
    }

private:
    int      _pin;
    uint8_t  _addr;
    uint8_t  _data;
    uint32_t _tickUs;
    uint16_t _repeats;
    bool     _inv;
    bool     _ready = false;

    bool ensureRmt() {
        if (_ready) return true;
        // 1 MHz → one tick per microsecond, so durations are written in us.
        _ready = rmtInit(_pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 1000000);
        if (!_ready) {
            DEBUG_PRINT(F("[RF] rmtInit failed on pin ")); DEBUG_PRINTLN(_pin);
        }
        return _ready;
    }

    // RCSwitch counts {high, low} in ticks and then inverts the levels, so a "1"
    // is {2,1} → two ticks low, one tick high.
    void sym(rmt_data_t& e, uint8_t firstTicks, uint8_t secondTicks) {
        e.level0    = _inv ? 0 : 1;
        e.duration0 = firstTicks  * _tickUs;
        e.level1    = _inv ? 1 : 0;
        e.duration1 = secondTicks * _tickUs;
    }

    // NOT `bit` — Arduino.h defines that as a macro, and a two-argument call to
    // it fails with an error that names neither this file's intent nor that one.
    void bitSym(rmt_data_t& e, bool one) {
        if (one) sym(e, 2, 1); else sym(e, 1, 2);
    }
};

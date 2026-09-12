// =============================================================================
// RfAddressGuess.h — the four ways a person can copy a DIP switch wrong.
//
// Jeff, 2026-09-10: if the address the user entered does not work, try flipping
// it and try reversing it, and keep whichever the collector answers. We have
// closed-loop feedback, so a candidate can be TESTED rather than reasoned about.
//
// THE FOUR CANDIDATES ARE NOT ARBITRARY. Reading an eight-way DIP upside down
// does two things at once — it reverses the rocker order AND swaps which side is
// ON — so the plausible mistakes are exactly: as entered, inverted, reversed,
// and both. There is no fifth. (A user who miscounts a single rocker produces a
// wrong address none of these reach, and that is correctly a failure: guessing
// further would be searching, not correcting.)
//
// ⚠️ SETUP ONLY. NEVER AT RUNTIME. This is the important constraint and it is
// not obvious.
//
// An inverted or reversed address is not a nonsense value — it is a PERFECTLY
// VALID address belonging to some other receiver. A shop with two Rockler
// receivers on different DIP settings, or a neighbour's collector within
// 26 feet, is a shop where trying the alternates can start the WRONG MACHINE.
//
// During setup that is acceptable: a person is standing there, watching, with a
// lamp in the outlet, and can stop. Unattended at runtime it is not — and it
// would also break the thing the retry policy exists to say. CollectorPress.h
// gives up after three presses precisely because a blower that will not start is
// a tripped breaker, an unplugged cord or a dead fob battery, and cycling
// through OTHER PEOPLE'S ADDRESSES instead of reporting that is worse than
// useless. A working address does not spontaneously become wrong; if it stops
// working, something physical changed.
//
// PURE. No Arduino.h, no I/O — the caller does the pressing and the watching.
// =============================================================================

#pragma once
#include <cstdint>

namespace topo {

/** How a candidate differs from what the user typed — for telling them after. */
enum class AddrGuess : uint8_t {
    AsEntered = 0,
    Inverted  = 1,   // every rocker read the wrong way round
    Reversed  = 2,   // rocker 1 read as rocker 8
    Both      = 3,   // the switch read upside down, which does both at once
};

inline const char* addrGuessName(AddrGuess g) {
    switch (g) {
        case AddrGuess::AsEntered: return "as entered";
        case AddrGuess::Inverted:  return "with every switch flipped";
        case AddrGuess::Reversed:  return "with the switches numbered the other way";
        case AddrGuess::Both:      return "read upside down";
    }
    return "?";
}

inline uint8_t addrInvert(uint8_t a) { return (uint8_t)(~a); }

inline uint8_t addrReverse(uint8_t a) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) if (a & (1u << i)) r |= (uint8_t)(1u << (7 - i));
    return r;
}

inline uint8_t addrFor(uint8_t entered, AddrGuess g) {
    switch (g) {
        case AddrGuess::AsEntered: return entered;
        case AddrGuess::Inverted:  return addrInvert(entered);
        case AddrGuess::Reversed:  return addrReverse(entered);
        case AddrGuess::Both:      return addrReverse(addrInvert(entered));
    }
    return entered;
}

/**
 * Fill `out` with the distinct addresses worth trying, in order, and return how
 * many there are. `outGuess` receives the matching labels.
 *
 * DEDUPED, and it matters: some addresses collapse. 0b00000000 has only two
 * distinct candidates (0 and 255) because reversing a uniform word changes
 * nothing, and a palindromic word is its own reverse. Trying the same address
 * twice would press the collector twice — which, against a TOGGLE, switches it
 * back off and makes a working address look like a failing one.
 *
 * `out` and `outGuess` must have room for 4.
 */
inline int addrCandidates(uint8_t entered, uint8_t* out, AddrGuess* outGuess) {
    const AddrGuess order[4] = { AddrGuess::AsEntered, AddrGuess::Inverted,
                                 AddrGuess::Reversed,  AddrGuess::Both };
    int n = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t a = addrFor(entered, order[i]);
        bool seen = false;
        for (int j = 0; j < n; j++) if (out[j] == a) { seen = true; break; }
        if (seen) continue;
        out[n]      = a;
        outGuess[n] = order[i];
        n++;
    }
    return n;
}

}  // namespace topo

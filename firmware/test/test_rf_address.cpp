// =============================================================================
// test_rf_address.cpp — host tests for control/RfAddressGuess.h.
//
// NOT half of a pair: nothing in shared/device-model/ transmits, so this exists
// once with nothing to drift against. Same shape as kBinDebounceMs.
//
// The case that earns the file is DEDUPING. Some addresses collapse — reversing
// a uniform word changes nothing, a palindrome is its own reverse — and trying
// the same address twice presses the collector twice. Against a TOGGLE that
// switches it back off, so a WORKING address would look like a failing one and
// the routine would report a wiring fault on a correctly-entered switch.
//
// Build + run via tools/ script `firmware:rfaddr:test`.
// =============================================================================

#include "../control/RfAddressGuess.h"
#include <cstdio>
#include <string>

static int passed = 0, failed = 0;
static void ok(const char* what, bool cond, const std::string& got = "") {
    if (cond) { printf("  ok   %s\n", what); passed++; }
    else      { printf("  FAIL %s%s%s\n", what,
                       got.empty() ? "" : "  got: ", got.c_str()); failed++; }
}

using namespace topo;

int main() {
    printf("\nA1 the two transforms\n");
    {
        ok("invert flips every bit", addrInvert(0b01011110) == 0b10100001);
        ok("reverse swaps rocker 1 and rocker 8", addrReverse(0b01011110) == 0b01111010);
        ok("invert twice is identity", addrInvert(addrInvert(94)) == 94);
        ok("reverse twice is identity", addrReverse(addrReverse(94)) == 94);
        // Reading the switch upside down does BOTH, which is why it is a
        // candidate in its own right rather than a combination nobody makes.
        ok("upside down is reverse of invert",
           addrFor(94, AddrGuess::Both) == addrReverse(addrInvert(94)));
    }

    printf("\nA2 the Rockler's own address gives four distinct tries\n");
    {
        uint8_t a[4]; AddrGuess g[4];
        const int n = addrCandidates(0b01011110, a, g);
        ok("four candidates", n == 4, std::to_string(n));
        ok("as entered first", a[0] == 94 && g[0] == AddrGuess::AsEntered);
        ok("then inverted",    a[1] == 161);
        ok("then reversed",    a[2] == 122);
        ok("then upside down", a[3] == 133);
    }

    printf("\nA3 collapsing addresses are deduped\n");
    {
        // All rockers the same way: reversing changes nothing, so there are only
        // two real candidates. Pressing a duplicate would toggle the collector
        // back off and make a working address look broken.
        uint8_t a[4]; AddrGuess g[4];
        int n = addrCandidates(0x00, a, g);
        ok("all-closed has two candidates, not four", n == 2, std::to_string(n));
        ok("...and they are 0 and 255",
           (a[0] == 0x00 && a[1] == 0xFF));

        n = addrCandidates(0xFF, a, g);
        ok("all-open likewise", n == 2, std::to_string(n));

        // A palindrome is its own reverse, so `reversed` duplicates `as entered`
        // and `both` duplicates `inverted`.
        n = addrCandidates(0b00011000, a, g);
        ok("a palindrome has two", n == 2, std::to_string(n));
        ok("...as entered, then inverted",
           a[0] == 0b00011000 && a[1] == 0b11100111);
    }

    printf("\nA4 every candidate is reachable and distinct where it should be\n");
    {
        // Exhaustive: for all 256 addresses the routine must never emit a
        // duplicate, must always include the entered value first, and must never
        // emit more than four.
        bool sane = true;
        for (int e = 0; e < 256 && sane; e++) {
            uint8_t a[4]; AddrGuess g[4];
            const int n = addrCandidates((uint8_t)e, a, g);
            if (n < 1 || n > 4)          sane = false;
            if (a[0] != (uint8_t)e)      sane = false;
            for (int i = 0; i < n && sane; i++)
                for (int j = i + 1; j < n && sane; j++)
                    if (a[i] == a[j]) sane = false;
        }
        ok("all 256 entered values behave", sane);
    }

    printf("\n%d passed, %d failed\n\n", passed, failed);
    return failed ? 1 : 0;
}

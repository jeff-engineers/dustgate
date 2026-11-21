// =============================================================================
// JoinPlan.h — what a board does while it waits to join the shop's WiFi.
//
// Pure (no Arduino.h) so the host tests can drive it; WiFiProvisioner::begin()
// feeds it what the radio and the DHCP client report and does what it says.
//
// THE CASE THAT SHAPED IT (2026-11-12). A bus board associated in under a
// second, password accepted, and never got an address: every ten seconds the
// router offered 192.168.86.26, the board checked whether anyone else answered
// for it, a guest TV asleep in standby did, and the board declined. The router
// (Google Wifi) offered .26 again, forever. It had leased .26 to the board
// before the board went away for ten days, and the TV had taken it since.
//
// Nobody will be walked through their router to fix that (no static IPs, no
// reservations — jeff). So the board fixes it from its side:
//
//   • DECLINES: after kDeclinesBeforeNewMac declines it changes MAC, to the
//     locally administered twin of its own (WiFiProvisioner), and asks again.
//     To the router that is a new device, and a new device gets a free address.
//     The brain finds boards by name, never by MAC, so nothing else notices.
//   • NEVER ASSOCIATED within kAssocTimeoutMs: the network is not there or
//     refused the password — the setup portal is the right answer, as before.
//   • ASSOCIATED BUT NOTHING ANSWERS DHCP for kQuietDhcpMs: rejoin and keep
//     trying. The portal is a dead end here; the password was accepted, and
//     handing a woodworker a setup page for WiFi that is already right is worse
//     than waiting for the router to come round.
// =============================================================================
#pragma once

namespace joinplan {

constexpr unsigned long kAssocTimeoutMs = 30000;   // never associated → portal
constexpr unsigned long kQuietDhcpMs = 60000;      // associated, no address and no decline → rejoin
constexpr int kDeclinesBeforeNewMac = 2;           // ~10 s apart: two in a row is the router insisting

enum class Act { Wait, Portal, SwitchMac, Rejoin };

// One attempt to join: begins at a WiFi.begin(), and again after a rejoin or a MAC change.
struct Attempt {
    unsigned long startMs = 0;
    bool associated = false;
    int declines = 0;

    void restart(unsigned long nowMs) { startMs = nowMs; associated = false; declines = 0; }

    Act next(unsigned long nowMs) const {
        const unsigned long elapsed = nowMs - startMs;
        if (!associated) return elapsed >= kAssocTimeoutMs ? Act::Portal : Act::Wait;
        if (declines >= kDeclinesBeforeNewMac) return Act::SwitchMac;
        return elapsed >= kQuietDhcpMs ? Act::Rejoin : Act::Wait;
    }
};

// The strongest signal heard while joining, from the RSSI the driver reports on each drop. Below kWeakRssiDbm the
// board is hearing the router at the edge of reception, so AUTH_EXPIRE / ASSOC_EXPIRE mean frames are not getting
// through, and the cause is almost always a missing or unseated antenna, not a slow router (2026-11-21: every drop
// at -91 to -99 dBm, nothing on the U.FL). The failure message says that instead of "raise the timeout".
constexpr int kWeakRssiDbm = -85;
constexpr int kNoRssi = -128;   // nothing heard yet

struct Signal {
    int best = kNoRssi;
    void reset() { best = kNoRssi; }
    // 0 and above is the driver's "not measured", never a reading.
    void heard(int rssi) { if (rssi < 0 && rssi > best) best = rssi; }
    bool weak() const { return best != kNoRssi && best < kWeakRssiDbm; }
};

// A board's other MAC: its own with the locally administered bit set (bit 1 of
// the first byte), multicast bit left clear. The same twin every time, so the
// router sees one stable second device rather than a new one per boot.
inline void twinMac(const unsigned char in[6], unsigned char out[6]) {
    for (int i = 0; i < 6; i++) out[i] = in[i];
    out[0] = (unsigned char)((in[0] | 0x02) & ~0x01);
}

}  // namespace joinplan

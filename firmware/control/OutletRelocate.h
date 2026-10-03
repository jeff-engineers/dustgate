// =============================================================================
// control/OutletRelocate.h — find a plug again after its address changed.
//
// WHY. The layout names a plug by IP and nothing else, and a Tasmota advertises
// nothing, so a DHCP renewal or a router swap loses the plug for good: the brain
// polls the old address for ever and the tool it senses is never "on" (found
// 2026-10-03 — "Miter Saw" at 192.168.86.57, the plug long since elsewhere).
//
// WHAT IDENTIFIES A PLUG is its MAC address: it survives a new lease, it is on the
// label, and Tasmota reports it (Status 5). It is stored in the layout beside the
// IP when a plug is paired, and backfilled for plugs paired before that.
//
// TIERS, cheapest first. Only the last exists today:
//   1. (future) resolve the plug's own name over mDNS — if Tasmotas are ever
//      flashed to advertise (jeff is considering it). Then a sweep is the fallback
//      rather than the only way, and the MAC check below still confirms the answer.
//   2. a sweep of the brain's own subnet, matched by MAC.
//   3. no MAC on file: pair by ELIMINATION — exactly one plug nobody has claimed
//      turned up, and exactly one plug in the layout is lost and has no MAC.
//      Anything less certain is left alone and reported; a wrong pairing means a
//      tool silently starting the wrong collector.
//
// Pure — no Arduino, no ArduinoJson — so the host tests drive it.
// =============================================================================
#pragma once
#include <string>
#include <vector>

namespace relocate {

struct Outlet {            // one plug the layout names
    std::string id;        // the machine that owns it
    std::string ip;
    std::string mac;       // normalised; "" = not recorded yet
};

struct Found {             // one Tasmota a sweep turned up
    std::string ip;
    std::string mac;       // normalised; "" = it would not say
    bool        pickable;  // ours or unowned — never someone else's
};

struct Result {
    std::string id;
    std::string ip;        // where it is now
    std::string mac;
    bool        byElimination;
};

// "aa:bb:cc:dd:ee:ff", "AA-BB-..." and "AABBCCDDEEFF" all mean the same plug.
// Returns "" for anything that is not six bytes of hex.
inline std::string normMac(const std::string& in) {
    std::string hex;
    for (char c : in) {
        if (c == ':' || c == '-' || c == '.' || c == ' ') continue;
        if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
        const bool isHex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
        if (!isHex) return "";
        hex += c;
    }
    if (hex.size() != 12) return "";
    std::string out;
    for (size_t i = 0; i < 12; i += 2) { if (i) out += ':'; out += hex.substr(i, 2); }
    return out;
}

// `lost` are the layout's plugs that stopped answering; `all` is every plug the
// layout names (lost or not), so a plug that turns up already belongs to someone
// is never handed to a second tool.
inline std::vector<Result> match(const std::vector<Outlet>& lost,
                                 const std::vector<Outlet>& all,
                                 const std::vector<Found>& found) {
    std::vector<Result> out;
    std::vector<bool> used(found.size(), false);

    for (const Outlet& l : lost) {
        if (l.mac.empty()) continue;
        for (size_t i = 0; i < found.size(); i++) {
            if (found[i].mac != l.mac || !found[i].pickable) continue;
            used[i] = true;
            if (found[i].ip != l.ip) out.push_back({l.id, found[i].ip, l.mac, false});
            break;
        }
    }

    std::vector<const Outlet*> macless;
    for (const Outlet& l : lost) if (l.mac.empty()) macless.push_back(&l);
    if (macless.size() != 1) return out;

    long unclaimed = -1; int n = 0;
    for (size_t i = 0; i < found.size(); i++) {
        if (used[i] || !found[i].pickable) continue;
        bool taken = false;
        for (const Outlet& a : all)
            if (a.ip == found[i].ip || (!a.mac.empty() && a.mac == found[i].mac)) { taken = true; break; }
        if (taken) continue;
        unclaimed = (long)i; n++;
    }
    if (n == 1) out.push_back({macless[0]->id, found[unclaimed].ip, found[unclaimed].mac, true});
    return out;
}

}  // namespace relocate

// =============================================================================
// control/NodeLinkPlan.h — which links to stop and which to start when the set of
// paired nodes changes.
//
// WHY. syncPairedNodes() used to stop EVERY link and dial them all again whenever
// anything about pairing changed. On 2026-10-03, pairing a 3rd and then a 4th node
// dropped every already-linked node each time (`Link lost` x3), took the heap to
// ~2 KB while the tasks and sockets were rebuilt, and left the shop with no
// commandable gates for the seconds it took. A healthy link has no business being
// touched because ANOTHER board was added.
//
// The rule: keep a link whose host is still paired, stop one that is not, dial one
// that is new. A host named for a TAKEOVER is restarted, because its claim rides the
// HELLO that goes out on connect.
//
// Pure — no Arduino — so the host tests drive it.
// =============================================================================
#pragma once
#include <string>
#include <vector>

namespace nodelinks {

struct Plan {
    std::vector<std::string> stop;    // live links to end
    std::vector<std::string> start;   // paired hosts that need a new link
    std::vector<std::string> keep;    // left exactly as they are
};

inline bool has(const std::vector<std::string>& v, const std::string& s) {
    for (const std::string& x : v) if (x == s) return true;
    return false;
}

// `live`: hosts with a running link now. `wanted`: hosts the registry pairs.
// `restart`: a host that must be re-dialled even though it stays paired ("" = none).
inline Plan plan(const std::vector<std::string>& live,
                 const std::vector<std::string>& wanted,
                 const std::string& restart) {
    Plan p;
    for (const std::string& h : live) {
        if (!has(wanted, h) || (!restart.empty() && h == restart)) p.stop.push_back(h);
        else p.keep.push_back(h);
    }
    for (const std::string& h : wanted) {
        if (h.empty() || has(p.keep, h) || has(p.start, h)) continue;   // a duplicate host dials once
        p.start.push_back(h);
    }
    return p;
}

}  // namespace nodelinks

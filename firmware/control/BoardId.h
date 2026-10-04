// =============================================================================
// control/BoardId.h — ONE rule for "which board is this controllerId?"
//
// Five places used to decide whether a controllerId meant THIS board or a given node,
// and they did not agree: NodeBus normalised the host, TopologyRuntime::sameBoard did,
// localBinSystemId compared EXACTLY (its own comment said it mirrored NodeBus "deliberately"
// and then said it did not normalise), and the RF board check added on 2026-10-04 compared
// exactly as well. The same node is legitimately "node-1" (what you paired, what mDNS
// advertises) and "node-1.local" (what you must dial, and so what /api/nodes reports and
// what the UI writes back into link.host), so a rule that is exact in ONE place is a shop
// that routes every gate and drops every jog, or watches a bin on the wrong board, with the
// link green the whole time. This header is the only place that rule is written.
//
// The shared-core work needs it for a second reason: board identity is decided INSIDE the
// core, so it has to be one rule before the core is built for a second platform.
//
// PURE — STL only. `bareHost` has two JS twins (tools/mock-api.js, the UI's board-setup);
// the rule is the same and the C++ is the reference. CLAUDE.md's pair table lists it.
// =============================================================================
#pragma once
#include <cctype>
#include <string>

namespace topo {

// ONE canonical spelling for a board's address. Case-insensitive (mDNS names are), a
// trailing dot (a fully-qualified name is "host.local.") and the ".local" suffix removed.
inline std::string bareHost(const char* h) {
    if (!h) return std::string();
    std::string s(h);
    if (!s.empty() && s.back() == '.') s.pop_back();
    const std::string suffix = ".local";
    if (s.size() > suffix.size()) {
        std::string tail = s.substr(s.size() - suffix.size());
        for (char& c : tail) c = (char)tolower((unsigned char)c);
        if (tail == suffix) s.erase(s.size() - suffix.size());
    }
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// Does this controllerId mean THIS board? Absent (empty) means this board — the rule every
// selector, sensor and the RF transmitter follow — and so does this board's own id, in
// whichever spelling. A board that does not yet know its own id (`ownId` empty) claims
// nothing that names a board: not knowing who you are is a reason to claim nothing.
inline bool isOwnBoard(const std::string& controllerId, const std::string& ownId) {
    return controllerId.empty() || bareHost(controllerId.c_str()) == bareHost(ownId.c_str());
}

// Do two controllerIds name the same board? Both meaning "this board" counts, whatever they
// are spelled; otherwise it is the normalised host.
inline bool sameBoard(const std::string& a, const std::string& b, const std::string& ownId) {
    if (isOwnBoard(a, ownId) && isOwnBoard(b, ownId)) return true;
    return bareHost(a.c_str()) == bareHost(b.c_str());
}

}  // namespace topo

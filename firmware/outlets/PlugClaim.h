#pragma once
// =============================================================================
// PlugClaim.h — who owns a smart plug, and what we're allowed to do to it.
//
// The C++ twin of shared/device-model/plug-claim.js (docs/shop-schema-rfc.md
// §8). Same four states, same rule, case for case — the paired tests
// (plug-claim.test.js / test_plugclaim.cpp) are the anti-drift mechanism, since
// the firmware can't import the JS.
//
// THE RULE: a plug already pointed at another controller is pairable READ-ONLY,
// by polling. We never rewrite its `Ws` config. Pairing repoints push via
// Ws.SetConfig, which silently steals the plug — the previous owner just stops
// hearing that the tool started, with no error on either side. Push is an
// optimization (latency, less traffic), not a capability: polling already gets
// us the one thing a sensor plug is for.
//
// PURE. No Arduino.h, no I/O — std::string only, so it runs on a host.
// =============================================================================

#include <string>
#include <cstddef>

namespace plugclaim {

// U+00B7 MIDDLE DOT, spaced, as UTF-8. Chosen over a hyphen because tool names
// contain hyphens ("Table Saw - shop") and the parse has to be unambiguous.
inline const char* ownerSep() { return " \xC2\xB7 "; }

// The path a DustGate brain's push URL always ends in. It is what separates
// "another DustGate has this" (refuse) from "Home Assistant has this" (share).
inline const char* dustgateWsPath() { return "/shelly-rpc"; }

enum class State {
    Unclaimed,   // nothing is pushing anywhere — the out-of-the-box case
    Ours,        // pushing at us
    Dustgate,    // another brain owns it. NOT pickable.
    Foreign,     // something else on the LAN listens. Pickable, by polling.
};

struct Claim {
    State       state = State::Unclaimed;
    std::string owner;     // from the name suffix, "" if none
    std::string label;     // the name with any owner suffix stripped
    bool        pickable = true;
    bool        repoint  = true;   // may an UNATTENDED pass write its Ws config?
    // Someone else has it, but a human who has been told what breaks may take
    // it. Refusing outright would only push the user into the Shelly app, where
    // the same repoint happens with no record of it at all.
    bool        takeable = false;
    std::string holder;    // who has it now, for the confirmation text
    bool        stale    = false;   // ours, at an address we no longer have
    std::string reason;    // why not pickable, or how it will be paired
};

/**
 * May we write this plug's Ws config?
 *
 * `confirmed` is the user's explicit takeover approval and NOTHING ELSE may set
 * it — in particular the poll task always passes false. That is what makes
 * silent theft structurally impossible rather than merely discouraged.
 */
inline bool mayRepoint(const Claim& c, bool confirmed = false) {
    if (c.repoint) return true;
    return confirmed && c.takeable;
}

/** "Table Saw" + "dustgate-big" → "Table Saw · dustgate-big". */
inline std::string formatName(const std::string& label, const std::string& owner) {
    if (owner.empty()) return label;
    if (label.empty())  return owner;
    return label + ownerSep() + owner;
}

/** Split on the LAST separator — our suffix is always last. */
inline void parseName(const std::string& name, std::string& label, std::string& owner) {
    const std::string sep = ownerSep();
    const size_t i = name.rfind(sep);
    if (i == std::string::npos) { label = name; owner.clear(); return; }
    label = name.substr(0, i);
    owner = name.substr(i + sep.size());
}

inline std::string toLower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

/** "ws://10.0.0.5:80/shelly-rpc" → "10.0.0.5". Lowercased. */
inline std::string wsHost(const std::string& url) {
    const size_t s = url.find("://");
    if (s == std::string::npos) return "";
    const std::string rest = url.substr(s + 3);
    const size_t end = rest.find_first_of("/:?");
    return toLower(end == std::string::npos ? rest : rest.substr(0, end));
}

/** "ws://10.0.0.5:80/shelly-rpc" → "/shelly-rpc". */
inline std::string wsPath(const std::string& url) {
    const size_t s = url.find("://");
    const std::string rest = (s == std::string::npos) ? url : url.substr(s + 3);
    const size_t i = rest.find('/');
    return i == std::string::npos ? "" : rest.substr(i);
}

/**
 * @param pushUrl      the plug's configured Ws.server ("" if unset)
 * @param pushEnabled  Ws.enable. A server with enable:false is a leftover, not
 *                     an owner — treat it as unclaimed.
 * @param ourHost      our own IP/host, as it appears in OUR push URL
 * @param name         the plug's current friendly name
 */
inline Claim decide(const std::string& pushUrl, bool pushEnabled,
                    const std::string& ourHost, const std::string& name,
                    const std::string& ourName = "") {
    Claim c;
    parseName(name, c.label, c.owner);

    const std::string url  = pushEnabled ? pushUrl : std::string();
    const std::string host = wsHost(url);
    const std::string ours = toLower(ourHost);

    if (host.empty()) {
        c.state = State::Unclaimed;
        return c;                       // pickable, repointable — defaults
    }
    if (host == ours) {
        c.state = State::Ours;
        return c;
    }
    if (wsPath(url) == dustgateWsPath()) {
        // A DustGate brain — but WHICH one? Our own address moves: a DHCP lease
        // renewal leaves every plug we own dialing an IP we no longer have. On
        // address alone our whole shop would read as someone else's property and
        // we would refuse to repair it, which is precisely what
        // SmartOutletControl::checkLocalIpChange() exists to do.
        //
        // The NAME breaks that tie, under two conditions that keep it honest:
        // only among targets already known to be DustGate brains, and only for
        // our own exact hostname.
        if (!ourName.empty() && c.owner == ourName) {
            c.state  = State::Ours;
            c.stale  = true;
            c.reason = "pointed at a stale address (" + host + ") \xE2\x80\x94 will be repaired";
            return c;
        }
        // Another brain. Refused, and NAMED: "not on offer" with no reason is
        // indistinguishable from a plug that failed to answer discovery.
        c.state    = State::Dustgate;
        c.pickable = false;
        c.repoint  = false;
        c.takeable = true;
        c.holder   = c.owner.empty() ? host : c.owner;
        c.reason   = "owned by " + c.holder;
        return c;
    }
    // Home Assistant, Node-RED, a script. Both owners keep working and neither
    // is told a lie: we poll, and leave its Ws config exactly as we found it.
    c.state    = State::Foreign;
    c.pickable = true;
    c.repoint  = false;
    c.takeable = true;
    c.holder   = host;
    c.reason   = "shared with " + host + " \xE2\x80\x94 polled, not pushed";
    return c;
}

/** Wire/UI spelling of a state — must match plug-claim.js. */
inline const char* stateName(State s) {
    switch (s) {
        case State::Ours:      return "ours";
        case State::Dustgate:  return "dustgate";
        case State::Foreign:   return "foreign";
        case State::Unclaimed:
        default:               return "unclaimed";
    }
}

}  // namespace plugclaim

// =============================================================================
// control/OutletRelocate.h — is the plug at a saved address still the plug the layout means, and if not, where did it go?
//
// WHY. The layout names a plug by IP, and an IP is only a lease. Two things go wrong with it, and the second is worse:
//   * the plug moves (a DHCP renewal, a router swap): the brain polls an address nobody answers, and the tool it senses
//     is never "on" (2026-10-03 — "Miter Saw" at 192.168.86.57, the plug long since elsewhere);
//   * ANOTHER plug is given the old address: the brain reads the wrong tool, claims a plug that is not its, and the app
//     hides the newcomer as already taken (2026-10-18 — the Table Saw's Shelly gone from .64, a new Tasmota leased .64).
//
// WHAT IDENTIFIES A PLUG is its MAC address: it survives a new lease and a reflash, it is on the label, and both kinds
// report it (Tasmota Status 5, Shelly.GetDeviceInfo). It is stored in the layout beside the IP when a plug is paired, and
// backfilled for a plug paired without one. With no MAC, the KIND is the only evidence: a Tasmota answering where the layout
// saved a Shelly is certainly a different plug.
//
// THE POLICY IS HERE, NOT IN A SHELL. Both brains run PlugRelocator; each shell only feeds it what its plugs are doing,
// gives it a way to probe an address and run a sweep, and applies the rewrites it returns. The loop that used to drive
// this lived in firmware.ino alone, and the Pi brain never relocated anything (tools/check-brain-parity.js now fails on
// that). The JS model shares samePlug() as plug-identity.js; test_relocate.cpp's "S" block and plug-identity.test.js
// are a matched pair, same cases, same order.
//
// Pure — STL and ArduinoJson only — so the host tests drive it.
// =============================================================================
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <ArduinoJson.h>

namespace relocate {

struct Outlet {            // one plug the layout names
    std::string id;        // what owns it (the shell's key)
    std::string ip;
    std::string mac;       // normalised; "" = not recorded yet
    std::string kind;      // "shelly" / "tasmota"; "" = shelly
};

struct Found {             // one plug a sweep turned up
    std::string ip;
    std::string mac;       // normalised; "" = it would not say
    bool        pickable;  // ours or unowned — never someone else's
    std::string kind;      // "" = shelly
};

struct Result {
    std::string id;
    std::string ip;        // where it is now
    std::string mac;
    bool        byElimination;
    std::string kind;      // what it is now — a reflashed plug keeps its MAC and changes kind
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

// The layout's default when it does not say (outletKindFromName): a Shelly.
inline std::string normKind(const std::string& k) { return k == "tasmota" ? "tasmota" : "shelly"; }

// ── is it the same plug? ─────────────────────────────────────────────────────
enum class Same { Yes, No, Unknown };

// `saved` is what the layout recorded; `seen` is what answered at that address just now.
//   * nothing answered:                    Unknown — a plug switched off is not a plug replaced
//   * both MACs known:                     they decide, whatever the kinds say (a reflash keeps its MAC)
//   * the kinds differ, no MAC to compare: No — a Tasmota is never the Shelly the layout saved
//   * a MAC on file, the plug would not say one: Unknown — never accuse a plug on a failed read
//   * no MAC on file, same kind:           Unknown — nothing to tell them apart by
inline Same samePlug(const std::string& savedKind, const std::string& savedMac,
                     bool reachable, const std::string& seenKind, const std::string& seenMac) {
    if (!reachable) return Same::Unknown;
    const std::string a = normMac(savedMac), b = normMac(seenMac);
    if (!a.empty() && !b.empty()) return a == b ? Same::Yes : Same::No;
    if (normKind(savedKind) != normKind(seenKind)) return Same::No;
    return Same::Unknown;
}

// `lost` are the layout's plugs that stopped answering (or were replaced); `all` is every plug the layout names (lost or
// not), so a plug that turns up already belonging to someone is never handed to a second tool.
//
// By ELIMINATION only when certain: exactly one lost plug with no MAC, and exactly one stranger OF THE SAME KIND that
// nobody has claimed. A Tasmota is never paired by elimination to a lost Shelly — that is the mistake this file exists to
// stop. Anything less certain is left alone and reported; a wrong pairing means a tool silently starting the wrong collector.
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
            if (found[i].ip != l.ip || normKind(found[i].kind) != normKind(l.kind))
                out.push_back({l.id, found[i].ip, l.mac, false, normKind(found[i].kind)});
            break;
        }
    }

    std::vector<const Outlet*> macless;
    for (const Outlet& l : lost) if (l.mac.empty()) macless.push_back(&l);
    if (macless.size() != 1) return out;

    long unclaimed = -1; int n = 0;
    for (size_t i = 0; i < found.size(); i++) {
        if (used[i] || !found[i].pickable) continue;
        if (normKind(found[i].kind) != normKind(macless[0]->kind)) continue;
        bool taken = false;
        for (const Outlet& a : all)
            if (a.ip == found[i].ip || (!a.mac.empty() && a.mac == found[i].mac)) { taken = true; break; }
        if (taken) continue;
        unclaimed = (long)i; n++;
    }
    if (n == 1) out.push_back({macless[0]->id, found[unclaimed].ip, found[unclaimed].mac, true, normKind(found[unclaimed].kind)});
    return out;
}

// ── the policy both brains run ───────────────────────────────────────────────

// One plug the layout names, as the shell sees it right now.
struct Plug {
    std::string key;        // the shell's name for it: "m:<machine>", "c:<system>" (a collector's sense plug), "s:<system>" (its switch)
    std::string ip, kind, mac;
    bool        reachable = false;   // the shell's own poll of it (with the saved kind's driver) answered
};

// What answered at an address: the shell's probe, trying both protocols (outletops::describe).
struct Seen { bool reachable = false; std::string kind, mac; };

// What a shell gives the policy to work with. Every call may block (HTTP); a shell runs the policy where that is allowed.
class Probe {
public:
    virtual ~Probe() {}
    virtual Seen describe(const std::string& ip) = 0;
    virtual bool sweepRunning() = 0;
    virtual bool startSweep() = 0;
    virtual uint32_t sweepsFinished() = 0;           // counts finished sweeps, whoever started them
    virtual std::vector<Found> sweepRows() = 0;      // the last finished sweep's plugs
};

// A change to make to the layout: point `key`'s plug at this address / kind / MAC.
struct Rewrite { std::string key, ip, kind, mac, why; };

static constexpr uint32_t kProbeSilentAfterMs = 60000;    // silent this long: ask the address what is there now
static constexpr uint32_t kRelocateAfterMs    = 300000;   // silent this long (or replaced): look for it on the subnet
static constexpr uint32_t kRelocateMinGapMs   = 900000;   // and never sweep for it more often than this
static constexpr uint32_t kRecheckUnverifiedMs = 10000;  // a plug not yet checked (so not yet polled) is asked again this soon
static constexpr uint32_t kReverifyMs         = 600000;   // a plug that answers is checked again this often: two plugs can swap leases

class PlugRelocator {
public:
    // `maxProbes` bounds how many addresses one tick may ask: the ESP32 runs this on its main loop (1); the Pi on a
    // thread of its own (more, so a boot's worth of plugs is checked in one pass).
    explicit PlugRelocator(int maxProbes = 1) : _maxProbes(maxProbes) {}

    // May the shell poll and claim this plug as `key`'s? Not when a different plug is at its address, and not before a
    // plug with a MAC on file has been checked once — two plugs that swapped leases overnight would otherwise read each
    // other's tools, and claiming writes to a plug. A plug with no MAC cannot be checked, so it is trusted (and its MAC
    // is recorded the first time it answers).
    // `macIfUnseen`: a key this policy has not ticked yet is trusted only if the layout gives it no MAC to check.
    bool trusted(const std::string& key, const std::string& macIfUnseen = "") const {
        auto it = _t.find(key);
        if (it == _t.end()) return normMac(macIfUnseen).empty();
        return !it->second.replaced && (it->second.mac.empty() || it->second.verified);
    }
    // A different plug answers at this one's address: what it is, for the problem a person sees ("" = not replaced).
    std::string replacedBy(const std::string& key) const {
        auto it = _t.find(key);
        return it == _t.end() || !it->second.replaced ? std::string() : it->second.replacedBy;
    }

    std::vector<Rewrite> tick(const std::vector<Plug>& plugs, Probe& probe, uint32_t now, std::vector<std::string>* log = nullptr) {
        std::vector<Rewrite> out;
        auto say = [&](const std::string& s) { if (log) log->push_back(s); };

        // Track what the layout names now; a key whose address or MAC changed starts again.
        std::map<std::string, Track> next;
        for (const Plug& p : plugs) {
            Track t;
            auto it = _t.find(p.key);
            auto ex = _expect.find(p.key);
            if (it != _t.end() && it->second.ip == p.ip && it->second.mac == normMac(p.mac) && it->second.kind == normKind(p.kind)) t = it->second;
            // A rewrite of OURS landing: the plug at the new address is the one we just checked, so it does not start untrusted.
            else if (ex != _expect.end() && ex->second.ip == p.ip && ex->second.mac == normMac(p.mac) && ex->second.kind == normKind(p.kind)) t = ex->second;
            t.ip = p.ip; t.mac = normMac(p.mac); t.kind = normKind(p.kind);
            if (p.reachable && !t.replaced) t.downSince = 0;
            else if (!t.downSince) t.downSince = now ? now : 1;
            next[p.key] = t;
        }
        _t.swap(next);
        // An expectation the layout has taken (or that it never took within a minute) is spent.
        for (auto e = _expect.begin(); e != _expect.end();) {
            auto it = _t.find(e->first);
            const bool landed = it != _t.end() && it->second.ip == e->second.ip && it->second.mac == e->second.mac;
            if (landed || now - e->second.verifiedAt > 60000) e = _expect.erase(e); else ++e;
        }

        // 1. A sweep finished (ours or a person's): it says what is at every address it found.
        const uint32_t done = probe.sweepsFinished();
        if (done != _sweepsSeen) {
            _sweepsSeen = done;
            const std::vector<Found> rows = probe.sweepRows();
            for (const Plug& p : plugs) {
                for (const Found& f : rows) if (f.ip == p.ip) judge(p.key, true, f.kind, f.mac, now, say);
            }
            std::vector<Outlet> lost, all;
            for (const Plug& p : plugs) {
                const Track& t = _t[p.key];
                all.push_back({p.key, t.replaced ? std::string() : p.ip, t.mac, t.kind});
                if (isLost(t, now)) lost.push_back({p.key, p.ip, t.mac, t.kind});
            }
            for (const Result& r : match(lost, all, rows)) {
                say("[RELOCATE] " + r.id + " found at " + r.ip + " (MAC " + (r.mac.empty() ? "none" : r.mac) + ")" +
                    (r.byElimination ? " - the only unclaimed plug of its kind, so it must be this one; layout updated."
                                     : " - matched by MAC; layout updated."));
                out.push_back({r.id, r.ip, r.kind, r.mac, r.byElimination ? "elimination" : "mac"});
            }
            if (out.empty() && !lost.empty() && _ownSweep)
                say("[RELOCATE] swept the subnet, found " + std::to_string(rows.size()) +
                    " plug(s) and could not tell which is the lost one - leaving the layout alone.");
            _ownSweep = false;
            if (!out.empty()) return expect(out, now);
        }

        // 2. Ask a few addresses what is there now, most urgent first.
        int budget = _maxProbes;
        for (int pass = 0; pass < 3 && budget > 0; pass++) {
            for (const Plug& p : plugs) {
                if (budget <= 0) break;
                Track& t = _t[p.key];
                // A plug waiting on its first check is held off the poller, so it is asked again sooner.
                const uint32_t gap = (!t.replaced && !t.mac.empty() && !t.verified) ? kRecheckUnverifiedMs : kProbeSilentAfterMs;
                if (t.probedAt && now - t.probedAt < gap) continue;
                const bool due =
                    pass == 0 ? (!t.replaced && !t.mac.empty() && (!t.verified || now - t.verifiedAt >= kReverifyMs))   // check a MAC on file
                  : pass == 1 ? (!t.replaced && p.reachable && t.mac.empty())                                            // record a missing MAC
                  // a silent plug — and a replaced one, which is not polled: its own plug may come back to the address
                  :             (t.downSince && now - t.downSince >= kProbeSilentAfterMs);
                if (!due) continue;
                budget--;
                t.probedAt = now ? now : 1;
                const Seen s = probe.describe(p.ip);
                if (pass == 1 && s.reachable && normKind(s.kind) == t.kind && !normMac(s.mac).empty()) {
                    say("[RELOCATE] recording the MAC of " + p.key + " (" + normMac(s.mac) + ") so it can be found if its address changes.");
                    out.push_back({p.key, p.ip, t.kind, normMac(s.mac), "backfill"});
                    continue;
                }
                const Same v = judge(p.key, s.reachable, s.kind, s.mac, now, say);
                // The same plug by its MAC, but it now speaks the other protocol: reflashed. Say so in the layout.
                if (v == Same::Yes && normKind(s.kind) != t.kind) {
                    say("[RELOCATE] " + p.key + " at " + p.ip + " is the same plug, reflashed as a " + normKind(s.kind) + "; correcting the layout.");
                    out.push_back({p.key, p.ip, normKind(s.kind), t.mac, "reflashed"});
                }
            }
        }
        if (!out.empty()) return expect(out, now);

        // 3. Something is lost: look for it, rate-limited (a sweep on the ESP32 is a minute of probing).
        if (probe.sweepRunning()) return out;
        for (const Plug& p : plugs) {
            if (!isLost(_t[p.key], now)) continue;
            if (_lastSweepMs && now - _lastSweepMs < kRelocateMinGapMs) break;
            if (probe.startSweep()) {
                _ownSweep = true; _lastSweepMs = now ? now : 1;
                say("[RELOCATE] " + p.key + " at " + p.ip + (_t[p.key].replaced ? " has been replaced by a different plug" : " has not answered for 5 min") +
                    " - sweeping the subnet for it.");
            }
            break;
        }
        return out;
    }

private:
    struct Track {
        std::string ip, mac, kind;
        uint32_t downSince = 0, probedAt = 0, verifiedAt = 0;
        bool verified = false, replaced = false;
        std::string replacedBy;
    };
    std::vector<Rewrite>& expect(std::vector<Rewrite>& out, uint32_t now) {
        for (const Rewrite& r : out) {
            Track t; t.ip = r.ip; t.mac = normMac(r.mac); t.kind = normKind(r.kind);
            t.verified = !t.mac.empty(); t.verifiedAt = now ? now : 1; t.probedAt = now ? now : 1;
            _expect[r.key] = t;
        }
        return out;
    }
    static bool isLost(const Track& t, uint32_t now) {
        return t.replaced || (t.downSince && now - t.downSince >= kRelocateAfterMs);
    }
    template <typename Say>
    Same judge(const std::string& key, bool reachable, const std::string& kind, const std::string& mac, uint32_t now, Say& say) {
        Track& t = _t[key];
        const Same v = samePlug(t.kind, t.mac, reachable, kind, mac);
        if (v == Same::Yes) { t.verified = true; t.verifiedAt = now ? now : 1; t.replaced = false; }
        if (v == Same::No && !t.replaced) {
            t.replaced = true;
            t.replacedBy = "a " + normKind(kind) + (normMac(mac).empty() ? std::string() : " (MAC " + normMac(mac) + ")");
            say("[RELOCATE] " + key + ": a different plug answers at " + t.ip + " - " + t.replacedBy + ", where the layout has a " +
                t.kind + (t.mac.empty() ? std::string() : " (MAC " + t.mac + ")") + ". Not polling or claiming it as " + key + ".");
        }
        return v;
    }
    int _maxProbes;
    std::map<std::string, Track> _t, _expect;
    uint32_t _sweepsSeen = 0, _lastSweepMs = 0;
    bool _ownSweep = false;
};

// Apply a Rewrite to a layout document. Keys are PlugRelocator's ("m:", "c:", "s:"). Returns false if the key names
// nothing in this layout. `gen` follows the kind: a Tasmota has no Shelly generation, and an entry carrying gen 2 beside a
// Tasmota is a Shelly's shape describing a plug that has never heard of one.
inline bool applyRewrite(JsonObject root, const Rewrite& r) {
    JsonObject outlet;
    const std::string id = r.key.size() > 2 ? r.key.substr(2) : std::string();
    if (r.key.rfind("m:", 0) == 0) {
        for (JsonObject m : root["machines"].as<JsonArray>()) if (id == (m["id"] | "")) { outlet = m["sensor"]["outlet"]; break; }
    } else if (r.key.rfind("c:", 0) == 0 || r.key.rfind("s:", 0) == 0) {
        for (JsonObject sys : root["systems"].as<JsonArray>()) {
            if (id != (sys["id"] | "")) continue;
            for (JsonObject e : sys["elements"].as<JsonArray>()) {
                if (std::string(e["type"] | "") != "collector") continue;
                outlet = r.key[0] == 'c' ? e["sensor"]["outlet"] : e["control"]["outlet"];
                break;
            }
        }
    }
    if (outlet.isNull()) return false;
    outlet["ip"] = r.ip;
    if (normKind(r.kind) == "tasmota") { outlet["kind"] = "tasmota"; outlet["gen"] = 0; }
    else { outlet.remove("kind"); if ((outlet["gen"] | 0) < 2) outlet["gen"] = 2; }
    if (!r.mac.empty()) outlet["mac"] = r.mac;
    return true;
}

}  // namespace relocate

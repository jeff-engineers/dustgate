// =============================================================================
// test_plugclaim.cpp — host test for PlugClaim.h.
//
// Case-for-case with shared/device-model/plug-claim.test.js. The two engines
// can't share code, so these paired assertions are the only thing keeping the
// device's answer and the configurator's answer the same — same discipline as
// test_nodebus.cpp against nodelink.test.js.
//
// Build + run:
//   c++ -std=c++17 firmware/test/test_plugclaim.cpp -o /tmp/pctest && /tmp/pctest
// (or the tools/ script `firmware:plugclaim:test`)
// =============================================================================
#include <cstdio>
#include "../outlets/PlugClaim.h"

using namespace plugclaim;

static int passed = 0, failed = 0;

static void ok(const char* what, bool cond) {
    if (cond) { printf("  ✓ %s\n", what); passed++; }
    else      { printf("  ✗ %s\n", what); failed++; }
}
static void eqs(const char* what, const std::string& got, const std::string& want) {
    if (got == want) { printf("  ✓ %s\n", what); passed++; }
    else { printf("  ✗ %s — got \"%s\" want \"%s\"\n", what, got.c_str(), want.c_str()); failed++; }
}

// Our brain, as it appears in the URL plugs dial back to.
static const char* kOurHost = "10.0.0.2";
static const char* kOurName = "dustgate-shop";
static const std::string kOurUrl = "ws://10.0.0.2:80/shelly-rpc";

int main() {
    printf("\n== PlugClaim ==\n");

    // ── names carry the owner for HUMANS ───────────────────────────────────
    eqs("format appends the owner", formatName("Table Saw", "dustgate-big"),
        "Table Saw \xC2\xB7 dustgate-big");
    eqs("no owner leaves the label alone", formatName("Table Saw", ""), "Table Saw");
    {
        std::string label, owner;
        parseName(formatName("Table Saw", "dustgate-big"), label, owner);
        eqs("round trip label", label, "Table Saw");
        eqs("round trip owner", owner, "dustgate-big");

        parseName("Table Saw", label, owner);
        eqs("an unsuffixed name has no owner", owner, "");
        eqs("...and is all label", label, "Table Saw");

        // Splits on the LAST separator: a label someone typed with a middle dot
        // is still a label, and our suffix is always last.
        parseName("Saw \xC2\xB7 bench \xC2\xB7 dustgate-big", label, owner);
        eqs("splits on the last separator", label, "Saw \xC2\xB7 bench");
        eqs("...owner is the tail", owner, "dustgate-big");
    }

    // ── URL parts ──────────────────────────────────────────────────────────
    eqs("host from a push URL", wsHost(kOurUrl), "10.0.0.2");
    eqs("host without a port", wsHost("ws://ha.local/api/shelly"), "ha.local");
    eqs("host of nothing", wsHost(""), "");
    eqs("path", wsPath(kOurUrl), "/shelly-rpc");
    eqs("path of a foreign target", wsPath("ws://ha.local/api/shelly/ws"), "/api/shelly/ws");

    // ── the four states (RFC §8.3) ─────────────────────────────────────────
    {
        Claim c = decide("", true, kOurHost, "shellyplus1pm-abc");
        eqs("no push target → unclaimed", stateName(c.state), "unclaimed");
        ok("unclaimed is pickable", c.pickable);
        ok("and we may point it at us", mayRepoint(c));
    }
    {
        Claim c = decide(kOurUrl, true, kOurHost, "Table Saw \xC2\xB7 dustgate-shop");
        eqs("pushing at us → ours", stateName(c.state), "ours");
        ok("ours is pickable", c.pickable);
        ok("ours may be repointed", mayRepoint(c));
        eqs("and the suffix is stripped for display", c.label, "Table Saw");
    }
    {
        // The case the whole file exists for. Pairing here would silently stop
        // the OTHER shop hearing that its saw started.
        Claim c = decide("ws://10.0.0.9:80/shelly-rpc", true, kOurHost,
                         "Bandsaw \xC2\xB7 dustgate-bench");
        eqs("another DustGate → dustgate", stateName(c.state), "dustgate");
        ok("not pickable", !c.pickable);
        ok("and NEVER repointed by an unattended pass", !mayRepoint(c));
        eqs("the reason names the owner", c.reason, "owned by dustgate-bench");
    }
    {
        // Name is a hint, not the authority: with no suffix we still refuse, and
        // say who by address rather than pretending not to know.
        Claim c = decide("ws://10.0.0.9:80/shelly-rpc", true, kOurHost, "Bandsaw");
        eqs("an unnamed peer is still refused", stateName(c.state), "dustgate");
        eqs("named by address instead", c.reason, "owned by 10.0.0.9");
    }
    {
        Claim c = decide("ws://ha.local/api/shelly/ws", true, kOurHost, "Dust Collector");
        eqs("a non-DustGate target → foreign", stateName(c.state), "foreign");
        ok("still pickable — polling needs no permission", c.pickable);
        ok("but its Ws config is left alone", !mayRepoint(c));
        eqs("and the UI says so plainly", c.reason,
            "shared with ha.local \xE2\x80\x94 polled, not pushed");
    }

    // ── enable:false is a leftover, not an owner ───────────────────────────
    {
        Claim c = decide("ws://10.0.0.9:80/shelly-rpc", /*enabled*/false, kOurHost, "Bandsaw");
        eqs("a disabled push target is unclaimed", stateName(c.state), "unclaimed");
        ok("so it can be claimed normally", mayRepoint(c));
    }

    // ── the authority is the URL, not the name ─────────────────────────────
    {
        Claim c = decide(kOurUrl, true, kOurHost, "Table Saw \xC2\xB7 dustgate-bench");
        eqs("a misleading suffix does not hand the plug away", stateName(c.state), "ours");
        ok("still repointable", mayRepoint(c));
    }
    {
        // Our own name, pointed at a DIFFERENT DustGate address: our plug after a
        // DHCP lease renewal. On address alone we would refuse to repair our own
        // shop, so the name breaks the tie — narrowly.
        Claim c = decide("ws://10.0.0.9:80/shelly-rpc", true, kOurHost,
                         "Table Saw \xC2\xB7 dustgate-shop", kOurName);
        eqs("our own suffix at a stale address is still ours", stateName(c.state), "ours");
        ok("flagged stale", c.stale);
        ok("and repointable, which IS the repair", mayRepoint(c));
        eqs("the reason explains the address", c.reason,
            "pointed at a stale address (10.0.0.9) \xE2\x80\x94 will be repaired");
    }
    {
        // Never against a foreign app, however the plug is named.
        Claim c = decide("ws://ha.local/api/shelly/ws", true, kOurHost,
                         "Table Saw \xC2\xB7 dustgate-shop", kOurName);
        eqs("our suffix cannot take a plug from a foreign app", stateName(c.state), "foreign");
        ok("still never repointed unattended", !mayRepoint(c));
    }
    {
        // Never for a suffix naming a DIFFERENT brain.
        Claim c = decide("ws://10.0.0.9:80/shelly-rpc", true, kOurHost,
                         "Table Saw \xC2\xB7 dustgate-bench", kOurName);
        eqs("another brain's suffix is refused", stateName(c.state), "dustgate");
        ok("not repointable", !mayRepoint(c));
    }
    {
        // And with no ourName at all, the address is the only evidence.
        Claim c = decide("ws://10.0.0.9:80/shelly-rpc", true, kOurHost,
                         "Table Saw \xC2\xB7 dustgate-shop");
        eqs("no ourName -> no tie-break", stateName(c.state), "dustgate");
    }

    // -- takeover: allowed, but only by a human who was told what breaks -----
    {
        Claim c = decide("ws://10.0.0.9:80/shelly-rpc", true, kOurHost,
                         "Bandsaw \xC2\xB7 dustgate-bench", kOurName);
        ok("a background pass may never take it", !mayRepoint(c));
        ok("...not even by asking twice", !mayRepoint(c, false));
        ok("a confirmed user may", mayRepoint(c, true));
        ok("it is offered as takeable", c.takeable);
        eqs("and the holder is named", c.holder, "dustgate-bench");
    }
    {
        Claim c = decide("ws://ha.local/api/shelly/ws", true, kOurHost, "Saw", kOurName);
        ok("foreign is takeable with confirmation", mayRepoint(c, true));
        eqs("holder is the foreign host", c.holder, "ha.local");
    }
    {
        Claim mine = decide(kOurUrl, true, kOurHost, "Saw", kOurName);
        Claim free = decide("", true, kOurHost, "Saw", kOurName);
        ok("ours is not takeable", !mine.takeable);
        ok("unclaimed is not takeable", !free.takeable);
    }

    // ── case and port differences are not ownership differences ────────────
    {
        Claim c = decide("ws://10.0.0.2:8080/shelly-rpc", true, kOurHost, "Saw");
        eqs("a different port on our own host is still ours", stateName(c.state), "ours");
    }
    {
        Claim c = decide("ws://dustgate-shop.local:80/shelly-rpc", true, "DustGate-Shop.local", "");
        eqs("host comparison is case-insensitive", stateName(c.state), "ours");
    }

    printf("\n%d/%d passed\n", passed, passed + failed);
    return failed == 0 ? 0 : 1;
}

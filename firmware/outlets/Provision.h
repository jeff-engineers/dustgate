// =============================================================================
// outlets/Provision.h — claim ONE smart plug for this brain: ask who owns it, and only then write to it.
//
// The ESP32 does this inside SmartOutletControl::provisionPushOutlets(), welded to its slot table and its serial log.
// The native brain needs the same rule (RFC §8, outlets/PlugClaim.h) and had no push endpoint to claim a plug FOR, so
// it only ever polled. This is the rule once, with no board in it, so the two brains cannot disagree about when a
// plug may be written. (The ESP's copy is still its own; moving it onto this is a one-bench-run change, TODO.md.)
//
// THE RULES, because each was bought with a real plug:
//   * A FAILED READ IS NOT PERMISSION. If the plug will not say who owns it we leave it alone and try again.
//   * A plug someone else owns is paired READ-ONLY, by polling, unless a person approved a takeover for it. The
//     approval comes from exactly one place — the caller's `takeover` argument — and a background pass passes false.
//   * Name FIRST, then the push target: Ws.SetConfig makes the plug reopen its socket, and a name write landing on
//     top of that is lost.
//   * The push target we displaced is handed back in `previousPushUrl`, so unpairing can restore it.
//
// PURE — the plug drivers, which reach the network only through outlets/PlugHttp.h.
// =============================================================================
#pragma once
#include <string>
#include "OutletOps.h"

namespace outletops {

enum class Provision {
    Done,       // the plug now pushes to us (a Tasmota: carries our claim)
    Pending,    // could not reach or read it — nothing was written, try again later
    PollOnly,   // it belongs to someone else: paired, polled, never written
};

struct ProvisionResult {
    Provision   state = Provision::Pending;
    std::string previousPushUrl;   // set only when a takeover displaced another push target
    std::string why;               // for the log
};

inline ProvisionResult provisionPlug(SmartOutlet& o, const Self& self, const std::string& wsUrl, bool takeover) {
    ProvisionResult r;
    if (!o.probe(OUTLET_PROVISION_PROBE_TIMEOUT_MS)) { r.why = "not answering"; return r; }

    const bool isTasmota = o.kind() == OUTLET_TASMOTA;
    // What the plug calls itself, from the plug: the layout's copy is a label we wrote once and may be stale.
    std::string devName;
    if (isTasmota) static_cast<TasmotaOutlet&>(o).readName(devName);
    else           devName = fetchShellyDeviceName(o.ip(), 2);

    plugclaim::Claim claim;
    std::string pushUrl;
    if (!o.readClaim(self.host.c_str(), devName.c_str(), self.name.c_str(), claim, &pushUrl)) {
        r.why = isTasmota ? "couldn't read Mem1" : "couldn't read Ws config";
        return r;
    }
    if (!plugclaim::mayRepoint(claim, takeover)) {
        o.setPollOnly(true);
        r.state = Provision::PollOnly; r.why = claim.reason;
        return r;
    }
    o.setPollOnly(false);

    if (isTasmota) {
        if (!static_cast<TasmotaOutlet&>(o).provision(self.name.c_str())) { r.why = "Tasmota claim failed"; return r; }
        o.setProvisioned(true);
        r.state = Provision::Done;
        return r;
    }

    if (takeover && claim.takeable) {
        plughttp::log(std::string("[PLUGS] TAKEOVER (user-confirmed) of ") + o.ip() + " from " + claim.holder +
                      " - previous push target: " + pushUrl);
        r.previousPushUrl = pushUrl;
    }
    // Name first: a plug with no name has nothing to suffix, and is claimed by its push target alone.
    if (!devName.empty()) {
        std::string label, owner;
        plugclaim::parseName(devName, label, owner);          // never double-suffix
        o.setName(plugclaim::formatName(label, self.name).c_str());
        plughttp::sleepMs(150);
    }
    if (!o.configureOutboundWs(wsUrl.c_str())) { r.why = "Ws.SetConfig failed"; return r; }
    o.setProvisioned(true);
    r.state = Provision::Done;
    return r;
}

}  // namespace outletops

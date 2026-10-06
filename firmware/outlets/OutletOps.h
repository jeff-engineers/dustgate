// =============================================================================
// outlets/OutletOps.h — what a person does to a smart plug, with no board in it: probe it and describe it for the
// picker, rename it, hand it back. Written as one routine per action because it used to live inside firmware.ino's
// loop-adjacent handlers, welded to the NVS slot table and the serial console — fine for one platform, and the
// reason the native brain could not answer the app's plug screens at all.
//
// WHAT STAYS IN THE SHELL: which stored slot a plug is in (renaming one must also be saved to NVS) and whether we
// only ever POLLED it (a poll-only plug was never written to, so release has nothing to give back). Those are
// arguments or results here, never reads.
//
// PURE — ArduinoJson, STL and the plug drivers (which reach the network only through outlets/PlugHttp.h).
// =============================================================================
#pragma once
#include <string>
#include <ArduinoJson.h>
#include "OutletFactory.h"
#include "ShellyDeviceName.h"
#include "PlugClaim.h"

namespace outletops {

// Who "we" are as far as a plug's claim is concerned: our mDNS host (what a Shelly's push target names) and our
// owner label (the "· name" suffix on a plug's name, and a Tasmota's Mem1 marker).
struct Self { std::string host, name; };

// Probe one plug and write the row the picker reads.
//
// WHEN THE KIND IS UNKNOWN, TRY BOTH. A Shelly answers /rpc/Switch.GetStatus and a Tasmota /cm?cmnd=Status%208; probing
// with the wrong driver reports a live plug as unreachable, and nothing in an address says which it is. Shelly first,
// because it is the default kind everywhere (outletKindFromName); a Tasmota costs one failed Shelly probe.
inline void describe(JsonObject o, const char* ip, const char* mdnsHost, bool kindKnown, OutletKind knownKind,
                     int mdnsGen, const Self& self) {
    ShellyGen2Outlet shellyProbe(ip, "discover");
    TasmotaOutlet    tasProbe(ip, "discover");

    bool isTasmota = kindKnown && (knownKind == OUTLET_TASMOTA);
    bool ok        = false;
    if (kindKnown) {
        SmartOutlet* p = isTasmota ? (SmartOutlet*)&tasProbe : (SmartOutlet*)&shellyProbe;
        ok = p->poll();
    } else {
        ok = shellyProbe.poll();
        if (!ok && tasProbe.poll()) { ok = true; isTasmota = true; }
    }

    // Gen2+ only; Gen3 shares the Gen2 RPC dialect, so one Gen2 probe covers every supported device. A Tasmota has no
    // generation at all (0). A hand-typed Shelly has no advertised gen, so it assumes 2.
    const int apiGen = isTasmota ? 0 : ((mdnsGen >= 3) ? 3 : 2);
    const float pw   = isTasmota ? tasProbe.getPowerW() : shellyProbe.getPowerW();

    // A Tasmota's mDNS hostname is the name its owner set in its own web UI. Added by hand there is none, and the row
    // falls back to the address — the only thing the person typing it knows.
    std::string host = mdnsHost ? mdnsHost : "";
    std::string devName;
    if (isTasmota) {
        // The DeviceName, NOT the hostname: a SWEPT plug has no hostname and used to be labelled with its own address twice.
        if (!ok || !tasProbe.readName(devName)) devName.clear();
    } else if (ok) {
        devName = fetchShellyDeviceName(ip, apiGen);
    }
    if (host.empty()) host = devName.empty() ? std::string(ip) : devName;

    plugclaim::Claim claim;
    // One call for both protocols — SmartOutlet::readClaim(). A Tasmota's claim is Mem1: weaker than a Shelly's in two
    // ways written down at plugclaim::decideMarker().
    const bool claimKnown = ok && (isTasmota ? tasProbe.readClaim(self.host.c_str(), devName.c_str(), self.name.c_str(), claim)
                                             : shellyProbe.readClaim(self.host.c_str(), devName.c_str(), self.name.c_str(), claim));

    // A Tasmota's MAC, so a layout can find the plug again when its address changes (control/OutletRelocate.h).
    if (isTasmota && ok) {
        std::string mac;
        if (tasProbe.readMac(mac, 1200) && !mac.empty()) o["mac"] = mac;
    }

    o["ip"]        = std::string(ip);
    o["hostname"]  = host;
    // The name with any "· owner" suffix stripped: the suffix is our bookkeeping and would otherwise be saved back and doubled.
    o["name"]      = claimKnown ? claim.label : devName;
    o["reachable"] = ok;
    o["powerW"]    = pw;
    o["gen"]       = ok ? apiGen : 0;
    o["kind"]      = std::string(outletKindName(isTasmota ? OUTLET_TASMOTA : OUTLET_SHELLY));
    if (claimKnown) {
        o["claim"]    = std::string(plugclaim::stateName(claim.state));
        o["owner"]    = claim.owner;
        o["holder"]   = claim.holder;
        o["pickable"] = claim.pickable;
        o["takeable"] = claim.takeable;
        if (!claim.reason.empty()) o["claimReason"] = claim.reason;
    } else {
        // "We couldn't ask" is its own answer, and must not read as "nobody owns it" — that reading is how a plug gets taken.
        o["claim"]    = "unknown";
        o["pickable"] = false;
        o["takeable"] = false;
        o["claimReason"] = std::string(!ok ? "didn't answer on either protocol"
                                           : (isTasmota ? "couldn't read its Mem1 marker" : "couldn't read its push config"));
    }
}

inline std::string describeJson(const char* ip, const Self& self) {
    DynamicJsonDocument d(1024);
    describe(d.to<JsonObject>(), ip, nullptr, /*kindKnown=*/false, OUTLET_SHELLY, 0, self);
    std::string out; serializeJson(d, out); return out;
}

// Rename: POST /api/outlets/name. Never writes a plug someone else owns unless a human said to (`takeover`), and a
// read we could not make is "we don't know", which stays a no — confirming a question nobody could pose is not consent.
// Only the NAME is written: the plug keeps reporting to whoever owns it. `landed` is what was written, for the shell
// to mirror into its own slot table.
inline std::string rename(const char* ip, const char* label, bool takeover, const Self& self, std::string* landed = nullptr) {
    DynamicJsonDocument resp(512);
    // BOTH PROTOCOLS: an IP says nothing about which (a Tasmota renamed through a Shelly-only path answered "not responding").
    ShellyGen2Outlet shellyPlug(ip, "rename");
    TasmotaOutlet    tasPlug(ip, "rename");
    bool isTasmota = false;
    bool alive = shellyPlug.poll();
    if (!alive && tasPlug.poll()) { alive = true; isTasmota = true; }
    SmartOutlet* plugP = isTasmota ? (SmartOutlet*)&tasPlug : (SmartOutlet*)&shellyPlug;

    if (!alive) {
        resp["ok"] = false; resp["error"] = "not responding";
        plughttp::log(std::string("[RENAME] ") + ip + " is not answering on either protocol \xE2\x80\x94 name unchanged.");
    } else {
        std::string devName;
        plugclaim::Claim claim;
        bool claimKnown = false;
        if (isTasmota) {
            tasPlug.readName(devName);   // a Tasmota's name is its DeviceName, which matters for a swept plug with no hostname
            claimKnown = tasPlug.readClaim(self.host.c_str(), devName.c_str(), self.name.c_str(), claim);
        } else {
            devName    = fetchShellyDeviceName(ip, 2);
            claimKnown = shellyPlug.readClaim(self.host.c_str(), devName.c_str(), self.name.c_str(), claim);
        }
        if (!claimKnown || !plugclaim::mayRepoint(claim, takeover)) {
            const std::string why = claimKnown ? claim.reason : std::string("could not read who owns this plug");
            resp["ok"] = false; resp["error"] = why;
            plughttp::log(std::string("[RENAME] Refused for ") + ip + " \xE2\x80\x94 " + why);
        } else {
            // The suffix says "this plug is being USED by that brain", so it goes on only when that is true: a plug renamed
            // under an override still belongs to whoever it reports to, and an unclaimed one is not ours yet.
            const bool ours  = (claim.state == plugclaim::State::Ours);
            const std::string full = ours ? plugclaim::formatName(label, self.name) : std::string(label);
            const bool ok = plugP->setName(full.c_str());
            resp["ok"] = ok; resp["name"] = full; resp["label"] = std::string(label);
            if (!ok) resp["error"] = "the plug refused the name";
            else if (landed) *landed = full;
            plughttp::log(std::string("[RENAME] ") + ip + " -> \"" + full + "\" " + (ok ? "ok" : "FAILED"));
        }
    }
    std::string out; serializeJson(resp, out); return out;
}

// Release: POST /api/outlets/release, the device half of unpairing. Best-effort by design — the layout half is a plain
// write the UI does regardless, because a plug you have unplugged is exactly when you want to detach it. A poll-only plug
// was never written to, so there is nothing to give back. `restoreUrl` is the push target a takeover displaced, if any.
inline std::string release(const char* ip, const Self& self, bool pollOnly, const char* restoreUrl) {
    DynamicJsonDocument resp(512);
    if (pollOnly) {
        resp["ok"] = true; resp["released"] = false; resp["note"] = "polled only \xE2\x80\x94 nothing was written to this plug";
        plughttp::log(std::string("[RELEASE] ") + ip + " was poll-only \xE2\x80\x94 nothing to hand back.");
    } else {
        ShellyGen2Outlet shellyRel(ip, "release");
        TasmotaOutlet    tasRel(ip, "release");
        bool isTasmota = false;
        bool alive = shellyRel.poll();
        if (!alive && tasRel.poll()) { alive = true; isTasmota = true; }
        if (!alive) {
            resp["ok"] = false; resp["released"] = false; resp["error"] = "not responding";
            plughttp::log(std::string("[RELEASE] ") + ip + " is not answering on either protocol \xE2\x80\x94 it keeps whatever we wrote.");
        } else if (isTasmota) {
            // PowerLock off BEFORE clearing Mem1, so a part-way failure leaves a plug that is still ours and still operable.
            std::string devName; bool nameOk = true;
            if (tasRel.readName(devName)) {
                std::string lbl, own;
                plugclaim::parseName(devName, lbl, own);
                if (!own.empty() && own == self.name) nameOk = tasRel.setName(lbl.c_str());
            }
            const bool relOk = tasRel.release();
            resp["ok"] = nameOk && relOk; resp["released"] = true; resp["restored"] = false;
            if (!relOk) resp["error"] = "the plug kept our claim"; else if (!nameOk) resp["error"] = "the plug kept our name";
            plughttp::log(std::string("[RELEASE] ") + ip + " (tasmota) name=" + (nameOk ? "ok" : "FAILED") + " claim=" + (relOk ? "cleared" : "FAILED"));
        } else {
            // Name first, then push — the order pairing uses: a Ws write makes the plug reopen its socket and a name write
            // landing on top of that gets lost.
            std::string devName = fetchShellyDeviceName(ip, 2);
            std::string lbl, own;
            plugclaim::parseName(devName, lbl, own);
            bool nameOk = true;
            if (!own.empty() && own == self.name) { nameOk = shellyRel.setName(lbl.c_str()); plughttp::sleepMs(150); }
            const bool pushOk = shellyRel.releasePush(restoreUrl ? restoreUrl : "");
            resp["ok"] = nameOk && pushOk; resp["released"] = true; resp["restored"] = (restoreUrl && *restoreUrl);
            if (!nameOk) resp["error"] = "the plug kept our name"; else if (!pushOk) resp["error"] = "the plug kept pushing to us";
            plughttp::log(std::string("[RELEASE] ") + ip + " name=" + (nameOk ? "ok" : "FAILED") + " push=" + (pushOk ? "ok" : "FAILED"));
        }
    }
    std::string out; serializeJson(resp, out); return out;
}

}  // namespace outletops

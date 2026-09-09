// =============================================================================
// TasmotaOutlet.h — a Tasmota plug, read as a power SENSOR and nothing else.
//
// Compatible devices: the Athom "No Relay Power Monitoring US Plug" (Tasmota
// build), and anything else running Tasmota with an energy monitor. Also — by
// design — our own CT sensor board, which serves the same endpoint rather than
// inventing a second protocol (docs/tool-sensing-rfc.md §6).
//
// API endpoint: GET http://<ip>/cm?cmnd=Status%208
// Power field:  response["StatusSNS"]["ENERGY"]["Power"]  (float, watts)
//
// BOTH VERIFIED against a real Athom plug on 2026-09-09 — Tasmota 14.3.0,
// ESP8285H16. The reply carries more than we read:
//
//   {"StatusSNS":{"ENERGY":{"Power":0,"ApparentPower":0,"ReactivePower":0,
//                           "Factor":0.00,"Voltage":115,"Current":0.000, ...}}}
//
// We take Power alone because that is what thresholdW is in. The rest is not
// waste: `Voltage` is MEASURED (115 V on that plug, not the 120 V nominal the CT
// bench assumes) and `Current` and `Factor` are exactly what a CT cannot give —
// see the note in firmware/wiring/ct-bench.md about a CT measuring current
// rather than power.
//
// WHY THIS EXISTS. A 1HP dust collector trips the overpower protection on a
// Shelly Plus Plug US, and that protection is there to guard the RELAY
// CONTACTS — so the fix is not a bigger plug, it is removing the relay from the
// sense path. A pass-through metering plug has no contacts to arc, weld, or
// trip. See docs/tool-sensing-rfc.md §2.
//
// IT CANNOT SWITCH, AND THAT IS THE POINT. setSwitch() is left at
// SmartOutlet's default (returns false). A tool is only ever SENSED; the
// collector is the one thing DustGate commands, and it is commanded by RF now.
// A caller that needs to switch something must not be handed one of these.
//
// OWNERSHIP, VIA Mem1. ShellyGen2Outlet::readPushConfig() is described there as
// THE authority on who owns a plug, because a Shelly records its push target and
// we can read it back; names are user-editable and prove nothing. Tasmota has no
// Ws.SetConfig — but it does have Mem1..Mem16: free-text variables that persist
// across reboot and are read and written over the same /cm?cmnd= endpoint. We
// write our hostname into Mem1 and read it back, which is the same shape of
// thing: device state we set and can verify, not a name a user might edit.
//
// ⚠️ IT IS ADVISORY, NOT ENFORCED, and that is a real weakening rather than an
// implementation detail. A Shelly can only push to ONE place, so its claim is
// the mechanism; Mem1 is a note we agree to read. Two brains can poll the same
// Tasmota plug and neither will notice, because polling leaves no trace. There
// is also NO Foreign state to be had — Home Assistant polling this plug writes
// nothing, so a plug reading Unclaimed may well be in use and we cannot tell.
// See plugclaim::decideMarker().
//
// The Shelly-shaped hooks stay at SmartOutlet's defaults, deliberately:
// configureOutboundWs() and readPushConfig() are about a push config this device
// does not have, and a stub that pretended otherwise would be worse than none.
// =============================================================================

#pragma once
#include "SmartOutlet.h"
#include "../config.h"

class TasmotaOutlet : public SmartOutlet {
public:
    TasmotaOutlet(const char* ip, const char* name);

    bool        poll()       override;
    bool        probe(uint32_t timeoutMs) override;
    const char* name()       const override { return _name; }
    const char* ip()         const override { return _ip; }

    // NOT a Shelly generation. `generation()` is a misnomer this class inherits
    // — it is only ever stored and reported, never dispatched on (the one caller
    // is SmartOutletControl.cpp writing it into OutletEntry). Dispatch happens on
    // OutletKind instead, because Shelly Gen3 and Gen4 exist and overloading this
    // number would make a real Shelly generation unrepresentable.
    //
    // 0 is deliberate: "no generation", not "generation zero".
    int         generation() const override { return 0; }
    OutletKind  kind()       const override { return OUTLET_TASMOTA; }

    // ── Ownership marker (Mem1) ──────────────────────────────────────────
    //
    // Blocking HTTP, like ShellyGen2Outlet's config calls — discovery and
    // provisioning paths only, never the poll task.

    // Read Mem1. Returns false if the plug did not answer or did not parse,
    // which is NOT "unclaimed": a read failure means we do not know, and the
    // caller must not turn that into permission to take the plug. Same rule
    // readPushConfig() states for Shelly, and for the same reason.
    bool readOwner(String& out, uint32_t timeoutMs = OUTLET_RPC_WRITE_TIMEOUT_MS);

    // Write Mem1. Pass "" to release — Tasmota clears a Mem to empty when given
    // the literal `"` (an empty quoted string), which is what release() sends.
    bool writeOwner(const char* owner);

    // Claim the plug AND make it behave like the pass-through it is meant to be.
    //
    // The Athom no-relay plug still ships firmware for its relay sibling: the web
    // UI has a Toggle button and reports a Power state, and on the no-relay
    // hardware pressing it does nothing at all (confirmed 2026-09-09). Harmless
    // there, and confusing — the page will cheerfully say OFF while the tool it
    // is sensing runs perfectly. On the relay SKU it is worse than confusing.
    //
    // So pairing locks it: PowerOnState 1 (always on at boot), then PowerLock 1
    // (Power commands ignored). ORDER MATTERS — locking first would nail down
    // whatever state it happens to be in, and a plug locked OFF is a tool that
    // silently has no power.
    bool provision(const char* owner);

    // Undo both, and hand the plug back. Unlock BEFORE clearing the claim, so a
    // failure part-way leaves a plug that is still ours rather than one nobody
    // owns and nobody can operate.
    bool release();

private:
    char _ip[16];
    char _name[32];

    bool doPoll(uint32_t timeoutMs = OUTLET_HTTP_TIMEOUT_MS);
    bool reresolve();
};

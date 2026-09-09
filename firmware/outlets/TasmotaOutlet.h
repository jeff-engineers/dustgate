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
// NO OWNERSHIP CLAIM YET. ShellyGen2Outlet::readPushConfig() is described there
// as THE authority on who owns a plug, because a Shelly records its push target
// and we can read it back; names are user-editable and prove nothing. Tasmota
// has no Ws.SetConfig. The defaults inherited from SmartOutlet (configureOutboundWs,
// setName, releasePush, readPushConfig all no-ops returning false) are therefore
// honest rather than lazy: this driver genuinely cannot claim a device, and a
// stub that pretended otherwise would be worse than none. Unresolved in §11, and
// it gates discovery — see the note in TasmotaOutlet.cpp.
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

private:
    char _ip[16];
    char _name[32];

    bool doPoll(uint32_t timeoutMs = OUTLET_HTTP_TIMEOUT_MS);
    bool reresolve();
};

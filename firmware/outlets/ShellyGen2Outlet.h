// =============================================================================
// ShellyGen2Outlet.h — Shelly Gen 2 / Plus / Pro power monitoring outlet
//
// Compatible devices: Shelly Plus Plug S, Plus 1PM, Pro 1PM, Pro 2PM, Pro 4PM
// API endpoint: GET http://<ip>/rpc/Switch.GetStatus?id=0
// Power field:  response["apower"]  (float, watts)
//
// Gen 2 devices use an RPC-based API instead of the flat JSON of Gen 1.
// Authentication (if enabled in the app) is not yet supported — disable
// "Authentication" in the Shelly app for devices used with this system.
// =============================================================================

#pragma once
#include "SmartOutlet.h"
#include "../config.h"

class ShellyGen2Outlet : public SmartOutlet {
public:
    ShellyGen2Outlet(const char* ip, const char* name);

    bool        poll()       override;
    bool        probe(uint32_t timeoutMs) override;
    bool        setSwitch(bool on) override;
    const char* name()       const override { return _name; }
    const char* ip()         const override { return _ip; }
    int         generation() const override { return 2; }

    // Point the plug's Outbound WebSocket at us (Ws.SetConfig), and set its
    // app-visible name (Switch.SetConfig). Blocking HTTP — poll task only.
    bool        configureOutboundWs(const char* wsUrl) override;
    bool        setName(const char* name) override;
    // Hand the plug back on unpair — restore `restoreUrl` if we took it from
    // another controller, otherwise disable Ws so it pushes nowhere.
    bool        releasePush(const char* restoreUrl) override;

    // Ws.GetConfig — read who this plug currently pushes to. THE AUTHORITY on
    // ownership (docs/shop-schema-rfc.md §8): names are user-editable, this is
    // not. Blocking HTTP, so discovery/provisioning paths only.
    //
    // Returns false if the plug didn't answer or the response didn't parse —
    // which is NOT "unclaimed". A read failure means we don't know, and the
    // caller must not turn that into permission to steal.
    bool        readPushConfig(String& outServer, bool& outEnabled,
                               uint32_t timeoutMs = OUTLET_RPC_WRITE_TIMEOUT_MS);

private:
    char _ip[16];
    char _name[32];

    bool doPoll(uint32_t timeoutMs = OUTLET_HTTP_TIMEOUT_MS);
    bool reresolve();
    // POST a JSON-RPC method to the plug's /rpc endpoint. Returns true on HTTP 200.
    bool rpcPost(const char* jsonBody);
};

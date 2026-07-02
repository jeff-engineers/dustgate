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
    const char* name()       const override { return _name; }
    const char* ip()         const override { return _ip; }
    int         generation() const override { return 2; }

private:
    char _ip[16];
    char _name[32];
};

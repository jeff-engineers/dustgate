// =============================================================================
// ShellyGen1Outlet.h — Shelly Gen 1 (original) power monitoring outlet
//
// Compatible devices: Shelly Plug, Plug S, 1PM, 2.5, EM, 3EM
// API endpoint: GET http://<ip>/status
// Power field:  response["meters"][0]["power"]  (float, watts)
//
// Ensure the Shelly is on the same local network as the ESP32 and that
// "Local control" is enabled in the Shelly app (it is by default).
// =============================================================================

#pragma once
#include "SmartOutlet.h"
#include "../config.h"

class ShellyGen1Outlet : public SmartOutlet {
public:
    ShellyGen1Outlet(const char* ip, const char* name);

    bool        poll()       override;
    bool        setSwitch(bool on) override;
    const char* name()       const override { return _name; }
    const char* ip()         const override { return _ip; }
    int         generation() const override { return 1; }

private:
    char _ip[16];    // "xxx.xxx.xxx.xxx\0"
    char _name[32];
};

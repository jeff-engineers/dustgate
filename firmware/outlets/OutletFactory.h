// =============================================================================
// OutletFactory.h — the one place that turns a kind into a driver.
//
// WHY THIS EXISTS NOW AND NOT BEFORE. When Gen1 was dropped there was exactly
// one driver left, so all four construction sites in SmartOutletControl.cpp
// hardcoded `new ShellyGen2Outlet(...)` and the dispatch collapsed to nothing.
// Adding Tasmota puts a choice back, and four hardcoded sites is four places to
// forget. A third protocol should be one edit here plus a driver, and nothing
// else.
//
// KIND, NOT GENERATION. A Shelly generation is a number inside Shelly's world;
// Tasmota has none, and Shelly Gen3/Gen4 exist. Overloading `generation` to mean
// "not a Shelly" would make a real generation unrepresentable the first time one
// is supported. See the note on OutletKind in SmartOutlet.h.
// =============================================================================

#pragma once
#include "SmartOutlet.h"
#include "ShellyGen2Outlet.h"
#include "TasmotaOutlet.h"

// Never returns null: an unknown kind falls back to Shelly, because that is what
// every device predating the field actually is. A corrupt NVS int should give a
// plug that probably works, not a null the callers do not check for.
inline SmartOutlet* makeOutlet(OutletKind kind, const char* ip, const char* name) {
    switch (kind) {
        case OUTLET_TASMOTA: return new TasmotaOutlet(ip, name);
        case OUTLET_SHELLY:
        default:             return new ShellyGen2Outlet(ip, name);
    }
}

// Human-readable, for logs and the API. Kept beside the factory so a new kind
// cannot be added in one place and forgotten in the other.
inline const char* outletKindName(OutletKind kind) {
    switch (kind) {
        case OUTLET_TASMOTA: return "tasmota";
        case OUTLET_SHELLY:
        default:             return "shelly";
    }
}

// Parse the wire spelling. Unknown strings are SHELLY, matching an absent field
// — a document from a newer UI naming a kind this firmware has never heard of
// gets the old behaviour rather than a plug that silently does nothing.
inline OutletKind outletKindFromName(const char* s) {
    if (s && strcmp(s, "tasmota") == 0) return OUTLET_TASMOTA;
    return OUTLET_SHELLY;
}

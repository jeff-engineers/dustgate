// =============================================================================
// control/SenseReport.h — one board's reported clamps, as JSON for GET /api/nodes.
//
// Shared by the remote-node loop and the `self` block in firmware.ino so the
// primary's OWN clamp renders through exactly the same fields. A clamp at the
// collector is the likeliest first one in any shop, and it must not be the one
// the screen cannot show.
//
// IN A HEADER RATHER THAN THE SKETCH because the Arduino preprocessor hoists a
// prototype for every function it finds in a .ino, and it hoists this one above
// the template parameter it depends on: "error: 'BusT' does not name a type".
// Headers are left alone, which is the whole reason this file exists.
//
// `reported` IS THE LOAD-BEARING FIELD, AND IT IS NOT `on`. A clamp the layout
// names, on a board that is online, that has never sent a SENSE, means the chain
// is broken somewhere between CONFIG and the ADC. "Off" looks identical on a
// screen and means the tool is simply idle. Telling those two apart is the
// entire reason this is on a screen at all — it is the question you ask standing
// at a bench with a node in one hand.
// =============================================================================
#pragma once
#include <ArduinoJson.h>

namespace topo {

template <typename BusT>
inline void addSenseArray(JsonObject o, const BusT& bus) {
    const size_t n = bus.senseCount();
    if (!n) return;                       // absent means "no clamp configured here"
    JsonArray arr = o.createNestedArray("sense");
    for (size_t i = 0; i < n; i++) {
        String id; bool reported = false, on = false;
        uint32_t ageMs = 0; float level = -1.0f;
        if (!bus.senseAt(i, id, reported, on, ageMs, level)) continue;
        JsonObject e = arr.createNestedObject();
        e["id"]       = id;
        e["reported"] = reported;
        // Only meaningful once something has actually arrived. Emitting on:false
        // for a clamp that has never spoken is precisely the lie above.
        if (reported) {
            e["on"]    = on;
            e["ageMs"] = ageMs;
            if (level >= 0.0f) e["level"] = level;
        }
    }
}

} // namespace topo

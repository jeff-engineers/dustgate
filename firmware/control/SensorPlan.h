// =============================================================================
// control/SensorPlan.h — what a layout wants WATCHED, resolved once.
//
// TopologyRuntime used to resolve the same sensors twice: pushSensorConfig() walked the
// systems and elements to build each board's CONFIG, and pollSensors() walked them again to
// find the readings by the ids it had sent. Every new kind of sensor (the clamp, then the
// plug, then the dust bin) had to be added to BOTH, spelled the same way ("bin:" + system),
// and kept in step by hand — the classic way a sensor ends up working on the push side and
// silent on the poll side, with nothing anywhere reporting a fault.
//
// This resolves the layout into one list. The push serialises it per board; the poll looks
// readings up by `id`. A third consumer (the native brain's status, a UI) reads the same list.
//
// WHAT IT KNOWS: how a layout names a sensor — through the MACHINE for a tool, through the
// element for a collector (see clampOf() in Shop.h), one entry per machine however many ports
// it has. WHAT IT DOES NOT KNOW: which boards can watch what (WELCOME caps), how many sensors
// a board accepts, or anything about the wire. Those are the push's job, because they are
// facts about a LINK that the layout cannot see.
//
// PURE — STL + ArduinoJson, no Arduino.h, so the host tests drive it.
// =============================================================================
#pragma once
#include <functional>
#include <set>
#include <string>
#include <vector>
#include "Shop.h"

namespace topo {

struct PlannedSensor {
    enum class Kind { Clamp, Plug, Bin };
    Kind        kind = Kind::Clamp;
    // The id on the wire, and the key a reading is read back by. A machine's id for a tool's
    // clamp or plug, a collector element's id for its clamp, "bin:<system>" for a dust bin.
    std::string id;
    // The controllerId of the board that watches it. "" = THIS board (whatever its own id is
    // spelled — see BoardId.h; callers compare with sameBoard/isOwnBoard, never ==).
    std::string board;
    std::string systemId;            // the system the element belongs to (collector clamp, bin)
    bool        onCollector = false; // a clamp on a COLLECTOR: it judges the blower, not a machine
    int         channel     = 0;     // clamp: which input on that board
    std::string ip;                  // plug
    bool        tasmota     = false; // plug: Tasmota rather than Shelly Gen2
    float       thresholdW  = 0.0f;  // plug: watts at or above which the tool is ON
    bool        invert      = true;  // bin: the pin reads LOW when full (the optocoupler's sense)
};

constexpr const char* kBinSensorPrefix = "bin:";

/**
 * Resolve every sensor the layout names. Order: clamps, then plugs, then bins — the order each
 * board's CONFIG is written in. One entry per MACHINE for a clamp or a plug (a saw with an
 * overarm has two ports and ONE sensor, and the node refuses a CONFIG with duplicates whole);
 * one per collector for a bin.
 *
 * A plug appears only for a tool that has no clamp — a machine with a clamp is sensed by the
 * clamp, exactly as it always has been — and only if the layout gives it an address. Its
 * `board` is who SHOULD poll it (plugOwnerOf, the matched pair with plugOwners() in shop.js);
 * "" means the brain, which polls its own plugs through SmartOutletControl and is never sent one.
 *
 * `thresholdOf` supplies a machine's on-threshold (the controller owns that default).
 */
inline std::vector<PlannedSensor> planSensors(JsonObjectConst topology,
                                              const std::function<float(const std::string&)>& thresholdOf) {
    std::vector<PlannedSensor> out;
    std::set<std::string> clamped;     // machine / element ids that already have a clamp entry

    // ── clamps ─────────────────────────────────────────────────────────────
    for (const SystemView& sys : systemsOf(topology)) {
        for (JsonObjectConst e : sys.elements) {
            // THROUGH THE MACHINE — see clampOf() in Shop.h.
            JsonObjectConst ct = clampOf(topology, e);
            if (ct.isNull()) continue;
            const std::string sid = sensedIdOf(e);
            if (sid.empty() || !clamped.insert(sid).second) continue;
            PlannedSensor p;
            p.kind        = PlannedSensor::Kind::Clamp;
            p.id          = sid;
            p.onCollector = _eq(e["type"], "collector");
            // A collector's clamp is on the collector's board; a tool's names its own.
            p.board       = p.onCollector ? collectorBoardOf(e) : std::string(ct["controllerId"] | "");
            p.systemId    = sys.id ? sys.id : "";
            p.channel     = ct["channel"] | 0;
            out.push_back(p);
        }
    }

    // ── plugs ──────────────────────────────────────────────────────────────
    std::set<std::string> plugged;
    for (const SystemView& sys : systemsOf(topology)) {
        for (JsonObjectConst e : sys.elements) {
            if (!_eq(e["type"], "tool")) continue;
            const std::string mid = machineIdOf(e);
            if (mid.empty() || plugged.count(mid) || clamped.count(mid)) continue;
            JsonObjectConst outlet = machineDoc(topology, mid)["sensor"]["outlet"];
            const char* ip = outlet["ip"].as<const char*>();
            if (!ip || !*ip) continue;
            plugged.insert(mid);
            PlannedSensor p;
            p.kind       = PlannedSensor::Kind::Plug;
            p.id         = mid;
            p.board      = plugOwnerOf(topology, mid);
            p.systemId   = sys.id ? sys.id : "";
            p.ip         = ip;
            p.tasmota    = _eq(outlet["kind"], "tasmota");
            p.thresholdW = thresholdOf ? thresholdOf(mid) : 0.0f;
            out.push_back(p);
        }
    }

    // ── dust bins ──────────────────────────────────────────────────────────
    for (const SystemView& sys : systemsOf(topology)) {
        const std::string sysId = sys.id ? sys.id : "";
        for (JsonObjectConst e : sys.elements) {
            if (!_eq(e["type"], "collector")) continue;
            JsonObjectConst bs = e["bin"]["sensor"];
            if (bs.isNull()) break;
            PlannedSensor p;
            p.kind     = PlannedSensor::Kind::Bin;
            p.id       = std::string(kBinSensorPrefix) + sysId;
            p.board    = collectorBoardOf(e);   // the collector's board (one per collector, 2026-10-10)
            p.systemId = sysId;
            p.invert   = bs["invert"] | true;
            out.push_back(p);
            break;                       // one collector per system
        }
    }
    return out;
}

}  // namespace topo

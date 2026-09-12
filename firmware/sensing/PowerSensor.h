// =============================================================================
// PowerSensor.h — "is this machine running?", and nothing else.
//
// The seam between the routing brain and however a machine's power happens to
// be observed. Today there is exactly one implementation, SmartOutlet, and that
// is deliberate: a seam with no second implementation is a guess, so this one is
// kept as small as the single caller actually needs (2026-09-10).
//
// THE VERDICT LIVES HERE, NOT IN THE BRAIN, and that is the whole design
// decision. The obvious interface carries watts and lets TopologyRuntime compare
// them to a threshold. That is the same trap `DEFAULT_THRESHOLD_W` fell into: a
// metering plug genuinely knows watts, and a CT clamp knows CURRENT and cannot
// convert without voltage and power factor (see the note at the top of
// TasmotaOutlet.h — the Athom plug reports all three, which is exactly what a
// bare CT cannot). An interface with `watts()` on it forces a current sensor to
// either lie or return a sentinel, and a sentinel that means "I can't say" is
// one refactor away from being read as zero.
//
// So each sensor answers the one question every caller actually asks —
// `isActive()` — using whatever it can measure and whatever threshold shape
// suits it. A plug compares watts to a user-set `thresholdW`. A CT would
// compare against a baseline learned at pairing time, because a ratio against a
// tool's own quiet state does not care what the absolute numbers mean, or where
// the board's noise floor sits (docs/tool-sensing-rfc.md §5.4a).
//
// THIS DOES NOT WEAKEN "a sensor reports, it never judges." What moves into the
// sensor is THRESHOLD judgement, which is per-sensor by nature — it is a
// question about units and noise, not about the shop. Every decision that is
// actually about the shop stays in the brain: which tool wins a contested
// selector, when a gate opens, whether the collector runs. TopologyRuntime is
// still the only thing that routes.
//
// WATTS RIDE ALONG, OPTIONALLY. The UI shows a live wattage per tool and that is
// worth keeping, so `hasWatts()` says whether the number means anything and
// `getPowerW()` returns it. A sensor that cannot measure power says so, rather
// than reporting a plausible-looking figure it inferred. Ask before reading.
//
// NOT IN THIS INTERFACE, on purpose: addresses, claims, provisioning, push
// config, switching. All of those are properties of a networked PLUG, not of
// power sensing, and pulling them up here would make an onboard CT — which has
// no address at all — implement five methods that can only return false.
// =============================================================================

#pragma once

class PowerSensor {
public:
    virtual ~PowerSensor() {}

    // Take a fresh reading. Blocking for a networked sensor — poll task only,
    // never loop(). Returns true if the reading succeeded.
    virtual bool poll() = 0;

    // Did the last reading succeed? Distinct from isActive(): a sensor we
    // cannot reach is NOT a machine that is off, and collapsing the two is how
    // a dead sensor becomes an idle tool that never opens its gate.
    virtual bool isReachable() const = 0;

    // THE ONE BIT. Is this machine running, as opposed to idling or off?
    //
    // Must be false when unreachable — an unknown answer is not a "yes", and a
    // gate opening on a guess is worse than one that stays put.
    virtual bool isActive() const = 0;

    // Whether getPowerW() means anything. A metering plug: yes. A bare CT:
    // no — it measures current, and watts need voltage and power factor.
    virtual bool hasWatts() const { return true; }

    // Last reading in watts. Only meaningful when hasWatts() is true; callers
    // that display it must check first rather than printing whatever comes back.
    virtual float getPowerW() const { return 0.0f; }
};

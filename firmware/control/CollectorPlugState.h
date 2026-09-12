// =============================================================================
// CollectorPlugState.h — what the blower is ACTUALLY doing, vs what we asked.
//
// C++ mirror of collectorPlugState() in shared/device-model/topology-device.js.
// A MATCHED PAIR: change one, change the other, and keep the two constants
// below equal to theirs. CLAUDE.md's pair table has the row.
//
// WHY IT BECAME A PAIR (2026-09-10). topology-device.js carried a note saying
// this was deliberately NOT mirrored — the firmware reported the plug facts and
// the judgement lived once, in JS — and that "if the OLED ever needs to say
// 'not starting' too, THAT is the moment this becomes a matched pair". This is
// that moment, and for a stronger reason than the OLED.
//
// EVERY WAY WE COMMAND THE COLLECTOR IS STATELESS. A servo pressing a fob
// button, or an HT12E frame, sends an edge and learns nothing (see
// docs/tool-sensing-rfc.md §4.2a/§4.2b). The firmware cannot know from what it
// SENT whether the blower is running; only the draw says. So the device has to
// hold the verdict itself — a browser that may not be open cannot be the only
// thing that notices a blower failed to start.
//
// PURE. No Arduino.h, no I/O, no clock — `onForMs` arrives as an AGE, computed
// by the caller, exactly as it crosses into TopologyRuntime. Host-testable, and
// test_collector_plug.cpp asserts the same cases as collector-plug.test.js in
// the same order.
// =============================================================================

#pragma once
#include <cstdint>

namespace topo {

// Watts above which a blower counts as actually running.
// COLLECTOR_RUNNING_W in topology-device.js — keep equal.
static const float kCollectorRunningW = 50.0f;

// How long a blower gets to reach running draw before we call it a failure.
// An induction motor takes a second or three to spin up, and a lightly-loaded
// one draws very little before it does; judging it the instant we commanded it
// would raise a false alarm on every single start. A debounce on the ALARM, not
// a state anyone is shown.
// COLLECTOR_SPINUP_GRACE_MS in topology-device.js — keep equal.
static const uint32_t kCollectorSpinupGraceMs = 4000;

enum class PlugState : uint8_t {
    NoPlug,       // nothing configured — nothing to know
    Unknown,      // the plug isn't answering; it may well be running
    Off,          // we aren't asking it to run
    Starting,     // commanded on, inside the spin-up grace — no claim yet
    Running,      // commanded on and drawing running current
    NotStarting,  // commanded on, past the grace, drawing nothing
};

inline const char* plugStateName(PlugState s) {
    switch (s) {
        case PlugState::NoPlug:      return "noplug";
        case PlugState::Unknown:     return "unknown";
        case PlugState::Off:         return "off";
        case PlugState::Starting:    return "starting";
        case PlugState::Running:     return "running";
        case PlugState::NotStarting: return "notStarting";
    }
    return "unknown";
}

/**
 * @param known        has anything reported a reading for this system at all?
 *                     (`plugKnown` — false means no plug is configured)
 * @param reachable    did the last read succeed?
 * @param watts        last reading
 * @param onForMs      how long we have been COMMANDING it on. 0 = we don't know,
 *                     which must never become an accusation — see below.
 * @param haveOnFor    whether onForMs means anything. The JS side distinguishes
 *                     an absent onForMs from a zero one; a bare uint32_t cannot,
 *                     and collapsing them would turn a device that never
 *                     reported the age into one that always reads "just
 *                     started". Explicit flag, so the difference survives.
 * @param commandedOn  what we asked for
 */
inline PlugState collectorPlugState(bool known, bool reachable, float watts,
                                    uint32_t onForMs, bool haveOnFor,
                                    bool commandedOn) {
    if (!known)       return commandedOn ? PlugState::Unknown : PlugState::NoPlug;
    if (!reachable)   return PlugState::Unknown;
    if (!commandedOn) return PlugState::Off;
    if (watts >= kCollectorRunningW) return PlugState::Running;
    // Still inside the grace: no claim either way. An unknown age means we
    // cannot say how long it has been on, and guessing "long enough" would turn
    // a just-started blower into an alarm.
    if (!haveOnFor || onForMs < kCollectorSpinupGraceMs) return PlugState::Starting;
    return PlugState::NotStarting;
}

}  // namespace topo

// =============================================================================
// utils/MdnsLock.h — one mDNS query at a time, shop-wide.
//
// ESP-IDF's mDNS has ONE querier. Two searches running at once do not queue —
// the second comes back empty, which every caller here reads as "nothing
// answered" and acts on.
//
// WHY THIS EXISTS (2026-09-17). Three independent things on this board query
// mDNS, on three different tasks:
//
//   - each RemoteActuatorBus, from its OWN FreeRTOS task, re-resolving its
//     node's name while the link is down (every 3s for the first minute)
//   - the node / Shelly / plain-HTTP scans, from the main loop
//   - ShellyGen2Outlet::reresolve(), from the outlet poller on core 0
//
// With one node that is rare enough to look like flaky WiFi. With three boards
// it is constant, and it produced the exact symptom that found this: a shop
// that pairs perfectly at BOOT and loses its nodes afterwards, recovering only
// on a reboot.
//
// The asymmetry is the tell, and it is not a coincidence. At boot,
// syncPairedNodes() called begin() for each node in a loop and begin() resolved
// SYNCHRONOUSLY before it created the task — so the boot-time resolves were
// serialised by construction and all succeeded. (Since 2026-09-27 begin() no
// longer resolves at all — the first resolve runs on the link task, so a Pair
// tap does not block the main loop for a query per node — which makes this
// lock the ONLY thing serialising them, at boot as well.) Every resolve after that happens
// on N parallel tasks on the same cadence, in lockstep, and they collide.
//
// So: take this lock around every mDNS search. It restores at runtime the
// serialisation boot already had by accident.
//
// The failure is silent on BOTH sides — an empty result and a timed-out lock
// look the same to a caller — so a timeout is logged rather than swallowed.
// =============================================================================
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace mdnslock {

// Long enough for a full query to finish and release (the longest is
// DISCOVER_MDNS_TIMEOUT_MS per attempt, and a scan runs several), short enough
// that a wedged holder cannot stall a node's task forever. A link that misses
// one resolve retries on its own cadence; one that blocks forever does not.
static const uint32_t kWaitMs = 8000;

inline SemaphoreHandle_t handle() {
    static SemaphoreHandle_t h = xSemaphoreCreateMutex();
    return h;
}

/** RAII. `held()` is false when the wait timed out — the caller should skip its
 *  query rather than run it unguarded, because running it is what corrupts the
 *  other search that is evidently still in flight. */
class Guard {
public:
    // `waitMs` is shorter for a NodeLink task than for the main loop: a link task
    // that queues 8 s for the lock is 8 s that RemoteActuatorBus::end() has to
    // wait out before it may re-dial, and a link that misses one resolve simply
    // tries again on its own cadence.
    explicit Guard(const char* who, uint32_t waitMs = kWaitMs) : _who(who) {
        SemaphoreHandle_t h = handle();
        _held = h && xSemaphoreTake(h, pdMS_TO_TICKS(waitMs)) == pdTRUE;
        if (!_held) {
            Serial.print(F("[mDNS] busy — skipped a query for "));
            Serial.println(_who ? _who : "?");
        }
    }
    ~Guard() { if (_held) xSemaphoreGive(handle()); }
    bool held() const { return _held; }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
private:
    const char* _who;
    bool        _held = false;
};

} // namespace mdnslock

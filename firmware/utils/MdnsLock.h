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
// syncPairedNodes() calls begin() for each node in a loop and begin() resolves
// SYNCHRONOUSLY before it creates the task — so the boot-time resolves are
// serialised by construction and all succeed. Every resolve after that happens
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
    explicit Guard(const char* who) : _who(who) {
        SemaphoreHandle_t h = handle();
        _held = h && xSemaphoreTake(h, pdMS_TO_TICKS(kWaitMs)) == pdTRUE;
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

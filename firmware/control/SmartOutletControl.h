// =============================================================================
// SmartOutletControl.h — Automatic gate selection via smart outlet polling
//
// Polls all configured Shelly outlets every OUTLET_POLL_INTERVAL_MS on a
// dedicated FreeRTOS task (Core 0). When a tool draws above its threshold
// wattage, the corresponding blast gate is opened automatically.
//
// Priority rule: if multiple tools are on simultaneously, the one drawing
// the most power wins (most likely the actively running tool rather than
// one coasting or idling).
//
// Configuration is stored in NVS (Preferences) and written during setup.
// See outlets/OutletConfig.h.
// =============================================================================

#pragma once
#include "ControlInput.h"
#include "../config.h"
#include "../outlets/SmartOutlet.h"

#ifdef CONTROL_SMART_OUTLET

// Verbose push tracing, off by default, toggled by the `plugtrace` serial
// command. On, every frame a plug sends is timestamped and printed — which is
// the only way to see the plug's REPORTING CADENCE, as opposed to the moments it
// happens to cross a threshold. Off, a crossing still prints one line.
//
// A toggle rather than a build flag because the question it answers ("why did
// the collector take 8 seconds to notice?") only comes up with a real tool, a
// real plug and a person watching, and reflashing to ask it is a poor trade.
namespace outlettrace {
inline bool& enabled() { static bool e = false; return e; }
}

class SmartOutletControl : public ControlInput {
public:
    SmartOutletControl();
    ~SmartOutletControl();

    bool begin()  override;
    void update() override; // no-op — polling is on its own task

    // ControlInput interface
    int  readRequestedStop() override;
    bool isEnabled()         override { return true; } // always on in outlet mode

    // -------------------------------------------------------------------------
    // Setup API
    // Configure a single outlet slot at runtime. Persists to NVS via saveSlot().
    // Call after begin() — the poll task will pick up the new outlet immediately.
    // -------------------------------------------------------------------------
    // Ask the poll task to run a provisioning pass on its next tick. Used by the
    // takeover path so an approved takeover lands now rather than whenever the
    // next reconfigure happens to come round.
    void requestProvision() { _needsProvision = true; }

    // Our identity, for plug ownership (RFC §8). ourHost() is what a plug's
    // configured push target is compared against; ourName() is what we write
    // into its friendly name for humans reading the Shelly app.
    const char* ourHost() const { return _ourHost; }
    const char* ourName() const { return _ourName; }

    void configureOutlet(int slot,
                         int generation,      // 1 = Gen 1, 2 = Gen 2/Plus
                         const char* ip,
                         const char* name,
                         int stopIndex,
                         float thresholdW,
                         const char* host = ""); // mDNS hostname, if known (see SmartOutlet::setHost)

    void removeOutlet(int slot);
    void saveSlot(int slot);       // persist a single slot to NVS
    void saveAll();                // persist all slots to NVS
    void printConfig();            // dump current config to Serial

    // Erases every outlet slot AND the dust collector (RAM + NVS). Used by
    // the setup wizard's "Start Over" — a plain gate/calibration reset alone
    // left old tool-to-gate outlet mappings from the previous run in place.
    void clearAllOutlets();

    int          outletCount() const { return _count; }
    SmartOutlet* outlet(int i)       { return (i >= 0 && i < _count) ? _outlets[i] : nullptr; }
    // Find a configured plug by address. Public because the takeover path in the
    // sketch has an IP from the UI and nothing else to look it up by.
    SmartOutlet* outletByIp(const char* ip);

    // -------------------------------------------------------------------------
    // TAKEOVER approval, held BY ADDRESS (RFC §8).
    //
    // The approval used to live on the SmartOutlet object, which broke it twice
    // over. A plug not yet paired with anything has no outlet object, so there
    // was nowhere to put the answer and the UI had to send the user off to pair
    // it first and come back. Worse, configureOutlet() builds a BRAND NEW outlet
    // on every topology adopt — and adopt is what pairing does — so an approval
    // recorded against the old object was thrown away by the very save that made
    // the plug takeable in the first place.
    //
    // Holding it against the IP fixes both: approve whenever, pair whenever, and
    // the approval is applied to whatever outlet turns up at that address.
    //
    // What does NOT change is the thing that matters: this is still settable only
    // from POST /api/outlets/takeover, still one-shot, and still just an
    // approval — the actual repoint happens on the ordinary provisioning pass,
    // through the same code as any other plug. There remains exactly one way to
    // repoint a plug, which is what keeps "never steal" true everywhere else.
    void approveTakeoverByIp(const char* ip);
    // Consumed by configureOutlet(), and cleared once the repoint has landed.
    bool takeoverApprovedFor(const char* ip) const;
    void clearTakeoverApproval(const char* ip);

    // -------------------------------------------------------------------------
    // Collector plugs — switchable Shelly outlets (we turn them on/off) rather
    // than power sensors. Their blocking HTTP switch calls stay off the motor
    // loop by living on the poll task.
    //
    // ONE PER AIRFLOW SYSTEM (COLLECTOR_COUNT). The two slot kinds differ in who
    // decides, not in how they switch:
    //
    //   slot 0    persisted in NVS, and the only one the pre-topology stop-index
    //             automation drives (on whenever a tool is active, off at idle).
    //             That path predates systems and has exactly one blower, so
    //             giving it any other slot would be inventing a policy for it.
    //   slots 1+  RAM-only, rebuilt from the layout on every adopt (same posture
    //             as the tool slots — persisting them would just be a second copy
    //             to keep in sync). Driven ONLY by an explicit setCollectorManual()
    //             from the routing runtime, which is the only thing that knows a
    //             second system exists.
    //
    // With a topology loaded, routing owns every slot including 0 — see the
    // collector assert in the sketch's loop.
    // -------------------------------------------------------------------------
    void configureCollector(int idx, int generation, const char* ip, const char* host = "");
    void removeCollector(int idx);
    bool collectorConfigured(int idx) const {
        return idx >= 0 && idx < COLLECTOR_COUNT && _collectors[idx] != nullptr;
    }
    // Is this slot already this plug? Lets a caller that re-asserts config (the
    // topology sync runs on every layout save) skip the swap — reconfiguring
    // clears _dcSynced, which re-commands the blower for no reason.
    bool collectorIs(int idx, const char* ip) const {
        return collectorConfigured(idx) && ip && strcmp(_collectors[idx]->ip(), ip) == 0;
    }
    bool collectorOn(int idx);       // thread-safe read for status JSON

    // Force one collector on/off. On slot 0 this holds until the next automatic
    // gate change (tool on/off), then the legacy automation resumes; on the other
    // slots there is no automation to resume to, so it simply holds.
    void setCollectorManual(int idx, bool on);

    // ---- slot-0 spellings, for the pre-systems call sites ----
    void configureDustCollector(int generation, const char* ip, const char* host = "") {
        configureCollector(0, generation, ip, host);
    }
    void removeDustCollector()        { removeCollector(0); }
    bool dcConfigured() const         { return collectorConfigured(0); }
    bool dcIs(const char* ip) const   { return collectorIs(0, ip); }
    bool dcOn()                       { return collectorOn(0); }
    void setDcManual(bool on)         { setCollectorManual(0, on); }

    // -------------------------------------------------------------------------
    // Manual override — bypasses outlet-driven gate selection until the next
    // time any outlet crosses its threshold (tool turned on).
    // Call from the main loop when the HTTP API receives a manual move command.
    // -------------------------------------------------------------------------
    void setManualOverride(int stop);
    bool isManualOverride();        // thread-safe read for status JSON

    // -------------------------------------------------------------------------
    // Push handlers — called from the HTTP server's Shelly WebSocket callback
    // (AsyncTCP task) when a Gen2 plug's Outbound WebSocket connects, pushes a
    // power reading, or disconnects. `ip` is the plug's dotted-quad source IP.
    // These update the matching outlet and wake the poll task so tool on/off is
    // reacted to immediately instead of at the next poll tick.
    // -------------------------------------------------------------------------
    void onPushConnect(const char* ip);
    void onPushedPower(const char* ip, float apower);
    void onPushDisconnect(const char* ip);

private:
    SmartOutlet*      _outlets[SMART_OUTLET_COUNT];
    int               _count;

    // Collector plugs (switchable), one per airflow system. nullptr = not
    // configured. Index 0 is the NVS-persisted one — see the header note above.
    SmartOutlet*      _collectors[COLLECTOR_COUNT];
    bool              _dcOn[COLLECTOR_COUNT];              // last commanded state (protected by _mutex)
    bool              _dcSynced[COLLECTOR_COUNT];          // false = force a switch command on next reconcile
    bool              _dcManualOverride[COLLECTOR_COUNT];  // true = follow _dcManualState, not gate state
    bool              _dcManualState[COLLECTOR_COUNT];     // forced on/off while override active

    // Shared state between poll task and main loop — protected by _mutex
    int               _requestedStop;
    // Which tool is currently active (its stop), or 0 = none. Distinct from
    // _requestedStop: at idle we HOLD the gate at its last position (don't return
    // home — keeps a path open so a manual collector start can't dead-head, and
    // avoids wear on a brief tool-off), but the dust collector still follows
    // _activeStop (off when no tool runs).
    int               _activeStop;
    bool              _manualOverride;   // true = ignore outlet selection until next tool-on
    SemaphoreHandle_t _mutex;

    // ── Retired plug objects, awaiting reclamation ───────────────────────────
    //
    // WHY THIS EXISTS: the poll task holds a raw SmartOutlet* across blocking
    // HTTP (a provisioning probe is seconds), while the main loop and the async
    // web server can reconfigure plugs at any moment. `delete` on those paths
    // freed an object the poll task was still using — a use-after-free that
    // crashed the board every boot, right after topology adoption dropped the
    // slots a new layout no longer wanted:
    //
    //   Guru Meditation Error: Core 0 panic'ed (LoadProhibited)
    //   Backtrace: SmartOutletControl::doPoll() → pollTaskFn()
    //
    // A mutex alone cannot fix it. Holding _mutex across the poll loop would
    // block every API request behind seconds of plug HTTP, and taking it only
    // around the delete leaves the poll task dereferencing a pointer it read
    // before the lock was taken.
    //
    // So configuration NEVER frees. It swaps the pointer out of the array under
    // _mutex — after which no task can newly reach it — and hands the old object
    // here. The POLL TASK frees, at the top of doPoll(), where it holds no plug
    // pointer of its own, and only once two poll cycles have passed: one to
    // outlive whatever pass was in flight when the object was retired, one for
    // the brief dereferences the main loop and API make inside a single call.
    //
    // That grace is a bound, not a proof. It is generous (a cycle is
    // OUTLET_POLL_INTERVAL_MS plus however long the plug HTTP took) and the
    // alternative — proving quiescence across three tasks — is a much larger
    // change to a board that runs a real shop.
    static const int RETIRE_SLOTS = SMART_OUTLET_COUNT + COLLECTOR_COUNT;
    SmartOutlet*      _retired[RETIRE_SLOTS];
    uint32_t          _retiredCycle[RETIRE_SLOTS];
    int               _retiredCount;
    uint32_t          _pollCycle;
    // Its own lock: retire() is called both with and without _mutex held, and
    // _mutex is a plain binary semaphore that would deadlock on re-entry.
    SemaphoreHandle_t _retireMutex;

    /** Hand an outlet object over to be freed later. Null is ignored. */
    void retire(SmartOutlet* o);
    /** Poll task only, at the top of doPoll(): free what has aged out. */
    void reapRetired();

    // Poll task handle — lets setDcManual() wake the task immediately (via
    // xTaskNotifyGive) so a manual dust-collector toggle switches at once
    // instead of waiting up to OUTLET_POLL_INTERVAL_MS for the next tick.
    // Automatic on/off is unaffected: it still follows the debounced gate
    // selection, so tool-driven switching keeps its coast-down delay.
    TaskHandle_t      _pollTaskHandle;

    // Push provisioning: when true, the poll task (re)configures every Gen2
    // plug's Outbound WebSocket + name on its next run. Set at begin() and
    // whenever an outlet is (re)configured. The WS URL points plugs back at
    // this device's current IP, so it's rebuilt on every boot — self-healing
    // across DHCP address changes without needing a static IP.
    volatile bool     _needsProvision;
    bool              _provisionPending;   // true = some plug still needs provisioning; retry periodically
    unsigned long     _lastProvisionMs;    // last provisioning attempt (for retry backoff)
    char              _wsUrl[64];   // ws://<our-ip>:80/shelly-rpc
    // Our own IP the last time we built _wsUrl. If DHCP moves us to a new address
    // at runtime, the URL plugs dial back to goes stale — checkLocalIpChange()
    // detects the change, rebuilds the URL, and forces re-provisioning so push
    // recovers instead of silently degrading to polling until reboot.
    uint32_t          _lastLocalIp;

    // OUR IDENTITY, for plug ownership (RFC §8). Two different strings on
    // purpose, because they answer two different questions:
    //
    //   _ourHost  our IP, as it appears in _wsUrl. What a plug's configured push
    //             target is COMPARED against — the machine-readable authority.
    //   _ourName  our mDNS hostname. What gets written into a plug's friendly
    //             name, so the Shelly app shows "Table Saw · dustgate-shop" and a
    //             human can see which brain spoke for it. A hint, never the
    //             decision: names are user-editable.
    //
    // Using the IP for the name would be worse than useless — it changes with a
    // DHCP lease, and the label would then claim the plug for an address nobody
    // is at any more.
    char              _ourHost[40];
    char              _ourName[40];

    // Addresses the user has approved a takeover for, whether or not a plug is
    // configured there yet. Small and fixed: approvals are rare, deliberate, and
    // consumed almost immediately. A full table simply refuses the newest rather
    // than evicting an older approval the user may still be acting on.
    static const int  kMaxPendingTakeovers = 4;
    char              _pendingTakeoverIp[kMaxPendingTakeovers][40];

    // Debounce tracking (poll task only — no mutex needed)
    int               _pendingStop;
    unsigned long     _pendingStartMs;
    // Per-outlet active state from the previous poll tick — lets doPoll()
    // detect a fresh OFF→ON transition (edge) instead of just "currently on"
    // (level), so manual override isn't immediately re-clobbered by a tool
    // that was already running before the manual move (poll task only).
    bool              _prevActive[SMART_OUTLET_COUNT];

    static void pollTaskFn(void* param);
    void        doPoll();
    void        reconcileCollectors();     // poll task: drive every collector plug to its desired state
    bool        provisionPushOutlets();    // poll task: push Ws/name config; returns true if any still pending
    void        checkLocalIpChange();      // poll task: rebuild _wsUrl + re-provision if our DHCP IP changed
};

#endif // CONTROL_SMART_OUTLET

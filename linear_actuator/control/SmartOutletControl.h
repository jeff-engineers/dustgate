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
// Configuration is stored in NVS (Preferences) and written by the setup
// agent. See outlets/OutletConfig.h.
// =============================================================================

#pragma once
#include "ControlInput.h"
#include "../config.h"
#include "../outlets/SmartOutlet.h"

#ifdef CONTROL_SMART_OUTLET

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
    // Setup agent API
    // Configure a single outlet slot at runtime. Persists to NVS via saveSlot().
    // Call after begin() — the poll task will pick up the new outlet immediately.
    // -------------------------------------------------------------------------
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

    // -------------------------------------------------------------------------
    // Dust collector plug — a switchable Shelly outlet (we turn it on/off)
    // rather than a power sensor. The poll task drives it on whenever a tool is
    // active (a real gate is selected) and off when idle, so its blocking HTTP
    // switch calls stay off the motor loop. Persisted separately in NVS.
    // -------------------------------------------------------------------------
    void configureDustCollector(int generation, const char* ip, const char* host = ""); // replaces + persists
    void removeDustCollector();
    bool dcConfigured() const { return _dustCollector != nullptr; }
    bool dcOn();                    // thread-safe read for status JSON

    // Manually force the dust collector on/off from the dashboard. Holds until
    // the next automatic gate change (tool on/off), then automation resumes.
    void setDcManual(bool on);

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

    // Dust collector plug (switchable). nullptr = not configured.
    SmartOutlet*      _dustCollector;
    bool              _dcOn;              // last commanded state (protected by _mutex)
    bool              _dcSynced;          // false = force a switch command on next reconcile
    bool              _dcManualOverride;  // true = follow _dcManualState, not gate state
    bool              _dcManualState;     // forced on/off while override active

    // Shared state between poll task and main loop — protected by _mutex
    int               _requestedStop;
    bool              _manualOverride;   // true = ignore outlet selection until next tool-on
    SemaphoreHandle_t _mutex;

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
    void        reconcileDustCollector();  // poll task: drive DC plug to desired state
    bool        provisionPushOutlets();    // poll task: push Ws/name config; returns true if any still pending
    void        checkLocalIpChange();      // poll task: rebuild _wsUrl + re-provision if our DHCP IP changed
    SmartOutlet* outletByIp(const char* ip); // match a push event to a configured outlet
};

#endif // CONTROL_SMART_OUTLET

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
                         float thresholdW);

    void removeOutlet(int slot);
    void saveSlot(int slot);       // persist a single slot to NVS
    void saveAll();                // persist all slots to NVS
    void printConfig();            // dump current config to Serial

    int          outletCount() const { return _count; }
    SmartOutlet* outlet(int i)       { return (i >= 0 && i < _count) ? _outlets[i] : nullptr; }

    // -------------------------------------------------------------------------
    // Dust collector plug — a switchable Shelly outlet (we turn it on/off)
    // rather than a power sensor. The poll task drives it on whenever a tool is
    // active (a real gate is selected) and off when idle, so its blocking HTTP
    // switch calls stay off the motor loop. Persisted separately in NVS.
    // -------------------------------------------------------------------------
    void configureDustCollector(int generation, const char* ip); // replaces + persists
    void removeDustCollector();
    bool dcConfigured() const { return _dustCollector != nullptr; }
    bool dcOn();                    // thread-safe read for status JSON

    // -------------------------------------------------------------------------
    // Manual override — bypasses outlet-driven gate selection until the next
    // time any outlet crosses its threshold (tool turned on).
    // Call from the main loop when the HTTP API receives a manual move command.
    // -------------------------------------------------------------------------
    void setManualOverride(int stop);
    bool isManualOverride();        // thread-safe read for status JSON

private:
    SmartOutlet*      _outlets[SMART_OUTLET_COUNT];
    int               _count;

    // Dust collector plug (switchable). nullptr = not configured.
    SmartOutlet*      _dustCollector;
    bool              _dcOn;              // last commanded state (protected by _mutex)
    bool              _dcSynced;          // false = force a switch command on next reconcile

    // Shared state between poll task and main loop — protected by _mutex
    int               _requestedStop;
    bool              _manualOverride;   // true = ignore outlet selection until next tool-on
    SemaphoreHandle_t _mutex;

    // Debounce tracking (poll task only — no mutex needed)
    int               _pendingStop;
    unsigned long     _pendingStartMs;

    static void pollTaskFn(void* param);
    void        doPoll();
    void        reconcileDustCollector();  // poll task: drive DC plug to desired state
};

#endif // CONTROL_SMART_OUTLET

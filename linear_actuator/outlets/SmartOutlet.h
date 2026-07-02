// =============================================================================
// SmartOutlet.h — Abstract base for any power-monitoring smart outlet
//
// Concrete implementations: ShellyGen1Outlet, ShellyGen2Outlet
// Future:  KasaOutlet, HomeAssistantOutlet, CTSensorOutlet, ...
//
// Thread safety: poll() is called from the background FreeRTOS task.
//   getPowerW() / isActive() may be read from the main loop — they read
//   only _lastPowerW and _thresholdW, which are float-aligned and written
//   atomically on Xtensa. For stricter guarantees, wrap reads in the
//   SmartOutletControl mutex.
// =============================================================================

#pragma once
#include <Arduino.h>

class SmartOutlet {
public:
    virtual ~SmartOutlet() {}

    // Fetch a fresh power reading from the outlet over the network.
    // Blocking — call only from the poll task, not from loop().
    // Returns true on success; false if unreachable or parse error.
    virtual bool poll() = 0;

    // Human-readable label for this outlet (e.g. "Table Saw")
    virtual const char* name() const = 0;

    // Last successfully polled power reading in watts.
    // Returns 0 if the outlet has never been reached or is offline.
    float getPowerW() const { return _lastPowerW; }

    // True when last reading is at or above the configured threshold.
    bool isActive() const { return _reachable && (_lastPowerW >= _thresholdW); }

    // True if the last poll() call succeeded.
    bool isReachable() const { return _reachable; }

    // Watts threshold above which the tool is considered "on".
    // Defaults to OUTLET_DEFAULT_THRESHOLD_W from config.h.
    void  setThresholdW(float w) { _thresholdW = w; }
    float getThresholdW() const  { return _thresholdW; }

    // Stop index this outlet maps to (1-based, matching NUM_STOPS).
    // 0 means unmapped / disabled.
    void setStopIndex(int i) { _stopIndex = i; }
    int  getStopIndex() const { return _stopIndex; }

    // IP address of this outlet on the local network
    virtual const char* ip() const = 0;

    // API generation (1 = Gen 1 /status, 2 = Gen 2+ /rpc/).
    // Used by saveSlot() to persist config; avoids RTTI / dynamic_cast.
    virtual int generation() const = 0;

protected:
    float _lastPowerW = 0.0f;
    float _thresholdW = 5.0f;
    int   _stopIndex  = 0;
    bool  _reachable  = false;
};

// =============================================================================
// SmartOutletControl.cpp
// =============================================================================

#include "SmartOutletControl.h"

#ifdef CONTROL_SMART_OUTLET

#include <WiFi.h>  // for WiFi.status() check in begin()
#include "../outlets/ShellyGen1Outlet.h"
#include "../outlets/ShellyGen2Outlet.h"
#include "../outlets/OutletConfig.h"

// =============================================================================
// Construction / destruction
// =============================================================================

SmartOutletControl::SmartOutletControl()
    : _count(0),
      _dustCollector(nullptr),
      _dcOn(false),
      _dcSynced(false),
      _dcManualOverride(false),
      _dcManualState(false),
      _requestedStop(0),
      _manualOverride(false),
      _mutex(nullptr),
      _pendingStop(-1),
      _pendingStartMs(0)
{
    memset(_outlets, 0, sizeof(_outlets));
}

SmartOutletControl::~SmartOutletControl() {
    for (int i = 0; i < SMART_OUTLET_COUNT; i++) {
        delete _outlets[i];
        _outlets[i] = nullptr;
    }
    delete _dustCollector;
    _dustCollector = nullptr;
}

// =============================================================================
// begin() — connect WiFi, load config, launch poll task
// =============================================================================

bool SmartOutletControl::begin() {
    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) {
        DEBUG_PRINTLN(F("[Outlets] Failed to create mutex."));
        return false;
    }

    // WiFiProvisioner::begin() in setup() guarantees WiFi is connected before
    // SmartOutletControl::begin() is called. Fail fast if that contract is broken.
    if (WiFi.status() != WL_CONNECTED) {
        DEBUG_PRINTLN(F("[Outlets] WiFi not connected — call WiFiProvisioner::begin() before SmartOutletControl::begin()."));
        return false;
    }

    // Load outlet mappings from NVS
    OutletEntry entries[SMART_OUTLET_COUNT];
    int n = OutletConfig::load(entries, SMART_OUTLET_COUNT);

    for (int i = 0; i < n; i++) {
        if (!entries[i].valid) continue;
        SmartOutlet* o = nullptr;
        if (entries[i].generation == 2) {
            o = new ShellyGen2Outlet(entries[i].ip, entries[i].name);
        } else {
            o = new ShellyGen1Outlet(entries[i].ip, entries[i].name);
        }
        o->setStopIndex(entries[i].stopIndex);
        o->setThresholdW(entries[i].thresholdW);
        _outlets[i] = o;
        _count = i + 1;
    }

    if (_count == 0) {
        DEBUG_PRINTLN(F("[Outlets] No outlets configured yet. Run setup agent to add outlets."));
    } else {
        DEBUG_PRINT(F("[Outlets] Loaded ")); Serial.print(_count); DEBUG_PRINTLN(F(" outlet(s) from NVS."));
        OutletConfig::print(entries, _count);
    }

    // Load the dust collector plug (optional)
    DustCollectorEntry dc;
    if (OutletConfig::loadDustCollector(dc)) {
        _dustCollector = (dc.generation == 2)
                       ? (SmartOutlet*)new ShellyGen2Outlet(dc.ip, "Dust Collector")
                       : (SmartOutlet*)new ShellyGen1Outlet(dc.ip, "Dust Collector");
        _dcSynced = false; // force initial off/on sync on first poll
        DEBUG_PRINT(F("[Outlets] Dust collector plug: gen"));
        Serial.print(dc.generation); DEBUG_PRINT(F(" @ ")); Serial.println(dc.ip);
    }

    // Launch polling task on Core 0 (Arduino/motor loop runs on Core 1)
    xTaskCreatePinnedToCore(
        pollTaskFn,   // task function
        "outletPoll", // name (for debugging)
        8192,         // stack bytes — HTTPClient + JSON needs headroom
        this,         // parameter
        1,            // priority (same as loop; yields to Core 1 motor updates)
        nullptr,      // task handle (not needed)
        0             // Core 0
    );

    DEBUG_PRINTLN(F("[Outlets] Poll task started."));
    return true;
}

void SmartOutletControl::update() {
    // Polling runs on its own task — nothing needed here.
}

// =============================================================================
// readRequestedStop() — called from main loop (Core 1)
// =============================================================================

int SmartOutletControl::readRequestedStop() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    int stop = _requestedStop;
    xSemaphoreGive(_mutex);
    return stop;
}

// =============================================================================
// Poll task — runs on Core 0, every OUTLET_POLL_INTERVAL_MS
// =============================================================================

void SmartOutletControl::pollTaskFn(void* param) {
    SmartOutletControl* self = static_cast<SmartOutletControl*>(param);
    while (true) {
        self->doPoll();
        vTaskDelay(pdMS_TO_TICKS(OUTLET_POLL_INTERVAL_MS));
    }
}

void SmartOutletControl::doPoll() {
    if (_count == 0) {
        // No sensor outlets — but a dust collector plug may still be configured,
        // so keep it reconciled (it will stay off since _requestedStop is 0).
        reconcileDustCollector();
        return;
    }

    // Find the outlet drawing the most power above its threshold
    int   bestStop  = 0;   // 0 = no active tool → home position
    float bestPower = 0.0f;

    for (int i = 0; i < _count; i++) {
        SmartOutlet* o = _outlets[i];
        if (!o) continue;
        if (strlen(o->ip()) == 0) continue;  // name-only gate — no plug to poll

        o->poll();

        if (o->isActive() && o->getPowerW() > bestPower) {
            bestPower = o->getPowerW();
            bestStop  = o->getStopIndex();
        }
    }

    // Debounce: the same stop must win for its full debounce window before we
    // commit. "Off" (stop=0) gets a longer window to avoid bouncing home on
    // brief idle moments (e.g. table saw coasting between cuts).
    unsigned long now = millis();

    if (bestStop != _pendingStop) {
        // Restart the debounce window — nothing commits this tick.
        _pendingStop    = bestStop;
        _pendingStartMs = now;
    } else {
        unsigned long window = (bestStop == 0) ? OUTLET_OFF_DEBOUNCE_MS
                                               : OUTLET_ON_DEBOUNCE_MS;
        if (now - _pendingStartMs >= window) {
            xSemaphoreTake(_mutex, portMAX_DELAY);
            // A tool powering on clears any manual override so outlet control resumes
            if (_manualOverride && bestStop != 0) {
                _manualOverride = false;
                DEBUG_PRINTLN(F("[Outlets] Manual override cleared — tool detected."));
            }
            if (!_manualOverride && _requestedStop != bestStop) {
                DEBUG_PRINT(F("[Outlets] → stop ")); Serial.println(bestStop);
                _requestedStop = bestStop;
                // An automatic tool on/off event resumes automatic DC control,
                // releasing any manual dashboard override.
                _dcManualOverride = false;
            }
            xSemaphoreGive(_mutex);
        }
    }

    // Keep the dust collector plug in sync with the committed gate selection.
    reconcileDustCollector();
}

// =============================================================================
// Dust collector reconciliation — poll task (Core 0)
// =============================================================================

void SmartOutletControl::reconcileDustCollector() {
    if (!_dustCollector) return;

    // Desired: while a manual override is active, follow the forced state;
    // otherwise ON whenever a real gate is selected (a tool is running or a
    // manual move targeted a gate) and OFF at home. _requestedStop is already
    // debounced by the poll logic above.
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool desired = _dcManualOverride ? _dcManualState : (_requestedStop > 0);
    bool needsSwitch = !_dcSynced || (desired != _dcOn);
    xSemaphoreGive(_mutex);

    if (!needsSwitch) return;

    // Blocking HTTP — safe here (poll task, Core 0), never on the motor loop.
    if (_dustCollector->setSwitch(desired)) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _dcOn     = desired;
        _dcSynced = true;
        xSemaphoreGive(_mutex);
        DEBUG_PRINT(F("[Outlets] Dust collector ")); DEBUG_PRINTLN(desired ? F("ON") : F("OFF"));
    }
    // On failure, leave _dcSynced so we retry on the next poll tick.
}

// =============================================================================
// Setup agent API
// =============================================================================

void SmartOutletControl::configureOutlet(int slot, int generation,
                                         const char* ip, const char* name,
                                         int stopIndex, float thresholdW) {
    if (slot < 0 || slot >= SMART_OUTLET_COUNT) return;

    // Replace existing outlet object
    delete _outlets[slot];

    SmartOutlet* o = nullptr;
    if (generation == 2) {
        o = new ShellyGen2Outlet(ip, name);
    } else {
        o = new ShellyGen1Outlet(ip, name);
    }
    o->setStopIndex(stopIndex);
    o->setThresholdW(thresholdW);
    _outlets[slot] = o;

    if (slot >= _count) _count = slot + 1;

    DEBUG_PRINT(F("[Outlets] Slot ")); Serial.print(slot);
    DEBUG_PRINT(F(" configured: ")); Serial.print(name);
    DEBUG_PRINT(F(" @ ")); Serial.print(ip);
    DEBUG_PRINT(F(" → stop ")); Serial.println(stopIndex);
}

void SmartOutletControl::removeOutlet(int slot) {
    if (slot < 0 || slot >= SMART_OUTLET_COUNT) return;
    delete _outlets[slot];
    _outlets[slot] = nullptr;
}

void SmartOutletControl::clearAllOutlets() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    for (int i = 0; i < SMART_OUTLET_COUNT; i++) {
        delete _outlets[i];
        _outlets[i] = nullptr;
    }
    _count = 0;
    delete _dustCollector;
    _dustCollector = nullptr;
    _dcOn     = false;
    _dcSynced = false;
    xSemaphoreGive(_mutex);

    OutletConfig::erase();
    DEBUG_PRINTLN(F("[Outlets] All outlet config cleared (RAM + NVS)."));
}

void SmartOutletControl::saveSlot(int slot) {
    if (slot < 0 || slot >= _count || !_outlets[slot]) return;
    SmartOutlet* o = _outlets[slot];

    OutletEntry e;
    e.generation = o->generation();
    strlcpy(e.ip,   o->ip(),   sizeof(e.ip));
    strlcpy(e.name, o->name(), sizeof(e.name));
    e.stopIndex  = o->getStopIndex();
    e.thresholdW = o->getThresholdW();
    e.valid      = true;

    OutletConfig::saveSlot(slot, e);
}

void SmartOutletControl::saveAll() {
    for (int i = 0; i < _count; i++) saveSlot(i);
}

// -----------------------------------------------------------------------------
// Dust collector plug config (setup agent API)
// -----------------------------------------------------------------------------

void SmartOutletControl::configureDustCollector(int generation, const char* ip) {
    // Swap the plug object. Same lifetime assumption as configureOutlet: config
    // changes are rare and the poll task tolerates a brief window here.
    xSemaphoreTake(_mutex, portMAX_DELAY);
    delete _dustCollector;
    _dustCollector = (generation == 2)
                   ? (SmartOutlet*)new ShellyGen2Outlet(ip, "Dust Collector")
                   : (SmartOutlet*)new ShellyGen1Outlet(ip, "Dust Collector");
    _dcOn     = false;
    _dcSynced = false;   // force a switch command on the next reconcile
    xSemaphoreGive(_mutex);

    DustCollectorEntry e;
    e.generation = generation;
    strlcpy(e.ip, ip, sizeof(e.ip));
    e.valid = true;
    OutletConfig::saveDustCollector(e);

    DEBUG_PRINT(F("[Outlets] Dust collector configured: gen"));
    Serial.print(generation); DEBUG_PRINT(F(" @ ")); Serial.println(ip);
}

void SmartOutletControl::removeDustCollector() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    delete _dustCollector;
    _dustCollector = nullptr;
    _dcOn     = false;
    _dcSynced = false;
    xSemaphoreGive(_mutex);
    OutletConfig::eraseDustCollector();
    DEBUG_PRINTLN(F("[Outlets] Dust collector plug removed."));
}

bool SmartOutletControl::dcOn() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = _dcOn;
    xSemaphoreGive(_mutex);
    return v;
}

void SmartOutletControl::setDcManual(bool on) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _dcManualOverride = true;
    _dcManualState    = on;
    xSemaphoreGive(_mutex);
    DEBUG_PRINT(F("[Outlets] Dust collector manual → ")); DEBUG_PRINTLN(on ? F("ON") : F("OFF"));
    // The poll task's next reconcile (≤ OUTLET_POLL_INTERVAL_MS) applies it.
}

void SmartOutletControl::setManualOverride(int stop) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _requestedStop  = stop;
    _manualOverride = true;
    xSemaphoreGive(_mutex);
    DEBUG_PRINT(F("[Outlets] Manual override → stop ")); Serial.println(stop);
}

bool SmartOutletControl::isManualOverride() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = _manualOverride;
    xSemaphoreGive(_mutex);
    return v;
}

void SmartOutletControl::printConfig() {
    Serial.println(F("--- Active Outlet Config ---"));
    for (int i = 0; i < _count; i++) {
        SmartOutlet* o = _outlets[i];
        if (!o) { Serial.print(F("  [")); Serial.print(i); Serial.println(F("] (empty)")); continue; }
        Serial.print(F("  [")); Serial.print(i); Serial.print(F("] "));
        Serial.print(o->name());
        Serial.print(F("  stop=")); Serial.print(o->getStopIndex());
        Serial.print(F("  thr=")); Serial.print(o->getThresholdW(), 1); Serial.print(F("W"));
        Serial.print(F("  last=")); Serial.print(o->getPowerW(), 1); Serial.print(F("W"));
        Serial.print(F("  ")); Serial.println(o->isReachable() ? F("online") : F("OFFLINE"));
    }
    Serial.println(F("----------------------------"));
}

#endif // CONTROL_SMART_OUTLET

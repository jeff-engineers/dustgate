// =============================================================================
// SmartOutletControl.cpp
// =============================================================================

#include "SmartOutletControl.h"

#ifdef CONTROL_SMART_OUTLET

#include <WiFi.h>  // for WiFi.status() check in begin()
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
      _activeStop(0),
      _manualOverride(false),
      _mutex(nullptr),
      _pollTaskHandle(nullptr),
      _needsProvision(true),
      _provisionPending(false),
      _lastProvisionMs(0),
      _lastLocalIp(0),
      _pendingStop(-1),
      _pendingStartMs(0)
{
    memset(_outlets, 0, sizeof(_outlets));
    memset(_prevActive, 0, sizeof(_prevActive));
    _wsUrl[0] = '\0';
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

    // Build the Outbound-WebSocket URL plugs dial back to. Uses our current IP,
    // so it's refreshed every boot — self-healing across DHCP lease changes.
    snprintf(_wsUrl, sizeof(_wsUrl), "ws://%s:%d/shelly-rpc",
             WiFi.localIP().toString().c_str(), API_PORT);
    _lastLocalIp = (uint32_t)WiFi.localIP();
    DEBUG_PRINT(F("[Outlets] Push endpoint for plugs: ")); DEBUG_PRINTLN(_wsUrl);

    // Load outlet mappings from NVS
    OutletEntry entries[SMART_OUTLET_COUNT];
    int n = OutletConfig::load(entries, SMART_OUTLET_COUNT);

    for (int i = 0; i < n; i++) {
        if (!entries[i].valid) continue;
        // Only Gen2+ plugs are supported (Gen1 dropped); stored generation is
        // always >= 2, and Gen3+ speaks the same RPC dialect as Gen2.
        SmartOutlet* o = new ShellyGen2Outlet(entries[i].ip, entries[i].name);
        o->setStopIndex(entries[i].stopIndex);
        o->setThresholdW(entries[i].thresholdW);
        o->setHost(entries[i].host);
        _outlets[i] = o;
        _count = i + 1;
    }

    if (_count == 0) {
        DEBUG_PRINTLN(F("[Outlets] No outlets configured yet. Run setup to add outlets."));
    } else {
        DEBUG_PRINT(F("[Outlets] Loaded ")); Serial.print(_count); DEBUG_PRINTLN(F(" outlet(s) from NVS."));
        OutletConfig::print(entries, _count);
    }

    // Load the dust collector plug (optional)
    DustCollectorEntry dc;
    if (OutletConfig::loadDustCollector(dc)) {
        _dustCollector = new ShellyGen2Outlet(dc.ip, "Dust Collector");
        _dustCollector->setHost(dc.host);
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
        1,               // priority (same as loop; yields to Core 1 motor updates)
        &_pollTaskHandle, // task handle — used by setDcManual() to wake the task
        0                // Core 0
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
        // Sleep until the next poll interval OR until woken early by a manual
        // dust-collector toggle (setDcManual → xTaskNotifyGive). ulTaskNotifyTake
        // returns immediately when a notification is pending, so a manual switch
        // is applied on the very next reconcile instead of after up to a full
        // OUTLET_POLL_INTERVAL_MS. clearCountOnExit=pdTRUE resets the count so
        // each wake consumes exactly one notification.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(OUTLET_POLL_INTERVAL_MS));
    }
}

// Poll task: detect our own DHCP address changing at runtime. The push URL we
// hand plugs is built from our IP; if a lease renewal moves us to a new address,
// plugs keep dialing the old (now-dead) URL forever — push silently falls back to
// polling and never recovers until reboot. On a change, rebuild the URL and force
// every plug to be re-provisioned so it dials the new address. Ignores 0.0.0.0
// (transient during a WiFi drop) so a brief disconnect doesn't churn provisioning.
void SmartOutletControl::checkLocalIpChange() {
    uint32_t ip = (uint32_t)WiFi.localIP();
    if (ip == 0 || ip == _lastLocalIp) return;
    _lastLocalIp = ip;
    snprintf(_wsUrl, sizeof(_wsUrl), "ws://%s:%d/shelly-rpc",
             WiFi.localIP().toString().c_str(), API_PORT);
    DEBUG_PRINT(F("[Outlets] Local IP changed — new push endpoint: ")); DEBUG_PRINTLN(_wsUrl);
    for (int i = 0; i < _count; i++) {
        if (!_outlets[i]) continue;
        _outlets[i]->setProvisioned(false);    // re-send Ws config with the new URL
        _outlets[i]->setPushConnected(false);  // old connection is to the stale address
    }
    _needsProvision = true;
}

void SmartOutletControl::doPoll() {
    checkLocalIpChange();   // re-point plugs if our DHCP IP moved out from under us

    // Push-provision plugs (Ws + name) when flagged (first run after boot, and
    // after any outlet (re)configuration), and periodically retry as long as
    // some plug still needs it — e.g. one that was briefly unreachable at boot.
    // Steady state (all plugs pushing) does no HTTP here.
    {
        unsigned long now = millis();
        if (_needsProvision ||
            (_provisionPending && (now - _lastProvisionMs) >= OUTLET_PROVISION_RETRY_MS)) {
            _needsProvision   = false;
            _lastProvisionMs  = now;
            _provisionPending = provisionPushOutlets();
        }
    }

    if (_count == 0) {
        // No sensor outlets — but a dust collector plug may still be configured,
        // so keep it reconciled (it will stay off since _activeStop is 0).
        reconcileDustCollector();
        return;
    }

    // Find the outlet drawing the most power above its threshold, and detect
    // whether any outlet just crossed OFF→ON this tick (edge, not level) —
    // used below to decide whether to clear a manual override. Checking
    // level (any outlet currently active) instead of edge was the bug: a
    // tool already running before a manual move kept bestStop pinned to its
    // gate, so the very next poll tick saw "a tool is active" and immediately
    // clobbered the user's manual choice.
    int   bestStop   = 0;   // 0 = no active tool → home position
    float bestPower  = 0.0f;
    bool  risingEdge = false;

    for (int i = 0; i < _count; i++) {
        SmartOutlet* o = _outlets[i];
        if (!o) continue;
        if (strlen(o->ip()) == 0) continue;  // name-only gate — no plug to poll

        // Push-connected plugs stream their power over the WebSocket, so skip
        // the HTTP poll entirely (this is what removes the polling storm).
        // Only fall back to an HTTP poll for a plug whose push isn't up.
        if (!o->isPushConnected()) o->poll();

        bool active = o->isActive();
        if (active && !_prevActive[i]) risingEdge = true;
        _prevActive[i] = active;

        if (active && o->getPowerW() > bestPower) {
            bestPower = o->getPowerW();
            bestStop  = o->getStopIndex();
        }
    }

    // Clear manual override only on a fresh power-on, checked independently
    // of the debounce window below (and every tick, not just when it
    // commits) so a still-running tool from before the override can't
    // re-trigger it just because it's still "active."
    if (risingEdge) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        if (_manualOverride) {
            _manualOverride = false;
            DEBUG_PRINTLN(F("[Outlets] Manual override cleared — tool freshly powered on."));
        }
        xSemaphoreGive(_mutex);
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
            // Track the active tool (0 = idle) for dust-collector control, and
            // release any manual DC override on a real tool on/off event.
            if (bestStop != _activeStop) {
                _activeStop = bestStop;
                _dcManualOverride = false;
            }
            // Move the gate only to an ACTIVE tool. At idle (bestStop==0) HOLD the
            // last position — never auto-return home. Keeps a duct path open (a
            // manual collector start can't dead-head) and avoids needless wear on a
            // brief tool-off; the dust collector still switches off via _activeStop.
            if (!_manualOverride && bestStop > 0 && _requestedStop != bestStop) {
                DEBUG_PRINT(F("[Outlets] → stop ")); Serial.println(bestStop);
                _requestedStop = bestStop;
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
    // otherwise ON whenever a TOOL is actively running and OFF when idle. Keyed
    // to _activeStop (debounced tool activity), NOT _requestedStop — at idle the
    // gate holds at its last position (_requestedStop stays > 0) but no tool is
    // running, so the collector must switch off.
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool desired = _dcManualOverride ? _dcManualState : (_activeStop > 0);
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
// Push (Gen2 Outbound WebSocket) — provisioning + inbound handlers
// =============================================================================

// Match a push event's source IP to a configured outlet. Poll task and the
// HTTP server's WS callback both call this; the outlet array only grows/shrinks
// during (rare) reconfiguration, so a lock-free scan is acceptable here.
SmartOutlet* SmartOutletControl::outletByIp(const char* ip) {
    if (!ip || !ip[0]) return nullptr;
    for (int i = 0; i < _count; i++) {
        SmartOutlet* o = _outlets[i];
        if (o && strcmp(o->ip(), ip) == 0) return o;
    }
    return nullptr;
}

// Poll task: tell every configured Gen2 plug to open an Outbound WebSocket back
// to us, and set its friendly name to the gate's name. Blocking HTTP — safe on
// the poll task. Best-effort: a plug that's briefly unreachable is retried on
// the next provisioning pass (e.g. after the next reconfigure or reboot).
bool SmartOutletControl::provisionPushOutlets() {
    if (_wsUrl[0] == '\0') return false;
    DEBUG_PRINT(F("[Outlets] Provisioning plugs (free heap "));
    DEBUG_PRINT(ESP.getFreeHeap()); DEBUG_PRINTLN(F(" bytes)..."));

    bool pending = false;
    for (int i = 0; i < _count; i++) {
        SmartOutlet* o = _outlets[i];
        if (!o || strlen(o->ip()) == 0) continue;  // name-only gate — no plug

        // Skip plugs we've already configured (or that are already pushing) —
        // re-POSTing the same config every retry is pointless and just churns
        // the network. A plug that's provisioned but not connecting is a
        // separate problem that re-sending config won't fix.
        if (o->isPushConnected() || o->isProvisioned()) continue;

        // Reachability probe before the slow flash-writing SetConfig POSTs. Uses
        // a generous timeout (not the 400ms poll timeout) so a marginal plug gets
        // a fair chance to answer here; if it still can't, don't hammer it with
        // 3s-timeout writes — mark it pending and retry later.
        bool reachable = o->probe(OUTLET_PROVISION_PROBE_TIMEOUT_MS);
        DEBUG_PRINT(F("[Outlets] provision ")); DEBUG_PRINT(o->ip());
        DEBUG_PRINT(F(" reachable(GetStatus)=")); DEBUG_PRINTLN(reachable ? F("yes") : F("no"));
        if (!reachable) { pending = true; continue; }

        // Name FIRST, while the plug is idle. Doing Ws.SetConfig first makes the
        // plug (re)open its outbound WebSocket, and the immediately-following
        // name write was landing while it was busy — so set the name, let it
        // settle, then point its push connection at us.
        if (strlen(o->name()) > 0) { o->setName(o->name()); delay(150); }
        if (o->configureOutboundWs(_wsUrl)) o->setProvisioned(true);
        else                                pending = true;
        delay(50);
    }

    if (pending) DEBUG_PRINTLN(F("[Outlets] Some plugs unprovisioned — will retry."));
    return pending;
}

void SmartOutletControl::onPushConnect(const char* ip) {
    SmartOutlet* o = outletByIp(ip);
    if (!o) return;   // not one of ours — ignore
    o->setPushConnected(true);
    DEBUG_PRINT(F("[Outlets] Push connected: ")); DEBUG_PRINTLN(ip);
    if (_pollTaskHandle) xTaskNotifyGive(_pollTaskHandle);
}

void SmartOutletControl::onPushedPower(const char* ip, float apower) {
    SmartOutlet* o = outletByIp(ip);
    if (!o) return;
    o->setPushConnected(true);   // a push implies the connection is live
    o->setPushedPower(apower);
    // Wake the poll task so a tool turning on/off is acted on immediately
    // (subject to the usual on/off debounce), not at the next 500ms tick.
    if (_pollTaskHandle) xTaskNotifyGive(_pollTaskHandle);
}

void SmartOutletControl::onPushDisconnect(const char* ip) {
    SmartOutlet* o = outletByIp(ip);
    if (!o) return;
    o->setPushConnected(false);  // clears reachable/power → doPoll falls back to HTTP
    DEBUG_PRINT(F("[Outlets] Push disconnected: ")); DEBUG_PRINTLN(ip);
    if (_pollTaskHandle) xTaskNotifyGive(_pollTaskHandle);
}

// =============================================================================
// Setup API
// =============================================================================

void SmartOutletControl::configureOutlet(int slot, int generation,
                                         const char* ip, const char* name,
                                         int stopIndex, float thresholdW,
                                         const char* host) {
    if (slot < 0 || slot >= SMART_OUTLET_COUNT) return;

    // Replace existing outlet object
    delete _outlets[slot];

    // Gen2+ only (Gen1 dropped); `generation` is retained in the config/API
    // for compatibility but is always >= 2.
    (void)generation;
    SmartOutlet* o = new ShellyGen2Outlet(ip, name);
    o->setStopIndex(stopIndex);
    o->setThresholdW(thresholdW);
    o->setHost(host);
    _outlets[slot] = o;

    if (slot >= _count) _count = slot + 1;

    DEBUG_PRINT(F("[Outlets] Slot ")); Serial.print(slot);
    DEBUG_PRINT(F(" configured: ")); Serial.print(name);
    DEBUG_PRINT(F(" @ ")); Serial.print(ip);
    DEBUG_PRINT(F(" → stop ")); Serial.println(stopIndex);

    // Flag the new plug for push provisioning (Ws.SetConfig + name write) and
    // wake the poll task so it happens now rather than at the next tick.
    _needsProvision = true;
    if (_pollTaskHandle) xTaskNotifyGive(_pollTaskHandle);
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
    strlcpy(e.host, o->host(), sizeof(e.host));
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
// Dust collector plug config (setup API)
// -----------------------------------------------------------------------------

void SmartOutletControl::configureDustCollector(int generation, const char* ip, const char* host) {
    // Swap the plug object. Same lifetime assumption as configureOutlet: config
    // changes are rare and the poll task tolerates a brief window here.
    xSemaphoreTake(_mutex, portMAX_DELAY);
    delete _dustCollector;
    _dustCollector = new ShellyGen2Outlet(ip, "Dust Collector");  // Gen2+ only
    _dustCollector->setHost(host);
    _dcOn     = false;
    _dcSynced = false;   // force a switch command on the next reconcile
    xSemaphoreGive(_mutex);

    DustCollectorEntry e;
    e.generation = generation;
    strlcpy(e.ip, ip, sizeof(e.ip));
    strlcpy(e.host, host, sizeof(e.host));
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
    // Wake the poll task now so the switch is applied immediately rather than
    // waiting up to OUTLET_POLL_INTERVAL_MS for its next scheduled reconcile.
    if (_pollTaskHandle) xTaskNotifyGive(_pollTaskHandle);
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

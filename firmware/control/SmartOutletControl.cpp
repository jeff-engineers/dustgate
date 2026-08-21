// =============================================================================
// SmartOutletControl.cpp
// =============================================================================

#include "SmartOutletControl.h"

#ifdef CONTROL_SMART_OUTLET

#include <WiFi.h>  // for WiFi.status() check in begin()
#include "../outlets/ShellyGen2Outlet.h"
#include "../outlets/OutletConfig.h"
#include "../outlets/PlugClaim.h"        // who owns a plug, and what we may do to it
#include "../utils/WiFiConfig.h"         // getHostname() — the owner we write into names

// =============================================================================
// Construction / destruction
// =============================================================================

SmartOutletControl::SmartOutletControl()
    : _count(0),
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
    memset(_retired, 0, sizeof(_retired));
    memset(_retiredCycle, 0, sizeof(_retiredCycle));
    _retiredCount = 0;
    _pollCycle    = 0;
    _retireMutex  = xSemaphoreCreateMutex();
    memset(_prevActive, 0, sizeof(_prevActive));
    memset(_collectors, 0, sizeof(_collectors));
    memset(_dcOn, 0, sizeof(_dcOn));
    memset(_dcSynced, 0, sizeof(_dcSynced));
    memset(_dcManualOverride, 0, sizeof(_dcManualOverride));
    memset(_dcManualState, 0, sizeof(_dcManualState));
    _wsUrl[0]   = '\0';
    _ourHost[0] = '\0';
    _ourName[0] = '\0';
}

SmartOutletControl::~SmartOutletControl() {
    for (int i = 0; i < SMART_OUTLET_COUNT; i++) {
        delete _outlets[i];
        _outlets[i] = nullptr;
    }
    for (int i = 0; i < COLLECTOR_COUNT; i++) {
        delete _collectors[i];
        _collectors[i] = nullptr;
    }
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
    strlcpy(_ourHost, WiFi.localIP().toString().c_str(), sizeof(_ourHost));
    strlcpy(_ourName, WiFiProvisioner::getHostname().c_str(), sizeof(_ourName));
    DEBUG_PRINT(F("[Outlets] Push endpoint for plugs: ")); DEBUG_PRINTLN(_wsUrl);
    DEBUG_PRINT(F("[Outlets] Plug owner name: ")); DEBUG_PRINTLN(_ourName);

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

    // Load the persisted collector plug (optional). Slot 0 only — the other
    // slots are rebuilt from the layout on adopt, so there is nothing to load.
    DustCollectorEntry dc;
    if (OutletConfig::loadDustCollector(dc)) {
        _collectors[0] = new ShellyGen2Outlet(dc.ip, "Dust Collector");
        _collectors[0]->setHost(dc.host);
        _dcSynced[0] = false; // force initial off/on sync on first poll
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

// =============================================================================
// Retire / reap — see the long note in the header for why plugs are not deleted
// where they are reconfigured.
// =============================================================================

void SmartOutletControl::retire(SmartOutlet* o) {
    if (!o) return;
    xSemaphoreTake(_retireMutex, portMAX_DELAY);
    if (_retiredCount >= RETIRE_SLOTS) {
        // Only reachable if the shop is reconfigured more times in two poll
        // cycles than it has plug slots. Freeing the oldest is the least-bad
        // answer — it has already survived at least one full cycle — but say so
        // out loud, because it is the one path here that can still race.
        DEBUG_PRINTLN(F("[Outlets] retire list full — freeing the oldest early"));
        delete _retired[0];
        for (int i = 1; i < RETIRE_SLOTS; i++) {
            _retired[i - 1]      = _retired[i];
            _retiredCycle[i - 1] = _retiredCycle[i];
        }
        _retiredCount = RETIRE_SLOTS - 1;
    }
    _retired[_retiredCount]      = o;
    _retiredCycle[_retiredCount] = _pollCycle;
    _retiredCount++;
    xSemaphoreGive(_retireMutex);
}

void SmartOutletControl::reapRetired() {
    xSemaphoreTake(_retireMutex, portMAX_DELAY);
    int keep = 0;
    for (int i = 0; i < _retiredCount; i++) {
        // Unsigned subtraction, so this stays right across the millis-scale
        // wrap of the cycle counter.
        if ((uint32_t)(_pollCycle - _retiredCycle[i]) >= 2) {
            delete _retired[i];
            continue;
        }
        _retired[keep]      = _retired[i];
        _retiredCycle[keep] = _retiredCycle[i];
        keep++;
    }
    _retiredCount = keep;
    xSemaphoreGive(_retireMutex);
}

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
    // Ownership is compared against this, so it moves with the URL. Without it a
    // plug we own would read as "pointed at a stranger" after a lease change and
    // we would refuse to re-provision our own plug.
    strlcpy(_ourHost, WiFi.localIP().toString().c_str(), sizeof(_ourHost));
    DEBUG_PRINT(F("[Outlets] Local IP changed — new push endpoint: ")); DEBUG_PRINTLN(_wsUrl);
    for (int i = 0; i < _count; i++) {
        if (!_outlets[i]) continue;
        _outlets[i]->setProvisioned(false);    // re-send Ws config with the new URL
        _outlets[i]->setPushConnected(false);  // old connection is to the stale address
    }
    _needsProvision = true;
}

void SmartOutletControl::doPoll() {
    // Top of the pass, before a single plug pointer is read: this is the one
    // place in the firmware that frees a SmartOutlet, and the only point where
    // this task provably holds none of them.
    _pollCycle++;
    reapRetired();

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
        reconcileCollectors();
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

    // Read without the lock, deliberately. Lifetime is safe because nothing frees
    // a plug except this task, at the top of doPoll() (see retire()) — so the
    // worst case here is polling a plug that was reconfigured a moment ago, which
    // costs one stale reading and is fixed on the next pass. Taking _mutex across
    // this loop would put every API request behind seconds of plug HTTP.
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
                // Slot 0 only: it is the one this legacy path has an opinion
                // about. Releasing the others would hand a system's blower to an
                // automation that has never heard of it, and switch it off.
                _dcManualOverride[0] = false;
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
    reconcileCollectors();
}

// =============================================================================
// Collector reconciliation — poll task (Core 0)
// =============================================================================

void SmartOutletControl::reconcileCollectors() {
    for (int i = 0; i < COLLECTOR_COUNT; i++) {
        if (!_collectors[i]) continue;

        // Desired: while an override is active, follow the forced state.
        // Otherwise fall back to the legacy stop-index automation — ON whenever a
        // TOOL is actively running, OFF at idle — which only slot 0 has. The
        // others have no automation to fall back to and simply stay off until
        // routing says otherwise, which is correct: nothing else in the firmware
        // knows what a second system's blower is for.
        //
        // The legacy rule is keyed to _activeStop (debounced tool activity), NOT
        // _requestedStop: at idle the gate holds at its last position
        // (_requestedStop stays > 0) but no tool is running, so it must switch off.
        xSemaphoreTake(_mutex, portMAX_DELAY);
        bool desired = _dcManualOverride[i] ? _dcManualState[i]
                                            : (i == 0 && _activeStop > 0);
        bool needsSwitch = !_dcSynced[i] || (desired != _dcOn[i]);
        xSemaphoreGive(_mutex);

        if (!needsSwitch) continue;

        // Blocking HTTP — safe here (poll task, Core 0), never on the motor loop.
        if (_collectors[i]->setSwitch(desired)) {
            xSemaphoreTake(_mutex, portMAX_DELAY);
            _dcOn[i]     = desired;
            _dcSynced[i] = true;
            xSemaphoreGive(_mutex);
            DEBUG_PRINT(F("[Outlets] Collector ")); Serial.print(i);
            DEBUG_PRINT(F(" ")); DEBUG_PRINTLN(desired ? F("ON") : F("OFF"));
        }
        // On failure, leave _dcSynced so we retry on the next poll tick.
    }
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

        // ...and skip, permanently, anything that belongs to someone else. A
        // poll-only plug is paired and working; it just never gets its Ws config
        // rewritten. Checked before the probe so a shared plug costs nothing.
        if (o->isPollOnly()) continue;

        // Reachability probe before the slow flash-writing SetConfig POSTs. Uses
        // a generous timeout (not the 400ms poll timeout) so a marginal plug gets
        // a fair chance to answer here; if it still can't, don't hammer it with
        // 3s-timeout writes — mark it pending and retry later.
        bool reachable = o->probe(OUTLET_PROVISION_PROBE_TIMEOUT_MS);
        DEBUG_PRINT(F("[Outlets] provision ")); DEBUG_PRINT(o->ip());
        DEBUG_PRINT(F(" reachable(GetStatus)=")); DEBUG_PRINTLN(reachable ? F("yes") : F("no"));
        if (!reachable) { pending = true; continue; }

        // ── WHO OWNS THIS PLUG? (RFC §8) ────────────────────────────────────
        // Ask before writing. Ws.SetConfig is silent theft when the answer is
        // "someone else": the previous owner stops hearing that its tool
        // started, with no error on either side.
        //
        // A FAILED READ IS NOT PERMISSION. If the plug won't answer Ws.GetConfig
        // we leave it alone and retry next pass — "I couldn't tell" must never
        // collapse into "so I took it".
        String server; bool wsEnabled = false;
        if (!o->readPushConfig(server, wsEnabled)) {
            DEBUG_PRINT(F("[Outlets] ")); DEBUG_PRINT(o->ip());
            DEBUG_PRINTLN(F(" — couldn't read Ws config; leaving it alone, will retry."));
            pending = true;
            continue;
        }

        const plugclaim::Claim claim = plugclaim::decide(
            server.c_str(), wsEnabled, _ourHost, o->name() ? o->name() : "", _ourName);

        // The `confirmed` argument is the user's explicit takeover approval and
        // comes from exactly one place: POST /api/outlets/takeover. This is a
        // background pass, so it can only ever pass what the user already said.
        if (!plugclaim::mayRepoint(claim, o->takeoverApproved())) {
            // Paired READ-ONLY. Polling gets us the wattage, which is the whole
            // job of a sensor plug, so this is a working pairing and not a
            // failure — hence no `pending`, and no retry.
            o->setPollOnly(true);
            DEBUG_PRINT(F("[Outlets] ")); DEBUG_PRINT(o->ip());
            DEBUG_PRINT(F(" is ")); DEBUG_PRINT(plugclaim::stateName(claim.state));
            DEBUG_PRINT(F(" (")); DEBUG_PRINT(claim.reason.c_str());
            DEBUG_PRINTLN(F(") — polling it, NOT repointing its push target."));
            continue;
        }
        if (o->takeoverApproved() && claim.takeable) {
            // Say it in the log, loudly and once. A takeover is the one thing
            // here that breaks something on another machine, and the serial log
            // is where anyone debugging THAT machine will end up looking.
            DEBUG_PRINT(F("[Outlets] TAKEOVER (user-confirmed) of ")); DEBUG_PRINT(o->ip());
            DEBUG_PRINT(F(" from ")); DEBUG_PRINT(claim.holder.c_str());
            DEBUG_PRINT(F(" — previous push target: ")); DEBUG_PRINTLN(server);
            o->setPreviousPushUrl(server.c_str());   // so unpairing can hand it back
        }
        o->setPollOnly(false);

        // Name FIRST, while the plug is idle. Doing Ws.SetConfig first makes the
        // plug (re)open its outbound WebSocket, and the immediately-following
        // name write was landing while it was busy — so set the name, let it
        // settle, then point its push connection at us.
        //
        // The name carries the owner for HUMANS: in the Shelly app there is no
        // DustGate UI to explain why a plug is spoken for, so it says so itself.
        if (strlen(o->name()) > 0) {
            std::string label, owner;
            plugclaim::parseName(o->name(), label, owner);   // never double-suffix
            o->setName(plugclaim::formatName(label, _ourHost).c_str());
            delay(150);
        }
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

    // Gen2+ only (Gen1 dropped); `generation` is retained in the config/API
    // for compatibility but is always >= 2.
    (void)generation;
    // Built BEFORE the lock: constructing a plug touches the heap and no other
    // task can see it yet, so there is nothing to serialize.
    SmartOutlet* o = new ShellyGen2Outlet(ip, name);
    o->setStopIndex(stopIndex);
    o->setThresholdW(thresholdW);
    o->setHost(host);

    // Swap under the lock, retire outside it. The old object is NOT deleted
    // here — the poll task may be mid-HTTP holding it. See retire().
    SmartOutlet* old;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    old = _outlets[slot];
    _outlets[slot] = o;
    if (slot >= _count) _count = slot + 1;
    xSemaphoreGive(_mutex);
    retire(old);

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
    // This is the call that crashed the board: adopting a stored topology drops
    // the slots the new layout doesn't want, from the main loop, while the poll
    // task is inside provisionPushOutlets() holding one of them.
    SmartOutlet* old;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    old = _outlets[slot];
    _outlets[slot] = nullptr;
    xSemaphoreGive(_mutex);
    retire(old);
}

void SmartOutletControl::clearAllOutlets() {
    // Collected under the lock, retired after it: retire() takes its own mutex,
    // and nesting it inside _mutex would deadlock on a plain binary semaphore.
    SmartOutlet* oldOutlets[SMART_OUTLET_COUNT];
    SmartOutlet* oldCollectors[COLLECTOR_COUNT];
    xSemaphoreTake(_mutex, portMAX_DELAY);
    for (int i = 0; i < SMART_OUTLET_COUNT; i++) {
        oldOutlets[i] = _outlets[i];
        _outlets[i] = nullptr;
    }
    _count = 0;
    for (int i = 0; i < COLLECTOR_COUNT; i++) {
        oldCollectors[i] = _collectors[i];
        _collectors[i] = nullptr;
        _dcOn[i]     = false;
        _dcSynced[i] = false;
    }
    xSemaphoreGive(_mutex);
    for (int i = 0; i < SMART_OUTLET_COUNT; i++) retire(oldOutlets[i]);
    for (int i = 0; i < COLLECTOR_COUNT; i++)    retire(oldCollectors[i]);

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
// Collector plug config (setup API + topology sync)
// -----------------------------------------------------------------------------

void SmartOutletControl::configureCollector(int idx, int generation, const char* ip, const char* host) {
    if (idx < 0 || idx >= COLLECTOR_COUNT) return;
    // Swap the plug object, retire the old one. "Config changes are rare and the
    // poll task tolerates a brief window" is what this used to say, and it is
    // what crashed the board: reconcileCollectors() holds this pointer across a
    // blocking setSwitch(), so "brief" is however long a plug takes to answer.
    SmartOutlet* fresh = new ShellyGen2Outlet(ip, "Dust Collector");  // Gen2+ only
    fresh->setHost(host);
    SmartOutlet* old;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    old = _collectors[idx];
    _collectors[idx] = fresh;
    _dcOn[idx]     = false;
    _dcSynced[idx] = false;   // force a switch command on the next reconcile
    xSemaphoreGive(_mutex);
    retire(old);

    // Only slot 0 persists: it is the one the pre-topology path uses at boot,
    // before any layout has been adopted. The rest are rebuilt from the layout
    // on every adopt, so persisting them would just be a second copy to keep in
    // sync — the same reasoning as the tool slots.
    if (idx == 0) {
        DustCollectorEntry e;
        e.generation = generation;
        strlcpy(e.ip, ip, sizeof(e.ip));
        strlcpy(e.host, host, sizeof(e.host));
        e.valid = true;
        OutletConfig::saveDustCollector(e);
    }

    DEBUG_PRINT(F("[Outlets] Collector ")); Serial.print(idx);
    DEBUG_PRINT(F(" configured: gen")); Serial.print(generation);
    DEBUG_PRINT(F(" @ ")); Serial.println(ip);
}

void SmartOutletControl::removeCollector(int idx) {
    if (idx < 0 || idx >= COLLECTOR_COUNT) return;
    SmartOutlet* old;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    old = _collectors[idx];
    _collectors[idx] = nullptr;
    _dcOn[idx]             = false;
    _dcSynced[idx]         = false;
    _dcManualOverride[idx] = false;
    _dcManualState[idx]    = false;
    xSemaphoreGive(_mutex);
    retire(old);   // never delete here — see retire()
    if (idx == 0) OutletConfig::eraseDustCollector();
    DEBUG_PRINT(F("[Outlets] Collector ")); Serial.print(idx);
    DEBUG_PRINTLN(F(" removed."));
}

bool SmartOutletControl::collectorOn(int idx) {
    if (idx < 0 || idx >= COLLECTOR_COUNT) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = _dcOn[idx];
    xSemaphoreGive(_mutex);
    return v;
}

void SmartOutletControl::setCollectorManual(int idx, bool on) {
    if (idx < 0 || idx >= COLLECTOR_COUNT) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _dcManualOverride[idx] = true;
    _dcManualState[idx]    = on;
    xSemaphoreGive(_mutex);
    DEBUG_PRINT(F("[Outlets] Collector ")); Serial.print(idx);
    DEBUG_PRINT(F(" manual → ")); DEBUG_PRINTLN(on ? F("ON") : F("OFF"));
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

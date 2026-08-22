// =============================================================================
// SmartOutlet.h — Abstract base for any power-monitoring smart outlet
//
// Concrete implementation: ShellyGen2Outlet (Gen2+ / Plus / Pro; Gen1 dropped)
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

    // Reachability check with a caller-chosen timeout, used by the provisioning
    // path where we can afford to wait for a marginal plug (unlike the tight
    // 400ms poll timeout). Default falls back to poll(); ShellyGen2Outlet honors
    // the timeout. Returns true if the outlet answered.
    virtual bool probe(uint32_t /*timeoutMs*/) { return poll(); }

    // Switch the outlet's load on or off. Used for actuator-style outlets
    // (e.g. the dust collector plug) rather than power sensing. Blocking —
    // call only from the poll task. Returns true on success. Power-monitoring
    // usage can ignore this; the base implementation is a no-op.
    virtual bool setSwitch(bool on) { (void)on; return false; }

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

    // -------------------------------------------------------------------------
    // mDNS hostname (without ".local"), if this outlet was discovered/paired
    // via mDNS rather than a hand-entered IP. When set, a failed poll() should
    // trigger a fresh MDNS.queryHost() resolve before giving up, so the outlet
    // keeps working across DHCP lease renewals instead of silently going dark
    // until the user re-runs the wizard. Empty string = no hostname known
    // (manually-entered IP with no mDNS record to fall back on).
    // -------------------------------------------------------------------------
    void setHost(const char* h) { strlcpy(_host, h, sizeof(_host)); }
    const char* host() const { return _host; }

    // -------------------------------------------------------------------------
    // Push support (Gen2 Outbound WebSocket). When a plug maintains an outbound
    // WebSocket to us, its power arrives via setPushedPower() instead of poll().
    // While a push connection is up, the poll task skips the HTTP poll for this
    // outlet entirely (no polling storm); if the connection drops, it falls back
    // to polling. See SmartOutletControl's push handlers + reconcile loop.
    // -------------------------------------------------------------------------
    void setPushedPower(float w) { _lastPowerW = w; _reachable = true; }
    void setPushConnected(bool c) {
        _pushConnected = c;
        if (!c) { _reachable = false; _lastPowerW = 0.0f; } // dropped → treat as offline until re-polled/re-pushed
    }
    bool isPushConnected() const { return _pushConnected; }

    // True once we've successfully pushed Ws/name config to this plug. Lets the
    // poll task provision each plug ONCE rather than re-POSTing every retry tick
    // (a plug that won't connect is a separate problem — re-sending the same
    // config won't fix it). Reset naturally when the outlet object is recreated.
    void setProvisioned(bool p) { _wsProvisioned = p; }
    bool isProvisioned() const  { return _wsProvisioned; }

    // Configure the plug to push to us (Ws.SetConfig) and, optionally, set its
    // friendly name (Switch.SetConfig). Blocking HTTP — poll task only. Base
    // no-ops; ShellyGen2Outlet implements them.
    virtual bool configureOutboundWs(const char* /*wsUrl*/) { return false; }
    virtual bool setName(const char* /*name*/)              { return false; }

    // Let the plug go: point its push target back at `restoreUrl` if we took it
    // from someone, or disable pushing entirely if it was unclaimed when we
    // found it. The inverse of configureOutboundWs(), and the reason
    // previousPushUrl() is stored on every takeover — without this, unpairing
    // leaves the previous owner permanently deaf and the plug still dialling a
    // brain that no longer cares. Blocking HTTP — poll/main task only.
    virtual bool releasePush(const char* /*restoreUrl*/)    { return false; }

    // Read the plug's current push target (Ws.GetConfig) — the ownership
    // authority of RFC §8. Base returns false, meaning "don't know", which
    // callers must treat as "don't touch it".
    virtual bool readPushConfig(String& /*outServer*/, bool& /*outEnabled*/,
                                uint32_t /*timeoutMs*/ = 0) { return false; }

    // POLL-ONLY: this plug belongs to someone else (Home Assistant, another
    // brain), so we read its wattage and never rewrite its Ws config. Set from
    // the claim at provisioning time; keeps the poll task from "helpfully"
    // re-provisioning it on the next pass.
    void setPollOnly(bool p) { _pollOnly = p; }
    bool isPollOnly() const  { return _pollOnly; }

    // TAKEOVER: the user was shown what breaks and said yes (RFC §8). Set only
    // by POST /api/outlets/takeover — never by any automatic path, which is what
    // keeps a background pass from ever stealing a plug.
    //
    // Deliberately NOT persisted: a successful takeover rewrites the plug's own
    // Ws config, so after it lands the plug reads as ours and the approval has
    // nothing left to authorize. If it fails and the board reboots first, the
    // user is asked again — which is the right way for a destructive
    // confirmation to expire.
    void approveTakeover()        { _takeoverApproved = true; _pollOnly = false; }
    bool takeoverApproved() const { return _takeoverApproved; }

    // Who this plug pushed to before we took it, so unpairing can hand it back
    // rather than leaving the other controller permanently deaf.
    void setPreviousPushUrl(const char* url) { strlcpy(_prevPushUrl, url ? url : "", sizeof(_prevPushUrl)); }
    const char* previousPushUrl() const      { return _prevPushUrl; }

protected:
    float _lastPowerW   = 0.0f;
    float _thresholdW   = 5.0f;
    int   _stopIndex    = 0;
    bool  _reachable    = false;
    bool  _pushConnected = false;
    bool  _wsProvisioned = false;
    bool  _pollOnly      = false;
    bool  _takeoverApproved = false;
    char  _prevPushUrl[64]  = "";
    char  _host[40]     = "";
};

// =============================================================================
// AgentConfig.h — Anthropic API key storage + WiFi credential reset
//
// IMPORTANT: this header intentionally does NOT include <WebServer.h>.
// It is safe to include alongside <ESPAsyncWebServer.h>.
// WiFiProvisioner.h (which includes <WebServer.h> for the captive portal) must
// NOT be included in the same translation unit as <ESPAsyncWebServer.h> because
// their HTTP method enum/macro definitions conflict.
//
// WiFiProvisioner.h itself includes this header so callers of WiFiProvisioner
// that do NOT also use ESPAsyncWebServer get all functions in one include.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "../config.h"

namespace WiFiProvisioner {

// NVS namespaces / keys shared between WiFiProvisioner.h and AgentConfig.h.
// Defined here (the smaller, always-safe include) and re-used via include.
static const char* const NVS_NS        = "wifi_creds";
static const char* const NVS_SSID      = "ssid";
static const char* const NVS_PASS      = "pass";
static const char* const NVS_ANT_NS    = "agent_cfg";
static const char* const NVS_ANT_KEY   = "claude_key";

// ---------------------------------------------------------------------------
// getAnthropicKey() — returns the stored Anthropic API key (empty if not set).
// ---------------------------------------------------------------------------
inline String getAnthropicKey() {
    Preferences prefs;
    prefs.begin(NVS_ANT_NS, /*readOnly=*/true);
    String key = prefs.getString(NVS_ANT_KEY, "");
    prefs.end();
    return key;
}

// ---------------------------------------------------------------------------
// setAnthropicKey() — persist a new key without re-running the portal.
// ---------------------------------------------------------------------------
inline void setAnthropicKey(const String& key) {
    Preferences prefs;
    prefs.begin(NVS_ANT_NS, /*readOnly=*/false);
    prefs.putString(NVS_ANT_KEY, key);
    prefs.end();
    DEBUG_PRINTLN(F("[WiFi] Anthropic API key updated."));
}

// ---------------------------------------------------------------------------
// reset() — erase stored WiFi credentials and reboot into the portal.
// ---------------------------------------------------------------------------
inline void reset() {
    Preferences prefs;
    prefs.begin(NVS_NS, /*readOnly=*/false);
    prefs.clear();
    prefs.end();
    DEBUG_PRINTLN(F("[WiFi] Credentials erased. Rebooting into setup portal..."));
    delay(1000);
    ESP.restart();
}

} // namespace WiFiProvisioner

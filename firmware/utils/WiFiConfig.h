// =============================================================================
// WiFiConfig.h — WiFi credential + hostname storage in NVS
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
#include <ArduinoJson.h>
#include "../config.h"

namespace WiFiProvisioner {

// NVS namespaces / keys shared with WiFiProvisioner.h. Defined here (the
// smaller, always-safe include) and re-used via include.
static const char* const NVS_NS        = "wifi_creds";
static const char* const NVS_SSID      = "ssid";
static const char* const NVS_PASS      = "pass";
static const char* const NVS_HOST      = "host";

// Default mDNS hostname (device reachable at http://<hostname>.local) when
// none has been provisioned.
static const char* const DEFAULT_HOSTNAME = "dustgate";

// ---------------------------------------------------------------------------
// getHostname() — returns the stored mDNS hostname, or DEFAULT_HOSTNAME.
// ---------------------------------------------------------------------------
inline String getHostname() {
    Preferences prefs;
    prefs.begin(NVS_NS, /*readOnly=*/true);
    String host = prefs.getString(NVS_HOST, DEFAULT_HOSTNAME);
    prefs.end();
    return host.length() > 0 ? host : String(DEFAULT_HOSTNAME);
}

// ---------------------------------------------------------------------------
// applyProvisionJson() — parses {"ssid","pass","host"} and writes
// whichever fields are present to NVS. Returns true if WiFi credentials were
// written (caller should reboot to apply them).
//
// Shared by the serial "provision" command (SerialDebugControl.cpp) AND the
// captive portal's own serial listener (WiFiProvisioner.h) — the portal's
// while(true) loop blocks forever servicing HTTP only, so without its own
// listener here, a "provision ..." command sent over serial while the device
// is sitting in the portal (e.g. right after a fresh erase, before any WiFi
// has ever been configured) would be silently ignored.
// ---------------------------------------------------------------------------
inline bool applyProvisionJson(const String& json, String* errOut = nullptr) {
    StaticJsonDocument<384> doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        if (errOut) *errOut = err.c_str();
        return false;
    }
    const char* ssid = doc["ssid"] | "";
    const char* pass = doc["pass"] | "";
    const char* host = doc["host"] | "";

    bool wifiSet = false;
    if (strlen(ssid) > 0) {
        Preferences prefs;
        prefs.begin(NVS_NS, /*readOnly=*/false);
        prefs.putString(NVS_SSID, ssid);
        prefs.putString(NVS_PASS, pass);
        prefs.end();
        wifiSet = true;
    }
    if (strlen(host) > 0) {
        Preferences prefs;
        prefs.begin(NVS_NS, /*readOnly=*/false);
        prefs.putString(NVS_HOST, host);
        prefs.end();
    }
    return wifiSet;
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

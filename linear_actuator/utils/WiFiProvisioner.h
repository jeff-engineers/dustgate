// =============================================================================
// WiFiProvisioner.h — Captive-portal WiFi provisioning for end users
//
// On first boot (no stored credentials), the ESP32 starts an access point
// named WIFI_PORTAL_SSID (config.h). The user connects to it and visits the
// portal page to enter their home WiFi credentials. Credentials are saved to
// NVS (Preferences) and the device reboots into station mode.
//
// On subsequent boots the stored credentials are used directly; the portal
// only appears again if the connection fails or 'wifireset' is issued.
//
// If WIFI_STA_SSID is hardcoded in config.h, that takes priority over NVS
// credentials and the portal is bypassed entirely (developer / fixed-network
// deployments).
//
// No external libraries required — uses ESP32-core WebServer + Preferences.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "../config.h"
// AgentConfig.h provides NVS constants, getAnthropicKey(), setAnthropicKey(),
// and reset() without including <WebServer.h> — safe alongside ESPAsyncWebServer.
#include "AgentConfig.h"

namespace WiFiProvisioner {

// ---------------------------------------------------------------------------
// Minimal portal HTML — served from the setup AP
// ---------------------------------------------------------------------------
static const char PORTAL_HTML[] PROGMEM = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DustGate — WiFi Setup</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:system-ui,sans-serif;background:#f4f4f4;display:flex;
       align-items:flex-start;justify-content:center;min-height:100vh;padding:32px 16px}
  .card{background:#fff;border-radius:12px;padding:28px 24px;width:100%;max-width:380px}
  h1{font-size:20px;color:#1c1c1c;margin-bottom:6px}
  p{font-size:14px;color:#666;margin-bottom:22px;line-height:1.5}
  label{display:block;font-size:13px;color:#555;margin-bottom:5px;font-weight:600}
  input{width:100%;padding:11px 12px;border:1px solid #ddd;border-radius:7px;
        font-size:15px;margin-bottom:16px;outline:none}
  input:focus{border-color:#1c1c1c}
  button{width:100%;padding:13px;background:#1c1c1c;color:#fff;border:none;
         border-radius:7px;font-size:15px;font-weight:600;cursor:pointer}
  .hint{font-size:12px;color:#aaa;margin-top:16px;text-align:center}
</style>
</head>
<body>
<div class="card">
  <h1>DustGate WiFi Setup</h1>
  <p>Connect this device to your home network so it can communicate with your Shelly outlets.</p>
  <form method="POST" action="/save">
    <label>Network name (SSID)</label>
    <input name="ssid" placeholder="Your WiFi network" autocomplete="off" required>
    <label>Password</label>
    <input name="pass" type="password" placeholder="Leave blank if open" autocomplete="off">
    <hr style="border:none;border-top:1px solid #eee;margin:8px 0 16px">
    <label>Anthropic API Key <span style="font-weight:400;color:#aaa">(optional — enables AI setup assistant)</span></label>
    <input name="claude_key" type="password" placeholder="sk-ant-..." autocomplete="off"
           pattern="sk-ant-[A-Za-z0-9\-_]+" title="Must start with sk-ant-">
    <button type="submit">Save &amp; Connect</button>
  </form>
  <p class="hint">The device reboots automatically after saving.<br>Reconnect to your normal network afterward.</p>
</div>
</body>
</html>
)html";

static const char SAVED_HTML[] PROGMEM = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Saved</title>
<style>
  body{font-family:system-ui,sans-serif;display:flex;align-items:center;
       justify-content:center;min-height:100vh;background:#f4f4f4}
  .card{background:#fff;border-radius:12px;padding:32px 24px;text-align:center;max-width:320px}
  .check{font-size:48px;margin-bottom:16px}
  h2{font-size:18px;color:#1c1c1c;margin-bottom:8px}
  p{font-size:14px;color:#666;line-height:1.5}
</style>
</head>
<body>
<div class="card">
  <div class="check">&#10003;</div>
  <h2>Credentials saved</h2>
  <p>The device is reconnecting to your network. You can close this page and reconnect your phone to your normal WiFi.</p>
</div>
</body>
</html>
)html";

// ---------------------------------------------------------------------------
// Internal: start setup AP and block until the user submits credentials.
// Saves to NVS and reboots — never returns.
// ---------------------------------------------------------------------------
inline void _runPortal() {
    DEBUG_PRINT(F("[WiFi] Starting setup portal — connect to: "));
    DEBUG_PRINTLN(F(WIFI_PORTAL_SSID));

    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_PORTAL_SSID);

    DEBUG_PRINT(F("[WiFi] Portal at http://"));
    Serial.println(WiFi.softAPIP().toString());

    WebServer server(80);

    // Redirect any unknown path to the root form
    auto redirectToRoot = [&]() {
        server.sendHeader("Location", "/");
        server.send(302);
    };

    server.on("/", HTTP_GET, [&]() {
        server.send_P(200, "text/html", PORTAL_HTML);
    });

    server.on("/save", HTTP_POST, [&]() {
        String ssid      = server.arg("ssid");
        String pass      = server.arg("pass");
        String claudeKey = server.arg("claude_key");

        server.send_P(200, "text/html", SAVED_HTML);
        delay(500); // let browser receive the page before we reboot

        Preferences prefs;
        prefs.begin(NVS_NS, /*readOnly=*/false);
        prefs.putString(NVS_SSID, ssid);
        prefs.putString(NVS_PASS, pass);
        prefs.end();

        // Anthropic key is stored in a separate NVS namespace so it survives wifireset
        if (claudeKey.length() > 0) {
            Preferences agentPrefs;
            agentPrefs.begin(NVS_ANT_NS, /*readOnly=*/false);
            agentPrefs.putString(NVS_ANT_KEY, claudeKey);
            agentPrefs.end();
            DEBUG_PRINTLN(F("[WiFi] Anthropic API key saved."));
        }

        DEBUG_PRINT(F("[WiFi] Credentials saved for SSID: "));
        Serial.println(ssid);
        delay(1000);
        ESP.restart();
    });

    server.onNotFound(redirectToRoot);
    server.begin();

    // Block here — device reboots when credentials are submitted
    while (true) {
        server.handleClient();
        delay(2);
    }
}

// ---------------------------------------------------------------------------
// begin() — call once from setup() before any WiFi-dependent code.
//
// Priority order:
//   1. Hardcoded WIFI_STA_SSID in config.h  (developer / fixed network)
//   2. NVS-stored credentials               (provisioned by end user)
//   3. Captive portal                        (no credentials found)
//
// Blocks until connected. On failure, launches the portal and never returns
// (portal reboots the device after saving credentials).
// ---------------------------------------------------------------------------
inline bool begin() {
#ifdef WIFI_STA_SSID
    // Developer mode: hardcoded credentials take priority over NVS
    DEBUG_PRINT(F("[WiFi] Connecting to ")); DEBUG_PRINTLN(F(WIFI_STA_SSID));
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
#else
    // End-user mode: load credentials from NVS
    Preferences prefs;
    prefs.begin(NVS_NS, /*readOnly=*/true);
    String ssid = prefs.getString(NVS_SSID, "");
    String pass = prefs.getString(NVS_PASS, "");
    prefs.end();

    if (ssid.length() == 0) {
        DEBUG_PRINTLN(F("[WiFi] No credentials stored."));
        _runPortal(); // never returns
    }

    DEBUG_PRINT(F("[WiFi] Connecting to ")); Serial.println(ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
#endif

    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 12000UL) {
        delay(250);
        DEBUG_PRINT(F("."));
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        DEBUG_PRINTLN(F("[WiFi] Connection failed — launching setup portal."));
        _runPortal(); // never returns
    }

    DEBUG_PRINT(F("[WiFi] Connected. IP: "));
    Serial.println(WiFi.localIP().toString());
    DEBUG_PRINT(F("[WiFi] Web UI:       http://"));
    Serial.println(WiFi.localIP().toString());
    DEBUG_PRINTLN(F("[WiFi] Setup assistant available at  http://<IP>/#/setup"));
    return true;
}

// reset(), getAnthropicKey(), setAnthropicKey() are defined in AgentConfig.h
// (included above). They live there so HttpApiServer.cpp can include them
// without also pulling in <WebServer.h>.

} // namespace WiFiProvisioner

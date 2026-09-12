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
#include <ESPmDNS.h>
#include "../config.h"
// WiFiConfig.h provides the NVS constants, getHostname() and reset() without
// including <WebServer.h> — safe alongside ESPAsyncWebServer.
#include "WiFiConfig.h"

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

// Optional heartbeat for the portal's blocking loop. _runPortal() never returns,
// so a caller's own loop() is unreachable the entire time the portal is up —
// which on a headless board means its status indicator would freeze on whatever
// colour it had at boot, exactly when it most needs to say "come configure me".
// Set this before begin() to get a tick; leave it null (the primary does) and
// nothing changes. Called frequently, so keep the callback cheap and non-blocking.
inline void (*&onPortalTick())() { static void (*cb)() = nullptr; return cb; }
inline void setPortalTick(void (*cb)()) { onPortalTick() = cb; }

// ---------------------------------------------------------------------------
// Internal: start setup AP and block until the user submits credentials.
// Saves to NVS and reboots — never returns.
// ---------------------------------------------------------------------------
inline void _runPortal() {
    DEBUG_PRINT(F("[WiFi] Starting setup portal — connect to: "));
    DEBUG_PRINTLN(F(WIFI_PORTAL_SSID));

    // EXPECT TWO ERROR LINES HERE, AND IGNORE THEM (documented 2026-09-11).
    //
    //   [E][STA.cpp:540] disconnect(): STA disconnect failed! 0xffffffff: ESP_FAIL
    //   [E][STA.cpp:346] connect(): STA config failed
    //
    // They come from the Arduino core, not from us, and they are printed at
    // ERROR level for something that is not one. We arrive here only after a
    // station connect has already FAILED, so the STA was never associated —
    // and mode(WIFI_AP) tears it down anyway. Disconnecting a station that
    // never connected returns ESP_FAIL, and reconfiguring one that is being
    // torn down fails for the same reason.
    //
    // THE PROOF THAT IT IS COSMETIC is the line below: if the portal prints its
    // address, the AP came up. Chasing these costs an evening and changes
    // nothing — which is the only reason this comment is worth its length.
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
        String ssid = server.arg("ssid");
        String pass = server.arg("pass");

        server.send_P(200, "text/html", SAVED_HTML);
        delay(500); // let browser receive the page before we reboot

        Preferences prefs;
        prefs.begin(NVS_NS, /*readOnly=*/false);
        prefs.putString(NVS_SSID, ssid);
        prefs.putString(NVS_PASS, pass);
        prefs.end();

        DEBUG_PRINT(F("[WiFi] Credentials saved for SSID: "));
        Serial.println(ssid);
        delay(1000);
        ESP.restart();
    });

    server.onNotFound(redirectToRoot);
    server.begin();

    // Block here — device reboots when credentials are submitted, either via
    // the web form above or a "provision {...}" command sent over serial.
    // Without this serial listener, a device sitting here (e.g. right after
    // a fresh erase, before WiFi has ever been configured) could never be
    // reached by the "provision" command at all — this loop never returns
    // control to the main setup()/loop(), where SerialDebugControl normally
    // handles it.
    String serialLine;
    while (true) {
        server.handleClient();
        if (onPortalTick()) onPortalTick();

        while (Serial.available()) {
            char c = Serial.read();
            if (c == '\n') {
                serialLine.trim();
                if (serialLine.startsWith("provision ")) {
                    String json = serialLine.substring(10);
                    json.trim();
                    String errMsg;
                    bool wifiSet = applyProvisionJson(json, &errMsg);
                    if (errMsg.length() > 0) {
                        Serial.print(F("[PROVISION] JSON parse error: "));
                        Serial.println(errMsg);
                    } else if (wifiSet) {
                        Serial.println(F("OK provision"));
                        Serial.println(F("[PROVISION] WiFi saved — rebooting to connect..."));
                        delay(300);
                        ESP.restart();
                    } else {
                        Serial.println(F("OK provision"));
                    }
                }
                serialLine = "";
            } else if (c == 0x08 || c == 0x7F) {          // backspace / delete
                if (serialLine.length() > 0) serialLine.remove(serialLine.length() - 1);
            } else if (c >= 0x20 && c <= 0x7E) {           // printable ASCII (covers JSON)
                if (serialLine.length() < 512) serialLine += c; // guard runaway buffer
            }
            // CR and other control bytes (tab, ESC/arrow-key sequences) are ignored.
        }

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
    // Recover the link unattended: keep credentials in the driver's NVS and let
    // the core auto-reconnect on a dropped connection (maintain() below is the
    // backstop for a full AP outage). Set before WiFi.begin() so persistence
    // takes effect for this session's connection.
    WiFi.persistent(true);
    WiFi.setAutoReconnect(true);

    // Modem sleep OFF. This is a correctness fix, not a tuning knob.
    //
    // The ESP32 defaults to WIFI_PS_MIN_MODEM: the radio dozes between DTIM
    // beacons and wakes for unicast addressed to it. MULTICAST is what gets
    // dropped — and mDNS is entirely multicast. Symptoms this caused on the
    // bench, all of which looked like different bugs:
    //
    //   • the primary's mDNS query for a node failing at boot, succeeding a
    //     minute later, with the node advertising perfectly the whole time
    //   • GET /api/nodes/discover returning [] while a laptop on the same
    //     LAN saw both boards instantly
    //   • ~220ms ping times to a board two metres away
    //
    // Both boards are mains-powered and one of them is holding a WebSocket open
    // for the whole session, so there is nothing to save here anyway.
    WiFi.setSleep(false);
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

    // 30s, RAISED FROM 12s ON 2026-09-11 — a brand-new C5 could not join a
    // guest network inside 12, with correct credentials, while every previously
    // provisioned board on the same network was fine.
    //
    // The C5 is DUAL BAND. It scans 2.4 GHz *and* 5 GHz before choosing a BSS,
    // where the single-band parts this number was first chosen against scanned
    // one. Add a busy guest AP and a WPA2 handshake and 12s is simply tight —
    // it was never a deadline anything needed, just a number.
    //
    // The cost of being wrong in each direction is lopsided, which is the real
    // argument: too long and a board with genuinely bad credentials takes half a
    // minute to offer its portal. Too short and a board with GOOD credentials
    // gives up and demands the user set up WiFi that is already correct — which
    // is worse, because the portal is a dead end when nothing is wrong.
    static const unsigned long kConnectTimeoutMs = 30000UL;

    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < kConnectTimeoutMs) {
        delay(250);
        DEBUG_PRINT(F("."));
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        // SAY WHY, not just that. A row of dots and "failed" is the same output
        // for a wrong password, a network that is not there, and one that is
        // simply slow — and those want three different responses from whoever
        // is standing at the bench. wl_status_t already knows which.
        const wl_status_t st = (wl_status_t)WiFi.status();
        DEBUG_PRINT(F("[WiFi] Connection failed after "));
        DEBUG_PRINT((millis() - t) / 1000);
        DEBUG_PRINT(F("s — status "));
        DEBUG_PRINT((int)st);
        switch (st) {
            case WL_NO_SSID_AVAIL:
                DEBUG_PRINTLN(F(" (NO SSID) — that network was not seen at all."));
                DEBUG_PRINTLN(F("       Check the name, and that it is on a band this"));
                DEBUG_PRINTLN(F("       board can reach. Hidden SSIDs also land here."));
                break;
            case WL_CONNECT_FAILED:
                DEBUG_PRINTLN(F(" (AUTH FAILED) — the network is there and rejected us."));
                DEBUG_PRINTLN(F("       That is almost always the password."));
                break;
            case WL_DISCONNECTED:
                DEBUG_PRINTLN(F(" (STILL TRYING) — found it, never finished associating."));
                DEBUG_PRINTLN(F("       A slow or busy AP. Raising kConnectTimeoutMs is"));
                DEBUG_PRINTLN(F("       the honest fix if this is repeatable."));
                break;
            default:
                DEBUG_PRINTLN(F(" — see wl_status_t in WiFiType.h."));
                break;
        }
        DEBUG_PRINTLN(F("[WiFi] Launching setup portal."));
        _runPortal(); // never returns
    }

    // ms, not a formatted float: DEBUG_PRINT is a single-argument macro, so the
    // two-argument Serial.print(x, digits) form does not survive it.
    DEBUG_PRINT(F("[WiFi] Connected in "));
    DEBUG_PRINT(millis() - t);
    DEBUG_PRINT(F("ms. IP: "));
    Serial.print(WiFi.localIP().toString());
    // RSSI on every boot, because "it connects but drops later" and "it barely
    // connected at all" look identical once it is up — and a collector board
    // lives at the far end of a shop from the router.
    DEBUG_PRINT(F("  RSSI "));
    DEBUG_PRINT(WiFi.RSSI());
    DEBUG_PRINTLN(F(" dBm"));

    String hostname = getHostname();
    if (MDNS.begin(hostname.c_str())) {
        MDNS.addService("http", "tcp", 80);
        // Also advertise on the DustGate service so a primary's node picker can
        // enumerate the shop. role=primary is what keeps this board OUT of its
        // own (and a neighbouring system's) list of actuator targets — the
        // picker only offers role=secondary.
        //
        // NOT on a secondary. This provisioner is shared with the node program,
        // which registers the SAME service with role=secondary right after this
        // runs (node/dustgate_node.cpp) — and mDNS refuses the duplicate, so the
        // first registration wins and a node advertises itself as a PRIMARY.
        // Caught on the C5 bench boot:
        //     mdns_service_add_for_host_base(808): Service already exists
        //     [E][ESPmDNS.cpp:191] addService(): Failed adding service
        // The node then answers on the network, looks healthy, and never appears
        // in the picker it is supposed to appear in — an invisible node, not a
        // broken one.
#ifndef DUSTGATE_SECONDARY
        MDNS.addService("dustgate", "tcp", 80);
        MDNS.addServiceTxt("dustgate", "tcp", "role",  "primary");
        MDNS.addServiceTxt("dustgate", "tcp", "board", BOARD_NAME);
#endif
        DEBUG_PRINT(F("[WiFi] mDNS hostname: "));
        Serial.print(hostname);
        Serial.println(F(".local"));
    } else {
        DEBUG_PRINTLN(F("[WiFi] mDNS failed to start — use the IP address below instead."));
    }

    DEBUG_PRINT(F("[WiFi] Web UI:       http://"));
    Serial.print(hostname);
    Serial.print(F(".local  (or http://"));
    Serial.print(WiFi.localIP().toString());
    Serial.println(F(")"));
    DEBUG_PRINTLN(F("[WiFi] Setup assistant available at  http://<host-or-ip>/#/setup"));
    return true;
}

// ---------------------------------------------------------------------------
// maintain() — call periodically from loop() to keep WiFi alive unattended.
//
// setAutoReconnect() (set in begin()) handles most transient drops, but a full
// AP outage can leave the radio disconnected indefinitely with no further
// retries. This nudges an explicit WiFi.reconnect() every
// WIFI_RECONNECT_INTERVAL_MS while down, using the persisted credentials, so a
// shop device rejoins on its own after the router comes back instead of needing
// a power cycle. Cheap and non-blocking: it only acts while disconnected.
// ---------------------------------------------------------------------------
inline void maintain() {
    static unsigned long lastAttempt = 0;
    if (WiFi.status() == WL_CONNECTED) return;
    unsigned long now = millis();
    if (now - lastAttempt < WIFI_RECONNECT_INTERVAL_MS) return;
    lastAttempt = now;
    DEBUG_PRINTLN(F("[WiFi] Disconnected — attempting reconnect..."));
    WiFi.reconnect();
}

// getHostname() and reset() are defined in WiFiConfig.h (included above). They
// live there so HttpApiServer.cpp can include them without also pulling in
// <WebServer.h>.

} // namespace WiFiProvisioner

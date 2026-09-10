// =============================================================================
// SerialDebugControl.cpp
// =============================================================================

#include "SerialDebugControl.h"
#include <Wire.h>                  // the `i2c` bring-up scan
#include "../utils/MotionMath.h"
#include "../utils/WiFiConfig.h"   // NVS constants + applyProvisionJson() — safe to include always
#if defined(CONTROL_SMART_OUTLET) || defined(ENABLE_HTTP_API)
  #include "../utils/WiFiProvisioner.h"
  #include "../utils/MdnsQuery.h"   // `mdnsprobe` — wanted on any board that queries
  #include "../outlets/TasmotaOutlet.h"  // `sweep` — the plugs that advertise nothing
  #include "../outlets/PlugClaim.h"     // ...and who owns one when we find it
  #include "../utils/Watchdog.h"    // the probe outlives a loop() iteration
#endif
#ifdef CONTROL_SMART_OUTLET
  #include "../outlets/ShellyGen2Outlet.h"
  #include "../outlets/ShellyDeviceName.h"
  #include "SmartOutletControl.h"    // `plugtrace` — outlettrace::enabled()
#endif

#if defined(CONTROL_SERIAL_DEBUG) || defined(ENABLE_SERIAL_COMMANDS)

SerialDebugControl::SerialDebugControl()
    : _requestedStop(0),
      _eStopPending(false),
      _homePending(false), _resetPending(false),
      _clearCalPending(false),
      _gconfPending(false),
      _jogPending(false),
      _jogMM(0.0f),
      _calPending(false),
      _calGates(0),
      _homeSidePending(false),
      _homedLeftValue(false),
      _servoPending(false),
      _servoIndex(0),
      _servoAngle(0),
      _servoDetach(false)
{ _calModel[0] = '\0'; }

bool SerialDebugControl::begin() {
    // Serial already started in setup() via Serial.begin(SERIAL_BAUD)
    printHelp();
    Serial.println(F("[DEBUG] System ready. Type 'home' to home."));
    return true;
}

int SerialDebugControl::readRequestedStop() {
    // Drain Serial into line buffer; process on newline. Handles line editing so
    // pasted/typed control bytes don't corrupt the command:
    //   - CR/LF        → end of line, process
    //   - BS (0x08) / DEL (0x7F) → erase last char
    //   - printable ASCII (0x20–0x7E) → append (bounded)
    //   - anything else (tab, ESC/arrow-key sequences, other control bytes) → ignore
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            _inputBuffer.trim();
            if (_inputBuffer.length() > 0) {
                processLine(_inputBuffer);
                _inputBuffer = "";
            }
        } else if (c == 0x08 || c == 0x7F) {
            if (_inputBuffer.length() > 0) _inputBuffer.remove(_inputBuffer.length() - 1);
        } else if (c >= 0x20 && c <= 0x7E) {
            if (_inputBuffer.length() < 128) _inputBuffer += c; // guard runaway buffer
        }
    }
    return _requestedStop;
}

bool SerialDebugControl::isEnabled() {
    // No enable/disable concept — the system always runs; only e-stop halts it.
    return true;
}

bool SerialDebugControl::consumeEStop() {
    if (_eStopPending) {
        _eStopPending = false;
        return true;
    }
    return false;
}

bool SerialDebugControl::consumePressRequest() {
    bool v = _pressRequest;
    _pressRequest = false;
    return v;
}

bool SerialDebugControl::consumeHomeRequest() {
    if (_homePending) {
        _homePending = false;
        return true;
    }
    return false;
}

bool SerialDebugControl::consumeCalibrateRequest(char* outModel, size_t modelLen, int& outGates) {
    if (_calPending) {
        _calPending = false;
        strlcpy(outModel, _calModel, modelLen);
        outGates = _calGates;
        return true;
    }
    return false;
}

bool SerialDebugControl::consumeHomeSideRequest(bool& outHomedLeft) {
    if (_homeSidePending) {
        _homeSidePending = false;
        outHomedLeft = _homedLeftValue;
        return true;
    }
    return false;
}

bool SerialDebugControl::consumeServoRequest(int& outIndex, int& outAngle, bool& outDetach) {
    if (_servoPending) {
        _servoPending = false;
        outIndex  = _servoIndex;
        outAngle  = _servoAngle;
        outDetach = _servoDetach;
        return true;
    }
    return false;
}

bool SerialDebugControl::consumeClearCalRequest() {
    if (_clearCalPending) {
        _clearCalPending = false;
        return true;
    }
    return false;
}

bool SerialDebugControl::consumeResetRequest() {
    if (!_resetPending) return false;
    _resetPending = false;
    return true;
}

bool SerialDebugControl::consumeGconfRequest() {
    if (_gconfPending) {
        _gconfPending = false;
        return true;
    }
    return false;
}

bool SerialDebugControl::consumeJogRequest(float& outMM) {
    if (_jogPending) {
        _jogPending = false;
        outMM = _jogMM;
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
void SerialDebugControl::processLine(const String& line) {
    Serial.print(F("> "));
    Serial.println(line);

    // Numeric: position selection
    if (line.length() == 1 && isDigit(line[0])) {
        int pos = line[0] - '0';
        if (pos >= 0 && pos <= NUM_STOPS) {
            _requestedStop = pos;
            Serial.print(F("[DEBUG] Position set to: "));
            Serial.println(pos);
        } else {
            Serial.print(F("[DEBUG] Invalid position. Enter 0-"));
            Serial.println(NUM_STOPS);
        }
        return;
    }

    // Text commands (case-insensitive)
    String cmd = line;
    cmd.toLowerCase();

    if (cmd == "estop" || cmd == "stop") {
        _eStopPending = true;
        Serial.println(F("[DEBUG] E-STOP — motion halted. Type 'home' to recover."));

    } else if (cmd == "home") {
        _eStopPending = false;     // clear any latched estop
        _homePending = true;
        Serial.println(F("[DEBUG] Homing requested."));

    } else if (cmd == "reset" || cmd == "retry") {
        // Not a reboot. It re-attempts the things that are latched for the life
        // of a boot — the drive coming up, and the fault flags that came with
        // it — so a servo plugged in after the board started can be picked up
        // without power-cycling the board and losing the log.
        _resetPending = true;
        _eStopPending = false;
        Serial.println(F("[DEBUG] Reset requested — retrying the drive and clearing"));
        Serial.println(F("        latched boot faults. This does NOT reboot the board."));

    } else if (cmd == "gconf") {
        _gconfPending = true;

    } else if (cmd.startsWith("jog ")) {
        float mm = cmd.substring(4).toFloat();
        String jogArg = cmd.substring(4);
        jogArg.trim();
        if (mm == 0.0f && jogArg != "0") {
            Serial.println(F("[JOG] Usage: jog <mm>  e.g. 'jog 10' or 'jog -5'"));
        } else {
            _jogMM = mm;
            _jogPending = true;
            Serial.print(F("[JOG] "));
            Serial.print(mm, 1);
            Serial.println(mm < 0 ? F(" mm toward home") : F(" mm away from home"));
        }

    } else if (cmd.startsWith("calibrate ")) {
        // calibrate <model> <gates>   e.g. 'calibrate rockler-2.5 2'
        String rest = cmd.substring(10); rest.trim();
        int sp = rest.indexOf(' ');
        if (sp < 0) {
            Serial.println(F("[CAL] Usage: calibrate <model> <gates>  e.g. 'calibrate rockler-2.5 2'  (models: rockler-2.5, rockler-4, custom)"));
        } else {
            String model = rest.substring(0, sp); model.trim();
            int gates = rest.substring(sp + 1).toInt();
            if (gates < 1 || gates > NUM_STOPS) {
                Serial.println(F("[CAL] gates must be 1..NUM_STOPS"));
            } else {
                strlcpy(_calModel, model.c_str(), sizeof(_calModel));
                _calGates   = gates;
                _calPending = true;
                Serial.print(F("[CAL] Requested reference sweep: "));
                Serial.print(model); Serial.print(F(" x")); Serial.println(gates);
            }
        }

    } else if (cmd.startsWith("servo ")) {
        // servo <1-4> <angle>   → move servo N to angle°  (Servo bring-up)
        // servo <1-4> detach    → de-energize servo N (ball holds by friction/detent)
        String rest = cmd.substring(6); rest.trim();
        int sp = rest.indexOf(' ');
        if (sp < 0) {
            Serial.println(F("[SERVO] Usage: servo <1-4> <0-180>  |  servo <1-4> detach"));
        } else {
            int idx = rest.substring(0, sp).toInt();
            String arg = rest.substring(sp + 1); arg.trim();
            if (idx < 1 || idx > 4) {
                Serial.println(F("[SERVO] index must be 1..4 (pins 25/26/27/14)"));
            } else if (arg == "detach") {
                _servoIndex = idx; _servoDetach = true; _servoPending = true;
                Serial.print(F("[SERVO] Detach servo ")); Serial.println(idx);
            } else {
                int angle = arg.toInt();
                if (angle < 0 || angle > 180) {
                    Serial.println(F("[SERVO] angle must be 0..180"));
                } else {
                    _servoIndex = idx; _servoAngle = angle; _servoDetach = false; _servoPending = true;
                    Serial.print(F("[SERVO] Servo ")); Serial.print(idx);
                    Serial.print(F(" → ")); Serial.print(angle); Serial.println(F("°"));
                }
            }
        }

    } else if (cmd == "homeside" || cmd.startsWith("homeside ")) {
        // homeside left|right → report which side the carriage homed to. 'right'
        // makes the firmware switch the datum to the other endstop and re-home left.
        String arg = cmd.length() > 8 ? cmd.substring(9) : String();
        arg.trim();
        if (arg != "left" && arg != "right") {
            Serial.println(F("[CFG] Usage: homeside left|right   (report which side it just homed to; 'right' re-homes to the left endstop)"));
        } else {
            _homedLeftValue  = (arg == "left");
            _homeSidePending = true;
            Serial.print(F("[CFG] Reported home side: "));
            Serial.println(arg);
        }

    } else if (cmd.startsWith("provision ")) {
        // provision {"ssid":"...","pass":"...","host":"..."}
        // Writes WiFi credentials + hostname directly to NVS via the shared
        // helper in WiFiConfig.h (also used by the captive portal's own serial
        // listener). Doesn't reboot — this path only runs once WiFi is
        // already connected, so changes apply on the next boot.
        // Use original 'line' (not lowercased 'cmd') to preserve credential case.
        String json = line.substring(10);
        json.trim();
        String errMsg;
        bool wifiSet = WiFiProvisioner::applyProvisionJson(json, &errMsg);
        if (errMsg.length() > 0) {
            Serial.print(F("[PROVISION] JSON parse error: "));
            Serial.println(errMsg);
            Serial.println(F("[PROVISION] Usage: provision {\"ssid\":\"MyNet\",\"pass\":\"pw\",\"host\":\"dustgate\"}"));
            return;
        }
        if (wifiSet) {
            Serial.println(F("[PROVISION] WiFi credentials saved."));
        }
        Serial.println(F("OK provision"));

    } else if (cmd == "wifireset") {
#if defined(CONTROL_SMART_OUTLET)
        Serial.println(F("[WiFi] Erasing stored credentials and rebooting into setup portal..."));
        delay(200);
        WiFiProvisioner::reset(); // does not return
#else
        Serial.println(F("[WiFi] WiFi not enabled in this build."));
#endif

    } else if (cmd == "clearcal") {
        _clearCalPending = true;
        Serial.println(F("[DEBUG] Calibration erase requested — config.h defaults will be used."));

#ifdef CONTROL_SMART_OUTLET
    } else if (cmd == "discover") {
        runDiscover();
    } else if (cmd.startsWith("probe ")) {
        runProbe(cmd.substring(6));
    } else if (cmd == "sweep" || cmd.startsWith("sweep ")) {
        int from = 1, to = 254;
        if (cmd.length() > 6) sscanf(cmd.c_str() + 6, "%d %d", &from, &to);
        runSweep(from, to);
#endif

#if defined(CONTROL_SMART_OUTLET) || defined(ENABLE_HTTP_API)
    } else if (cmd == "press") {
        // Fires on the main loop, where the transmitter lives — this only asks.
        // Same shape as consumeHomeRequest(): a debug command never touches
        // hardware from the serial task.
        _pressRequest = true;
        Serial.println(F("[RF] press queued — watch the receiver."));
    } else if (cmd == "mdnsprobe") {
        runMdnsProbe();
#endif

#ifdef CONTROL_SMART_OUTLET
    } else if (cmd == "plugtrace") {
        outlettrace::enabled() = !outlettrace::enabled();
        Serial.print(F("[DEBUG] Plug push tracing "));
        Serial.println(outlettrace::enabled()
            ? F("ON — every frame a plug sends, timestamped. Flip a tool and "
                "read the gaps between [PUSH] lines: that is the plug's cadence, "
                "and the delay before the collector notices lives in it.")
            : F("off."));
#endif

    } else if (cmd == "endstops" || cmd == "e") {
#if HAS_LINEAR
        // GPIO numbers, not pad nicknames. These said "D10"/"D11" until
        // 2026-08-28, which were the DevKitC's labels — and the XIAO C5 has no
        // D11 pad at all (the castellated edge is D0..D10), so the one board
        // this can run on today was being told to check a pin that does not
        // exist. The macro is the only thing that knows where the switch is.
        Serial.print(F("[ENDSTOP] Home (GPIO"));
        Serial.print(PIN_ENDSTOP_HOME); Serial.print(F("): "));
        Serial.print(digitalRead(PIN_ENDSTOP_HOME) == HIGH ? F("TRIGGERED") : F("open"));
        Serial.print(F("   Far (GPIO"));
        Serial.print(PIN_ENDSTOP_MAX); Serial.print(F("): "));
        Serial.println(digitalRead(PIN_ENDSTOP_MAX) == HIGH ? F("TRIGGERED") : F("open"));
#else
        Serial.println(F("[ENDSTOP] no slider on this board"));
#endif

    } else if (cmd == "i2c" || cmd.startsWith("i2c ")) {
        // "i2c" with no args scans wherever the build says the screen is;
        // "i2c <sda> <scl>" scans a pair you name, which is the useful form
        // when you are trying to find out where a module actually landed.
        int sda = -1, scl = -1; bool force = false;
        if (cmd.length() > 4) {
            String args = cmd.substring(4);
            args.trim();
            // Trailing "force" lifts the TMC pin refusal below — for a bench
            // board with no driver fitted, where those pins are just pins.
            if (args.endsWith(" force")) { force = true; args = args.substring(0, args.length() - 6); args.trim(); }
            int sp = args.indexOf(' ');
            if (sp > 0) {
                sda = args.substring(0, sp).toInt();
                scl = args.substring(sp + 1).toInt();
            }
        }
#if defined(PIN_OLED_SDA) && defined(PIN_OLED_SCL)
        if (sda < 0) { sda = PIN_OLED_SDA; scl = PIN_OLED_SCL; }
#endif
        if (sda < 0 || scl < 0) {
            Serial.println(F("[I2C] This build declares no I2C pins — name them: i2c <sda> <scl>"));
        } else {
            runI2cScan(sda, scl, force);
        }

    } else if (cmd == "status") {
        printStatus();

    } else if (cmd == "help" || cmd == "?") {
        printHelp();

    } else {
        Serial.print(F("[DEBUG] Unknown command: '"));
        Serial.print(line);
        Serial.println(F("' — type 'help' for commands."));
    }
}

#if defined(CONTROL_SMART_OUTLET) || defined(ENABLE_HTTP_API)
// -----------------------------------------------------------------------------
// mdnsprobe — how long does an mDNS answer actually take to arrive here?
//
// Written on 2026-08-22 for a XIAO C5 primary that advertised itself perfectly —
// a laptop's `dns-sd -B _dustgate._tcp local.` listed both the primary and its
// node — while every query the SAME board made came back empty: 0 nodes, and 0
// Shelly plugs out of the four that are definitely there. Responder fine,
// querier blind.
//
// The first version of this probe answered the first question and got the
// second one wrong, in two ways worth recording because both are easy to walk
// back into:
//
//   1. IT MEASURED THE CACHE. It asked at 3000ms (found everything), then
//      walked a ladder of shorter timeouts to find the threshold — and 200ms
//      "found" all four plugs, which is nonsense on a network where 400ms had
//      just found none. The 3000ms query had populated the mDNS cache and every
//      rung after it was answered locally. A latency ladder has exactly one
//      cold rung, the first, and this one had spent it. Any timing question
//      here has to be asked ONCE, on a cold cache, or not at all.
//
//   2. IT RESET THE BOARD. Three 3000ms blocking queries plus the ladder is
//      ~11s inside a single processLine() call, and loop() — the only thing
//      that pets the watchdog (firmware.ino) — never came round. WDT_TIMEOUT_SEC
//      is 10. The first run survived at ~9s and the second died at ~11s, which
//      is the least useful kind of intermittent.
//
// So this version asks asynchronously and polls. mdns_query_async_new() starts
// the search and returns immediately; we watch the answer count climb, printing
// the moment each one lands, and pet the watchdog every pass. Nothing blocks for
// more than a poll interval, the arrival times are real, and each service type
// is asked exactly once from cold.
//
// That last property is why THE ORDER MATTERS and why running mdnsprobe twice
// in one boot tells you less than running it once: the second run reads its own
// cache back. Reboot between runs.
//
// This is also a prototype of where production has to go. The reason
// DISCOVER_MDNS_TIMEOUT_MS is 400ms is that the query blocks the loop task, and
// the crash above is what the far end of that tradeoff looks like — a window
// long enough to hear the plugs was, in the same breath, long enough to reset
// the board. Async removes the tradeoff instead of tuning it.
// -----------------------------------------------------------------------------

static const uint32_t kProbeWindowMs = 3000;

// Runs one PTR query and lists what answered. mdnsQueryService() does the async
// polling and the watchdog petting; this only adds the per-answer detail, which
// production doesn't print and a person at the bench wants.
static int probeOneService(const char* service, const char* proto) {
    Serial.println(F(""));
    Serial.print(F("  --- ")); Serial.print(service);
    Serial.print(F(".")); Serial.print(proto);
    Serial.print(F("  (")); Serial.print(kProbeWindowMs);
    Serial.println(F("ms, cold) ---"));

    static const int kMax = 24;
    MdnsHit hits[kMax];
    int n = mdnsQueryService(service, proto, kProbeWindowMs, hits, kMax);

    for (int i = 0; i < n; i++) {
        Serial.print(F("      "));
        Serial.print(hits[i].hostname.length() ? hits[i].hostname : String("(no hostname)"));
        Serial.print(F("  ")); Serial.print(hits[i].ip);
        if (hits[i].role.length()) { Serial.print(F("  role=")); Serial.print(hits[i].role); }
        if (hits[i].gen)           { Serial.print(F("  gen="));  Serial.print(hits[i].gen); }
        Serial.println();
    }
    return n;
}

void SerialDebugControl::runMdnsProbe() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[MDNSPROBE] WiFi not connected — nothing to probe."));
        return;
    }

    int ch = WiFi.channel();
    Serial.println(F(""));
    Serial.println(F("=== mDNS probe ==="));
    Serial.println(F("  Each service type is asked ONCE, from cold. Run this twice in"));
    Serial.println(F("  one boot and the second run just reads the cache back — reboot"));
    Serial.println(F("  between runs if you want the timings again."));
    Serial.print(F("  hostname   ")); Serial.print(WiFiProvisioner::getHostname());
    Serial.println(F(".local"));
    Serial.print(F("  ip         ")); Serial.print(WiFi.localIP().toString());
    Serial.print(F("  gw "));         Serial.print(WiFi.gatewayIP().toString());
    Serial.print(F("  mask "));       Serial.println(WiFi.subnetMask().toString());
    Serial.print(F("  ap         ")); Serial.print(WiFi.SSID());
    Serial.print(F("  bssid "));      Serial.println(WiFi.BSSIDstr());
    Serial.print(F("  channel    ")); Serial.print(ch);
    Serial.print(ch > 14 ? F("  (5 GHz)") : F("  (2.4 GHz)"));
    Serial.print(F("   rssi "));      Serial.print(WiFi.RSSI());
    Serial.println(F(" dBm"));

    // The two we actually route on go FIRST, while their caches are cold — they
    // are the ones whose arrival times decide what the production timeout has
    // to be. The meta-query goes last: it is a presence check ("does ANY
    // multicast answer reach this board"), and by the time it runs it has
    // already been answered by the two above.
    probeOneService("_shelly",   "_tcp");
    probeOneService("_dustgate", "_tcp");
    // Presence check, last: "does ANY multicast answer reach this board". By
    // the time it runs the two above have already answered that, which is the
    // point — it only earns its 3 seconds when they come back empty.
    probeOneService("_services._dns-sd", "_udp");

    Serial.println(F(""));
    Serial.println(F("  These use the same window production does now"));
    Serial.print(F("  (DISCOVER_MDNS_TIMEOUT_MS = ")); Serial.print(DISCOVER_MDNS_TIMEOUT_MS);
    Serial.println(F("ms), so what this finds, a scan finds."));
    Serial.println(F(""));
}
#endif // CONTROL_SMART_OUTLET || ENABLE_HTTP_API

#ifdef CONTROL_SMART_OUTLET

// One address, and WHY it failed.
//
// `sweep` can only ever say "nothing found", and that is the same answer for an
// unreachable network, a firewalled host, an HTTP error and a reply we parsed
// wrongly. Those want completely different fixes, so this walks the same steps
// and reports each one. Reach for it before believing a negative sweep.
void SerialDebugControl::runProbe(const String& ipArg) {
    String ip = ipArg; ip.trim();
    if (ip.length() == 0) { Serial.println(F("[PROBE] usage: probe <ip>")); return; }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[PROBE] WiFi not connected."));
        return;
    }

    Serial.printf("[PROBE] %s   (we are %s on \"%s\")\n",
                  ip.c_str(), WiFi.localIP().toString().c_str(), WiFi.SSID().c_str());

    // Step 1: raw TCP to port 80. Separates "cannot reach this host at all" —
    // a different subnet, or guest-network client isolation — from anything
    // about HTTP or our parsing.
    WiFiClient probe;
    probe.setTimeout(2);
    const uint32_t t0 = millis();
    const bool connected = probe.connect(ip.c_str(), 80);
    const uint32_t dt = millis() - t0;
    probe.stop();
    Serial.printf("  1. TCP :80          %s  (%lums)\n",
                  connected ? "connected" : "NO ROUTE / refused", (unsigned long)dt);
    if (!connected) {
        Serial.println(F("     Nothing is listening, or we cannot reach it at all."));
        Serial.println(F("     If curl from a laptop works but this does not, suspect"));
        Serial.println(F("     GUEST-NETWORK CLIENT ISOLATION — the board and the plug"));
        Serial.println(F("     are on the same subnet but forbidden to talk."));
        return;
    }

    // Step 2: the raw HTTP exchange, BEFORE the driver sees it. Separating this
    // from the parse is the whole point — the first version of this reported
    // "Status 8 parse FAILED" for what was actually a chunked-transfer problem,
    // and a reply that curl renders perfectly. Show the code and the body.
    {
        char url[64];
        snprintf(url, sizeof(url), "http://%s/cm?cmnd=Status%%208", ip.c_str());
        HTTPClient http;
        http.begin(url);
        http.setConnectTimeout(2000);
        http.setTimeout(2000);
        const int code = http.GET();
        const String body = (code == 200) ? http.getString() : String();
        http.end();
        Serial.printf("  2. HTTP GET         %d\n", code);
        if (code == 200) {
            Serial.print(F("     "));
            Serial.println(body.substring(0, 160));
        } else {
            Serial.println(F("     Reachable on :80 but the endpoint did not answer 200."));
            return;
        }
    }

    // Step 3: the driver, on the same reply.
    TasmotaOutlet t(ip.c_str(), "probe");
    const bool ok = t.probe(2000);
    Serial.printf("  3. Status 8 parse   %s\n", ok ? "ok" : "FAILED");
    if (ok) {
        Serial.printf("     %.1f W\n", t.getPowerW());
    } else {
        Serial.println(F("     Reachable, but no StatusSNS.ENERGY.Power came back."));
        Serial.println(F("     Either it is not a Tasmota, or it has no energy monitor."));
        return;
    }

    // Step 4: the claim substrate.
    String marker;
    if (t.readOwner(marker)) {
        plugclaim::Claim c = plugclaim::decideMarker(marker.c_str(), "");
        Serial.printf("  4. Mem1             \"%s\"  -> %s\n",
                      marker.c_str(), plugclaim::stateName(c.state));
    } else {
        Serial.println(F("  4. Mem1             UNREADABLE — claiming is unavailable"));
    }
}

// Sweep the local /24 for Tasmota plugs, one address at a time.
//
// WHY A BRUTE-FORCE SWEEP EXISTS AT ALL. Shelly plugs advertise _shelly._tcp, so
// `discover` finds them with three UDP queries and no guessing. Tasmota does not
// advertise anything: its mDNS needs USE_DISCOVERY compiled in and is NOT in the
// precompiled builds, upstream having left it out because "mDNS generates more
// problems than it solves" — so an Athom plug that is powered, joined and
// working is completely invisible to `discover`. Confirmed against a real plug
// on 2026-09-09: nothing on _http._tcp, nothing anywhere.
//
// That leaves knocking on every door. It is inelegant and it is the only thing
// that works.
//
// SERIAL ONLY, DELIBERATELY. A full pass is around a minute — far past any
// browser's patience — so putting this behind the HTTP discover endpoint would
// mean an async job, a progress endpoint and a UI to poll it. That is a real
// feature and this is not it: this exists so someone can find a plug's address
// once and type it in, which is the fallback the UI already supports.
void SerialDebugControl::runSweep(int from, int to) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[SWEEP] WiFi not connected — nothing to sweep."));
        return;
    }
    if (from < 1)   from = 1;
    if (to   > 254) to   = 254;
    if (from > to)  { Serial.println(F("[SWEEP] from > to")); return; }

    const IPAddress self = WiFi.localIP();
    Serial.printf("[SWEEP] %d.%d.%d.%d-%d, looking for Tasmota. Any key aborts.\n",
                  self[0], self[1], self[2], from, to);
    Serial.println(F("        (Shelly plugs advertise and are found by `discover` —"));
    Serial.println(F("         this is only for devices that advertise nothing.)"));

    while (Serial.available()) Serial.read();

    int found = 0, scanned = 0;
    const uint32_t t0 = millis();

    for (int host = from; host <= to; host++) {
        watchdog::pet();                 // a full pass outlives many loop()s

        if (Serial.available()) {
            while (Serial.available()) Serial.read();
            Serial.println(F("\n[SWEEP] stopped."));
            break;
        }
        if (host == self[3]) continue;   // that is us

        char ip[16];
        snprintf(ip, sizeof(ip), "%d.%d.%d.%d", self[0], self[1], self[2], host);

        // A SHORT timeout is the whole budget. An address with nothing at it
        // never answers ARP, so it costs the full timeout — and there are
        // usually ~250 of those. 250ms keeps a full pass near a minute; raising
        // it makes the sweep unusable long before it finds anything new.
        //
        // ⚠️ IF A KNOWN PLUG IS MISSED, SUSPECT THIS FIRST. Tasmota ships with
        // `Sleep 50` — dynamic sleep, up to 50ms of nap per loop — so an
        // ESP8285 can take noticeably longer than a Shelly to answer. `Sleep 0`
        // on the plug, or a wider `sweep from to` with fewer addresses to cover,
        // are both cheaper than raising this for all 254.
        TasmotaOutlet probe(ip, "sweep");
        scanned++;
        if (!probe.probe(250)) {
            if ((host % 32) == 0) { Serial.print('.'); }
            continue;
        }

        // It answered Status 8 with an ENERGY block, which nothing but a
        // Tasmota energy monitor does.
        found++;
        Serial.printf("\n[SWEEP] %-15s  %.1f W", ip, probe.getPowerW());

        // Who owns it, asked here for the same reason discovery asks: this is
        // the list someone picks from, so a plug belonging to another brain has
        // to arrive already labelled rather than fail mysteriously later.
        String marker;
        if (probe.readOwner(marker)) {
            plugclaim::Claim c = plugclaim::decideMarker(marker.c_str(), "");
            Serial.printf("   claim=%s", plugclaim::stateName(c.state));
            if (!c.holder.empty()) Serial.printf(" (%s)", c.holder.c_str());
        } else {
            // Not "unclaimed" — we could not ask, and the difference is what
            // stops a plug being taken on a failed read.
            Serial.print(F("   claim=unknown"));
        }
        Serial.println();
    }

    Serial.printf("\n[SWEEP] %d found, %d addresses, %lus\n",
                  found, scanned, (unsigned long)((millis() - t0) / 1000));
    if (found == 0) {
        Serial.println(F("        Nothing answered /cm?cmnd=Status%208 with an ENERGY"));
        Serial.println(F("        block. A Tasmota with no energy monitor fitted looks"));
        Serial.println(F("        the same as no Tasmota — that is deliberate, since a"));
        Serial.println(F("        plug that cannot report watts is no use as a sensor."));
    }
}

void SerialDebugControl::runDiscover() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[DISCOVER] WiFi not connected — can't scan."));
        return;
    }
    Serial.println(F("[DISCOVER] Querying mDNS for _shelly._tcp ..."));

    // mDNS/UDP query is lossy — retry a few times and merge unique hosts by
    // IP, same as the HTTP discover endpoint (see firmware.ino).
    String hitIp[DISCOVER_MAX_RESULTS];
    String hitHost[DISCOVER_MAX_RESULTS];
    int hitCount = 0;

    for (int attempt = 0; attempt < DISCOVER_MDNS_ATTEMPTS; attempt++) {
        MdnsHit mdnsHits[DISCOVER_MAX_RESULTS];
        // _shelly._tcp is advertised only by Gen2+ Shelly devices (Gen1 dropped),
        // so every responder is a supported plug — no hostname filtering needed.
        int n = mdnsQueryShellyTcp(DISCOVER_MDNS_TIMEOUT_MS, mdnsHits, DISCOVER_MAX_RESULTS);
        Serial.print(F("[DISCOVER] attempt "));
        Serial.print(attempt + 1);
        Serial.print(F("/"));
        Serial.print(DISCOVER_MDNS_ATTEMPTS);
        Serial.print(F(": "));
        Serial.print(n);
        Serial.println(F(" Shelly host(s) responded:"));

        for (int i = 0; i < n; i++) {
            String host = mdnsHits[i].hostname;
            String ip   = mdnsHits[i].ip;

            Serial.print(F("  - "));
            Serial.print(host.length() ? host : String("(no hostname)"));
            Serial.print(F("  "));
            Serial.println(ip);

            if (ip.length() == 0 || ip == "0.0.0.0") continue;

            bool dup = false;
            for (int j = 0; j < hitCount; j++) {
                if (hitIp[j] == ip) { dup = true; break; }
            }
            if (dup || hitCount >= DISCOVER_MAX_RESULTS) continue;
            hitIp[hitCount]   = ip;
            hitHost[hitCount] = host;
            hitCount++;
        }

        if (attempt < DISCOVER_MDNS_ATTEMPTS - 1) delay(DISCOVER_MDNS_RETRY_DELAY_MS);
    }

    Serial.print(F("[DISCOVER] "));
    Serial.print(hitCount);
    Serial.println(F(" unique host(s) across all attempts:"));

    if (hitCount == 0) {
        Serial.println(F("  (nothing found — check the outlet is powered, on the same WiFi network,"));
        Serial.println(F("   and that mDNS is enabled in the Shelly app's device settings)"));
        return;
    }

    for (int i = 0; i < hitCount; i++) {
        const String& ip   = hitIp[i];
        const String& host = hitHost[i];

        // Gen2+ only (Gen1 dropped) — see firmware.ino's discover handling.
        ShellyGen2Outlet gen2(ip.c_str(), "discover");
        bool  ok  = gen2.poll();
        float pw  = gen2.getPowerW();
        int   gen = 2;
        String devName = ok ? fetchShellyDeviceName(ip.c_str(), gen) : String();
        Serial.print(F("  - ")); Serial.print(host); Serial.print(F("  ")); Serial.print(ip);
        Serial.print(F("  probe -> reachable="));
        Serial.print(ok ? F("yes") : F("no"));
        Serial.print(F(" gen="));
        Serial.print(ok ? gen : 0);
        Serial.print(F(" powerW="));
        Serial.print(pw, 1);
        Serial.print(F(" name="));
        Serial.println(devName.length() ? devName : String("(none set)"));
    }
    Serial.print(F("[DISCOVER] "));
    Serial.print(hitCount);
    Serial.println(F(" would appear in the outlet scan."));
}
#endif


// -----------------------------------------------------------------------------
// `i2c [sda] [scl]` — what is actually on the bus.
//
// Written for the status-screen bring-up, where the first question is never
// "is my code right" but "is the module answering at all". A scan separates
// three failures that look identical from the outside: nothing powered, wrong
// address, and SDA/SCL swapped.
//
// It names the pins in its output on purpose. I2C on an ESP32 is remapped
// through the GPIO matrix, every board here puts it somewhere different, and a
// scan of the wrong pair reports "no devices" just as confidently as a dead
// module does.
// -----------------------------------------------------------------------------
void SerialDebugControl::runI2cScan(int sda, int scl, bool force) {
    // REFUSED, not warned about: on the DevKitC the "obvious" I2C pins are the
    // TMC2209's EN and DIR, and EN is active LOW — a scan pulling it down is a
    // scan that silently energises the motor. This is the same trap the board's
    // pin map dodges (see attic/linear/devkitc_wroom32.h), and a debug command is
    // exactly where someone would walk back into it.
#if defined(PIN_TMC_EN) && defined(PIN_TMC_DIR)
    if (!force && (sda == PIN_TMC_EN || sda == PIN_TMC_DIR ||
                   scl == PIN_TMC_EN || scl == PIN_TMC_DIR)) {
        Serial.print(F("[I2C] Refusing: GPIO"));
        Serial.print(PIN_TMC_EN); Serial.print(F("/"));
        Serial.print(PIN_TMC_DIR);
        Serial.println(F(" are the TMC2209 EN/DIR on this board — scanning them drives the driver."));
        Serial.println(F("[I2C] If no driver is fitted, repeat with: i2c <sda> <scl> force"));
        return;
    }
    if (force) Serial.println(F("[I2C] force: scanning pins that are normally refused."));
#endif

    Serial.print(F("[I2C] Scanning SDA=GPIO")); Serial.print(sda);
    Serial.print(F(" SCL=GPIO")); Serial.print(scl);
    Serial.println(F(" at 100kHz..."));

    // Wire.end() FIRST, and this is not defensive tidiness. On a board with a
    // status screen the bus is already running (StatusScreen::begin() took it at
    // boot), and a second Wire.begin() on a live bus does not re-initialise it —
    // it leaves the peripheral in a state where every address NAKs. The symptom
    // is the one that cost a bench cycle here: `[SCREEN] SSD1306 up` at boot,
    // and then a scan of those same pins reporting an empty bus.
    Wire.end();
    // 100kHz for the scan even though the screen runs at 400k: a marginal pull-up
    // or a long dupont run fails at 400k and answers fine at 100k, and knowing
    // the module is ALIVE is worth more here than knowing it is fast.
    Wire.begin(sda, scl);
    Wire.setClock(100000);

    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() != 0) continue;
        found++;
        Serial.print(F("  0x"));
        if (addr < 0x10) Serial.print('0');
        Serial.print(addr, HEX);
        // A guess at what answered, because "0x27" means nothing at 11pm and the
        // difference between an SSD1306 and a character-LCD backpack is the
        // difference between a firmware setting and the wrong part entirely.
        switch (addr) {
            case 0x3C: Serial.println(F("  SSD1306 OLED (the expected status screen)")); break;
            case 0x3D: Serial.println(F("  SSD1306 OLED at its ALTERNATE address — set -DOLED_I2C_ADDR=0x3D")); break;
            case 0x27:
            case 0x3F: Serial.println(F("  looks like a PCF8574 character-LCD backpack — NOT an SSD1306; this firmware cannot drive it")); break;
            case 0x68: Serial.println(F("  RTC or IMU")); break;
            case 0x76:
            case 0x77: Serial.println(F("  BME/BMP sensor")); break;
            default:   Serial.println(F("  unknown device")); break;
        }
    }

    if (found == 0) {
        Serial.println(F("[I2C] Nothing answered. In the order worth checking:"));
        Serial.println(F("  1. Power — VCC on 3V3 (NOT 5V), GND common with the board"));
        Serial.println(F("  2. SDA/SCL swapped — try: i2c <scl> <sda>"));
        Serial.println(F("  3. Wrong pins for how it is actually wired"));
        Serial.println(F("  4. Bad jumper. A scan cannot tell a broken wire from a dead module."));
    } else {
        Serial.print(F("[I2C] ")); Serial.print(found); Serial.println(F(" device(s) answered."));
    }

#if defined(PIN_OLED_SDA) && defined(PIN_OLED_SCL)
    // Hand the bus back the way the screen driver left it. Without this a scan
    // would leave the display running at 100kHz for the rest of the session —
    // harmless, but it would quietly invalidate the refresh timing anyone
    // measured afterwards.
    if (sda != PIN_OLED_SDA || scl != PIN_OLED_SCL) {
        Wire.end();
        Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
    }
    Wire.setClock(400000);
#endif
}

void SerialDebugControl::printStatus() {
    Serial.println(F("--- Status ---"));
    Serial.print(F("  Requested stop:    ")); Serial.println(_requestedStop);
    Serial.print(F("  EStop pending:     ")); Serial.println(_eStopPending ? F("YES") : F("no"));
    Serial.print(F("  Homing speed:      ")); Serial.print(HOMING_SPEED_STEPS_PER_SEC, 0); Serial.println(F(" steps/sec"));
#if HAS_LINEAR
    Serial.print(F("  Home endstop (GPIO")); Serial.print(PIN_ENDSTOP_HOME); Serial.print(F("): "));
    Serial.println(digitalRead(PIN_ENDSTOP_HOME) == HIGH ? F("TRIGGERED") : F("open"));
    Serial.print(F("  Far endstop (GPIO"));  Serial.print(PIN_ENDSTOP_MAX);  Serial.print(F("): "));
    Serial.println(digitalRead(PIN_ENDSTOP_MAX)  == HIGH ? F("TRIGGERED") : F("open"));
#endif
    Serial.print(F("  Home datum endstop:")); Serial.print(F(" "));
    Serial.println(g_homeIsMaxEndstop ? F("MAX (the user's left)") : F("HOME (the user's left)"));
    Serial.println(F("  Stop positions (from g_stopPositionsMM[], Gate 1..N left→right):"));
    for (int i = 0; i <= NUM_STOPS; i++) {
        Serial.print(F("    [")); Serial.print(i); Serial.print(F("]  "));
        Serial.print(g_stopPositionsMM[i], 2); Serial.print(F(" mm  = "));
        long steps = (long)(g_stopPositionsMM[i] * stepsPerMM()) * (-HOME_DIRECTION);
        Serial.print(steps); Serial.println(F(" steps"));
    }
    Serial.println(F("--------------"));
}

void SerialDebugControl::printHelp() {
    Serial.println(F(""));
    Serial.println(F("=== Serial Debug Control ==="));
    Serial.println(F("  0-7               Select position (0=home)"));
    Serial.println(F("  estop             Immediate stop (latches until 'home')"));
    Serial.println(F("  home              Re-trigger homing sequence"));
    Serial.println(F("  jog <mm>          Move relative: + = away from home, - = toward home"));
    Serial.println(F("  calibrate <m> <n> Dual-endstop reference sweep: model (rockler-2.5|rockler-4|custom) + gate count"));
    Serial.println(F("  homeside l|r      Report which side it homed to; 'right' re-homes to the left endstop"));
#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)
    Serial.println(F("  servo <1-4> <deg> Servo bring-up: move servo N to angle (or 'servo N detach')"));
#endif
    Serial.println(F("  clearcal          Erase EEPROM calibration (reload from config.h)"));
    Serial.println(F("  wifireset         Erase WiFi credentials, reboot into setup portal"));
    Serial.println(F("  gconf             Read GCONF + CHOPCONF from driver"));
    Serial.println(F("  status            Print state, stop positions, both endstops"));
    Serial.println(F("  reset (retry)     Retry the drive and clear latched boot faults (no reboot)"));
    Serial.println(F("  endstops (e)      Print both endstop states, with their GPIO numbers"));
    Serial.println(F("  i2c [sda] [scl]   Scan the I2C bus — what is out there, and at what address ('force' to override refusals)"));
#ifdef CONTROL_SMART_OUTLET
    Serial.println(F("  discover          Scan mDNS for Shelly outlets, print raw + filtered results"));
    Serial.println(F("  probe <ip>        One address, verbosely, saying WHICH step failed —"));
    Serial.println(F("                    TCP, the Status 8 parse, or Mem1. Use it before"));
    Serial.println(F("                    believing a `sweep` that found nothing."));
    Serial.println(F("  sweep [from] [to] Knock on every address in the local /24 looking for"));
    Serial.println(F("                    Tasmota plugs. They advertise NOTHING, so `discover`"));
    Serial.println(F("                    cannot see one however healthy it is. ~1 min; any key"));
    Serial.println(F("                    aborts. Default range 1-254."));
#endif
#if defined(CONTROL_SMART_OUTLET) || defined(ENABLE_HTTP_API)
    Serial.println(F("  mdnsprobe         Radio facts, then time every mDNS answer (once per boot — it warms the cache)"));
#endif
#ifdef CONTROL_SMART_OUTLET
    Serial.println(F("  plugtrace         Toggle: timestamp every frame a plug pushes — how fast does it report?"));
#endif
    Serial.println(F("  provision <json>  Write WiFi+host to NVS: {\"ssid\":\"x\",\"pass\":\"y\",\"host\":\"dustgate\"}"));
    Serial.println(F("  press             Fire the collector's RF transmitter ONCE, now."));
    Serial.println(F("                    Bypasses the retry policy — no cooldown, no"));
    Serial.println(F("                    spin-up grace, no sensor needed. Needs a"));
    Serial.println(F("                    control.rf block on the layout's collector."));
    Serial.println(F("  help              Show this list"));
#if defined(PIN_PIXEL) || defined(PIN_LED)
    // The pixel is the only diagnostic you get once the board is in a box and
    // the USB cable is gone, and its legend otherwise lives only in WIRING.md §1
    // — which is not where you are when you're squinting at a blinking light.
    //
    // Kept in step with utils/StatusLed.h, which is the source of truth for both
    // colour and rate. Note the two oranges: solid means something is moving,
    // blinking means the WiFi dropped. That pair is the one real ambiguity here,
    // so it is spelled out rather than left to the reader to notice.
    Serial.println(F("--- Status pixel ---"));
    Serial.println(F("  green             Ready — routing live (node: primary linked)"));
    Serial.println(F("  blue, slow pulse  On WiFi, nothing to do yet — no layout stored"));
    Serial.println(F("  orange, SOLID     Moving. Slow blink = homing, fast = calibration sweep"));
    Serial.println(F("  orange, blinking  WiFi lost (or never joined). Blinks ~1.5x/sec"));
    Serial.println(F("  white, blinking   Setup portal is up, waiting for WiFi credentials"));
    Serial.println(F("  red, fast pulse   Fault — init failed or e-stop latched"));
#endif
#if defined(CONTROL_SMART_OUTLET) || defined(ENABLE_HTTP_API)
    Serial.println(F("--- Network ---"));
    if (WiFi.status() == WL_CONNECTED) {
        String host = WiFiProvisioner::getHostname();
        Serial.print(F("  Web UI:     http://"));
        Serial.print(host);
        Serial.print(F(".local  (or http://"));
        Serial.print(WiFi.localIP().toString());
        Serial.println(F(")"));
        Serial.print(F("  Setup:      http://"));
        Serial.print(host);
        Serial.println(F(".local/#/setup"));
    } else {
        Serial.println(F("  WiFi not connected."));
        Serial.println(F("  Connect to \"" WIFI_PORTAL_SSID "\" to run first-time setup."));
    }
    Serial.println(F("  wifireset   Forget WiFi credentials, restart setup portal"));
#endif
    Serial.println(F("============================"));
    Serial.println(F(""));
}

#endif // CONTROL_SERIAL_DEBUG || ENABLE_SERIAL_COMMANDS

// =============================================================================
// dustgate_node.cpp — the SECONDARY firmware. A dumb servo bank in the star.
//
// WHY THIS IS A SEPARATE PROGRAM, not #ifdefs in firmware.ino:
// the main sketch is ~1700 lines with the stepper, endstops, homing, the
// reference sweep, Shelly polling and the routing brain woven through 97
// separate places. Splitting that with the preprocessor would leave a sketch
// nobody can read and a servo-only path nobody actually exercises. The RFC says
// a secondary "is essentially a stripped-down DustGate node… P3 is mostly
// SUBTRACTING from the current firmware" — so here is the subtraction, done
// once, as a program small enough to hold in your head.
//
// What it does, in full:
//   1. join WiFi (shared WiFiProvisioner — same captive portal as the primary)
//   2. advertise itself over mDNS so the primary's picker can find it
//   3. accept ONE WebSocket at /nodelink and answer HELLO / PING / SET
//   4. move a servo channel to an angle
//
// What it deliberately does NOT have: a topology, a router, a sequencer, tool
// power sensing, a web UI, calibration, a stepper. It never decides anything.
// SET frames arrive already resolved to a channel + an absolute angle (see
// control/NodeLink.h), which is exactly why this file can be this short — and
// why a $5 board can be a node.
//
// FAIL-SAFE: if the primary disappears, every servo HOLDS. There is no timeout
// that closes gates, no homing on reconnect, no autonomous behaviour of any
// kind. Losing the link mid-cut must never slam a gate on a running tool.
//
// Build:  pio run -e dustgate_node
// =============================================================================

#include <Arduino.h>
#include "../config.h"

// WiFiProvisioner.h FIRST, before ESPAsyncWebServer: it pulls in the core's
// <WebServer.h> (for the captive portal), whose HTTP_GET/HTTP_POST enums collide
// with ESPAsyncWebServer's unless the core header is seen first. Same ordering
// the main sketch relies on — see the note in api/HttpApiServer.cpp.
#include "../utils/WiFiProvisioner.h"

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>          // the persisted owner claim — see THE CLAIM below
#include <ESPmDNS.h>
#include <esp_heap_caps.h>        // bootTrace() — internal-DRAM headroom at each stage
#include "../utils/Watchdog.h"

#include "../motor/ServoActuator.h"
#include "../control/NodeLink.h"
#include "../utils/StatusLed.h"
#include "../utils/StatusScreen.h"   // optional SSD1306; nothing on a board without one

#if !HAS_SERVO
  #error "dustgate_node needs a servo bank — build with -DENABLE_SERVO and a board that defines SERVO_PWM_PIN_1"
#endif

static AsyncWebServer server(API_PORT);
static AsyncWebSocket nodeWs("/nodelink");

static ServoActuator servos[SERVO_COUNT];
static const int SERVO_PINS[SERVO_COUNT] = {
    SERVO_PWM_PIN_1, SERVO_PWM_PIN_2, SERVO_PWM_PIN_3, SERVO_PWM_PIN_4
};

// Pending SET, handed from the AsyncTCP task to loop(). One slot: the primary
// serializes moves (one servo at a time is its current budget), so a second
// command can only mean the first is stale.
static portMUX_TYPE      cmdMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool     cmdPending = false;
static topo::nodelink::SetCommand cmdSlot;

// The move currently being reported on, so arrival can be announced when the
// servo settles rather than at a guessed interval.
static char pendingSel[48]   = "";
static char pendingState[32] = "";
static bool awaitingSettle   = false;

// Written from the AsyncTCP task, read by loop() for the status pixel. A plain
// bool is enough: it is advisory display state, not a control input.
static volatile bool g_primaryLinked = false;
// When the last SET was actually commanded. The screen ages it ("last cmd 3s
// ago"), which is the one number that separates "linked" from "linked and
// being talked to" — the two look identical on the pixel.
static volatile uint32_t g_lastCmdMs = 0;
static volatile uint32_t g_linkedClientId = 0;

// ── THE CLAIM ──────────────────────────────────────────────────────────────
//
// This node belongs to ONE primary. Before this existed, WS_EVT_CONNECT pointed
// the "linked client" at whichever primary connected most recently, so a bench
// brain and a shop brain could both hold sockets and both drive these servos —
// with neither told, and a gate that contradicts the routing of both shops.
// (Same shape as the smart-plug theft in RFC §8, worse consequences.)
//
// FIRST COMPLETED HANDSHAKE WINS, and the owner is persisted: a claim that
// evaporated on a power cut would just be re-raced at every brownout, and on a
// bench that is several times a day.
//
// NOT RELEASED ON DISCONNECT, deliberately. A node holds its gates when the
// link drops (the fail-safe at the top of this file), so a primary rebooting is
// an ordinary event — releasing the claim then would let a neighbouring brain
// quietly adopt shop hardware during a reboot. Only an explicit, user-confirmed
// takeover moves ownership.
static Preferences claimPrefs;
static const char* kClaimNs  = "nodeclaim";
static const char* kClaimKey = "owner";
static String g_owner;              // "" = unclaimed
// Which client id passed the handshake as the owner. A SET from any other
// socket is refused: an accepted WELCOME is what earns the right to command,
// not merely having a connection open.
static volatile uint32_t g_ownerClientId = 0;
static volatile bool     g_ownerLinked   = false;

static void loadClaim() {
    claimPrefs.begin(kClaimNs, /*readOnly=*/true);
    g_owner = claimPrefs.getString(kClaimKey, "");
    claimPrefs.end();
    if (g_owner.length()) {
        Serial.print(F("[CLAIM] This node belongs to ")); Serial.println(g_owner);
    } else {
        Serial.println(F("[CLAIM] Unclaimed — the first primary to say HELLO owns it."));
    }
}

static void saveClaim(const String& owner) {
    g_owner = owner;
    claimPrefs.begin(kClaimNs, /*readOnly=*/false);
    claimPrefs.putString(kClaimKey, owner);
    claimPrefs.end();
}

static bool anyServoMoving() {
    for (int i = 0; i < SERVO_COUNT; i++) if (servos[i].isMoving()) return true;
    return false;
}

// -----------------------------------------------------------------------------
// NodeLink frame handling (AsyncTCP task — never touches a servo directly)
// -----------------------------------------------------------------------------
static void onNodeWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client,
                          AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        Serial.print(F("[NODE] Primary connected — client #"));
        Serial.print(client->id());
        Serial.print(F(" from ")); Serial.print(client->remoteIP());
        Serial.print(F(", ")); Serial.print(nodeWs.count()); Serial.println(F(" open"));
        g_linkedClientId = client->id();
        g_primaryLinked  = true;
        return;
    }
    if (type == WS_EVT_DISCONNECT || type == WS_EVT_ERROR) {
        // HOLD. No servo command here, by design — see the fail-safe note above.
        Serial.print(F("[NODE] Primary disconnected — client #"));
        Serial.print(client->id());
        Serial.println(F(" — holding all gates."));
        // Track the ONE linked client by id rather than inferring from count().
        // count() includes the client currently being torn down, which is why this
        // used to read `> 1`; that guess also went wrong the other way, reporting
        // linked when all that remained was a zombie the server hadn't reaped.
        if (client->id() == g_linkedClientId) g_primaryLinked = false;
        // The OWNER's socket closing means "no owner is connected", not "the
        // node is unowned" — the claim itself survives, so a rebooting primary
        // gets its node back rather than losing it to whoever dials in first.
        if (client->id() == g_ownerClientId) g_ownerLinked = false;
        return;
    }
    if (type != WS_EVT_DATA) return;

    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (!(info->final && info->index == 0 && info->len == len)) return;
    if (info->opcode != WS_TEXT) return;

    StaticJsonDocument<384> doc;
    if (deserializeJson(doc, data, len)) return;
    JsonObjectConst f = doc.as<JsonObjectConst>();
    const char* t = f["t"].as<const char*>();
    if (!t) return;

    StaticJsonDocument<256> reply;

    // Declared HERE, not inside the HELLO branch, and it matters: ArduinoJson
    // stores a `const char*` value BY POINTER without copying, and the document
    // is serialized after the if/else below. A String scoped to the branch is
    // destroyed at its closing brace, so serializeJson() then read freed heap —
    // which by that point held the reply being assembled, producing a WELCOME
    // whose nodeId was a fragment of its own JSON:
    //     {"t":"WELCOME","v":1,"nodeId":"d\"t\":\"WELCOME\",",...}
    // board/fw escaped only because they are string literals. Any const char*
    // handed to a build*() helper must outlive the serialize call.
    String host;

    if (strcmp(t, "HELLO") == 0) {
        if ((f["v"] | 0) != topo::nodelink::kVersion) {
            Serial.println(F("[NODE] HELLO version mismatch — refusing."));
            client->close();
            return;
        }
        // Identify by mDNS hostname: stable across reboots and DHCP, and the
        // same string the primary's topology binds link.host to.
        host = WiFiProvisioner::getHostname();

        // ── the claim decision ──────────────────────────────────────────────
        const char* asker = f["primaryId"] | "";
        const bool  wantsTakeover = f["takeover"] | false;
        bool accepted = true;

        if (g_owner.length() == 0) {
            saveClaim(asker);                       // unclaimed → first asker wins
            Serial.print(F("[CLAIM] Adopted by ")); Serial.println(asker);
        } else if (g_owner == asker) {
            // Ordinary reconnect.
        } else if (wantsTakeover) {
            // A human was shown what breaks and said yes. Logged loudly and
            // permanently: this is the frame that makes another shop's gates
            // stop moving, and whoever debugs THAT will end up reading this log.
            Serial.print(F("[CLAIM] TAKEOVER (user-confirmed): ")); Serial.print(g_owner);
            Serial.print(F(" -> ")); Serial.println(asker);
            saveClaim(asker);
        } else {
            accepted = false;
            Serial.print(F("[CLAIM] REFUSED ")); Serial.print(asker);
            Serial.print(F(" — this node belongs to ")); Serial.println(g_owner);
        }

        if (accepted) {
            g_ownerClientId = client->id();
            g_ownerLinked   = true;
        }
        topo::nodelink::buildWelcome(reply.to<JsonObject>(), host.c_str(),
                                     BOARD_NAME, "1.0.0", SERVO_COUNT, /*linear=*/0,
                                     g_owner.c_str(), accepted);
    } else if (strcmp(t, "PING") == 0) {
        topo::nodelink::buildPong(reply.to<JsonObject>());
    } else if (strcmp(t, "SET") == 0) {
        topo::nodelink::SetCommand cmd;
        const char* err = nullptr;
        // THE ENFORCEMENT. A WELCOME that was accepted is what earns the right
        // to command; merely holding a socket does not. Checked on every SET
        // rather than once at connect, because that is the frame that moves a
        // real valve — and because a client id can be reused after a reconnect.
        if (!g_ownerLinked || client->id() != g_ownerClientId) {
            Serial.print(F("[SET] REFUSED — client #")); Serial.print(client->id());
            Serial.print(F(" is not the owner (")); Serial.print(g_owner);
            Serial.println(F(")"));
            topo::nodelink::buildAck(reply.to<JsonObject>(), f["seq"] | 0, false,
                                     "not the owner of this node");
        } else if (!topo::nodelink::parseSetFrame(f, cmd, err)) {
            Serial.print(F("[SET] MALFORMED — ")); Serial.println(err ? err : "?");
            topo::nodelink::buildAck(reply.to<JsonObject>(), f["seq"] | 0, false, err);
        } else if (!cmd.isServo) {
            Serial.println(F("[SET] REFUSED — linear move, no stepper on this node."));
            // No stepper on this board. Refusing is the honest answer — the
            // primary marks the gate un-driveable rather than believing it moved.
            topo::nodelink::buildAck(reply.to<JsonObject>(), cmd.seq, false,
                                     "no linear actuator on this node");
        } else if (cmd.channel < 0 || cmd.channel >= SERVO_COUNT) {
            Serial.print(F("[SET] REFUSED — channel ")); Serial.print(cmd.channel);
            Serial.print(F(" out of range (this board has ")); Serial.print(SERVO_COUNT);
            Serial.println(F(")"));
            topo::nodelink::buildAck(reply.to<JsonObject>(), cmd.seq, false, "no such channel");
        } else {
            portENTER_CRITICAL(&cmdMux);
            cmdSlot    = cmd;
            cmdPending = true;
            portEXIT_CRITICAL(&cmdMux);
            // The line the bring-up actually needs: what arrived, on which pin.
            Serial.print(F("[SET] Servo")); Serial.print(cmd.channel + 1);
            Serial.print(F(": ")); Serial.print(cmd.angle); Serial.print(F("deg"));
            Serial.print(F("  (pin ")); Serial.print(SERVO_PINS[cmd.channel]);
            Serial.print(F(", ")); Serial.print(cmd.selectorId);
            Serial.print(F(" -> ")); Serial.print(cmd.stateId);
            Serial.print(F(", seq ")); Serial.print(cmd.seq); Serial.println(F(")"));
            // ACK means accepted, not arrived; arrival is a separate STATE frame.
            topo::nodelink::buildAck(reply.to<JsonObject>(), cmd.seq, true);
        }
    } else {
        return;   // unknown frame — ignore rather than guess
    }

    String s; serializeJson(reply, s);
    client->text(s);
}

static void reportState(const char* selectorId, const char* stateId, bool moving) {
    StaticJsonDocument<192> doc;
    topo::nodelink::buildState(doc.to<JsonObject>(), selectorId, stateId, moving);
    String s; serializeJson(doc, s);
    nodeWs.textAll(s);
}

// -----------------------------------------------------------------------------
// Boot-stage memory trace.
//
// The XIAO C5 bring-up died at ~1.2 s with one IDF line — "Failed to allocate
// dummy cacheline for PSRAM memory barrier!" — and nothing else on the wire: no
// banner, no panic, no reboot loop. A stock sketch on the same board reports
// 8 MB of working PSRAM, so the fault is in THIS program, and a boot that dies
// before its own first print tells you nothing about where.
//
// Internal DRAM is the number that matters — PSRAM can't back a DMA descriptor,
// an ISR stack or a WiFi buffer — so print it separately from the total. Print
// the largest single block too: early allocation failures are usually
// fragmentation rather than exhaustion, and the two look identical if you only
// watch the free total. Flush each line, or a hang eats the one that mattered.
static void bootTrace(const char* stage) {
    Serial.printf("[BOOT] %-8s t=%5lums heap=%6u internal=%6u largest=%6u psram=%u\n",
                  stage, (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)ESP.getPsramSize());
    Serial.flush();
}

void setup() {
    Serial.begin(SERIAL_BAUD);
#if BOARD_HAS_NATIVE_USB
    unsigned long t0 = millis();
    while (!Serial && (millis() - t0) < 5000) { delay(10); }
#endif
    delay(100);
    bootTrace("serial");

    // Before WiFi, so the pixel is already saying something during the blocking
    // connect below — on a board with no serial attached that is the only sign
    // it got past reset at all.
    statusled::begin();
    statusled::set(statusled::BOOTING);
    statusled::update();

    // Jeff wants a screen on the nodes in his own shop; a product can't require
    // one on every board in the building. So it is the same optional fitting
    // here as on the primary, and a node without one is unchanged.
    if (statusscreen::begin()) Serial.println(F("[SCREEN] panel answered at 0x3C — drawing"));
#if defined(PIN_OLED_SDA) && defined(PIN_OLED_SCL)
    else Serial.println(F("[SCREEN] declared, but nothing answered; disabled"));
#endif

    Serial.println(F("=== DustGate node (secondary) ==="));
    Serial.print(F("Board: ")); Serial.println(BOARD_NAME);

    // Same provisioning path as the primary: hardcoded creds, then NVS, then a
    // captive portal. A secondary is headless, so the portal is the only way in
    // — and the portal's loop never returns, so without this tick the pixel
    // would sit frozen on BOOTING for as long as the node waits to be told which
    // WiFi to join. That is the one state where a human definitely needs to act.
    WiFiProvisioner::setPortalTick([]() {
        statusled::set(statusled::PORTAL);
        statusled::update();
        // A headless node in the portal is the hardest state in the shop to
        // read. Where a screen is fitted, it is the only thing that says what
        // to join and where to go.
        statusscreen::Facts f;
        f.role     = statusscreen::Role::NODE;
        f.status   = statusled::PORTAL;
        f.apName   = WIFI_PORTAL_SSID;
        f.portalIp = "192.168.4.1";
        statusscreen::update(f);
    });
    // Load the claim BEFORE the WebSocket can accept anyone: a HELLO that
    // arrived first would otherwise adopt a node that already has an owner.
    loadClaim();
    bootTrace("claim");

    WiFiProvisioner::begin();
    WiFiProvisioner::setPortalTick(nullptr);
    bootTrace("wifi");

    for (int i = 0; i < SERVO_COUNT; i++) servos[i].begin(SERVO_PINS[i]);
    bootTrace("servos");

    // Advertise for the primary's node picker (GET /api/nodes/discover).
    // The TXT record is what distinguishes a node from the primary's own
    // _http._tcp advert.
    if (MDNS.begin(WiFiProvisioner::getHostname().c_str())) {
        MDNS.addService("dustgate", "tcp", API_PORT);
        MDNS.addServiceTxt("dustgate", "tcp", "role",   "secondary");
        MDNS.addServiceTxt("dustgate", "tcp", "board",  BOARD_NAME);
        MDNS.addServiceTxt("dustgate", "tcp", "servos", String(SERVO_COUNT).c_str());
    }

    bootTrace("mdns");

    nodeWs.onEvent(onNodeWsEvent);
    server.addHandler(&nodeWs);
    server.begin();
    bootTrace("server");
    Serial.print(F("[NODE] Listening on ws://"));
    Serial.print(WiFiProvisioner::getHostname());
    Serial.println(F(".local/nodelink"));

    // Watchdog armed last, after the blocking WiFi connect — same discipline as
    // the primary sketch.
    watchdog::begin();
    bootTrace("ready");
}


// -----------------------------------------------------------------------------
// The optional status screen. A node's whole world is one question — can the
// brain reach me? — so that is what its screen answers, and it answers it with
// the same statusled state the pixel is showing.
//
// UNVERIFIED: no panel has been wired to any board. Compiles to nothing on a
// node built without one, which is every node today.
// -----------------------------------------------------------------------------
static void updateStatusScreen() {
    if (!statusscreen::present()) return;

    statusscreen::Facts f;
    f.role   = statusscreen::Role::NODE;
    f.status = statusled::state();
    f.motion = statusled::motion();

    static String host;
    if (!host.length()) host = WiFiProvisioner::getHostname();
    f.hostname = host.c_str();

    static String ssid;
    ssid = WiFi.SSID();
    f.ssid = ssid.length() ? ssid.c_str() : nullptr;
    if (WiFi.status() == WL_CONNECTED) {
        const int rssi = WiFi.RSSI();
        f.wifiBars = rssi >= -60 ? 4 : rssi >= -70 ? 3 : rssi >= -80 ? 2 : rssi >= -90 ? 1 : 0;
    } else {
        f.wifiBars = 0;
    }

    f.servoCount = SERVO_COUNT;
    // The OWNER, not "whoever is connected": that is the name this node will
    // still be waiting for after a reboot, and the useful thing to read when it
    // is waiting.
    if (g_primaryLinked && g_owner.length()) f.primaryHost = g_owner.c_str();

    if (g_lastCmdMs) f.lastCmdSec = (int)((millis() - g_lastCmdMs) / 1000);

    statusscreen::update(f);
}

void loop() {
    watchdog::pet();
    WiFiProvisioner::maintain();

    // REQUIRED, not housekeeping. ESPAsyncWebServer never reaps disconnected
    // WebSocket clients on its own — without this call they accumulate until the
    // server stops accepting new ones. A primary that can't connect retries every
    // second forever (kReconnectMinMs), so a node left running beside a failing
    // link burns through client slots fast, and the symptom is the confusing one:
    // a node that answers a laptop fine while refusing the primary indefinitely.
    nodeWs.cleanupClients();

    // Status pixel — the node's only UI. Derived fresh each loop rather than
    // set at transitions, so it can never latch a stale colour after a silent
    // WiFi drop (the failure this is most likely to be diagnosing).
    if (WiFi.status() != WL_CONNECTED) {
        statusled::set(statusled::NO_WIFI);
    } else {
        statusled::set(g_primaryLinked ? statusled::READY : statusled::ONLINE);
    }
    // Orange for the whole sweep, not just the instant the frame landed.
    statusled::setMoving(anyServoMoving());
    statusled::update();
    updateStatusScreen();

    // Advance sweeps and effect the deferred detach.
    for (int i = 0; i < SERVO_COUNT; i++) servos[i].update();

    // Drain a pending SET. Servos are only ever touched from here.
    bool have = false;
    topo::nodelink::SetCommand cmd;
    portENTER_CRITICAL(&cmdMux);
    if (cmdPending) { cmd = cmdSlot; cmdPending = false; have = true; }
    portEXIT_CRITICAL(&cmdMux);

    if (have) {
        servos[cmd.channel].setHoldAtRest(cmd.holdAtRest);
        servos[cmd.channel].moveTo(cmd.angle);
        // Distinct from the [SET] line above: that one says a frame ARRIVED, this
        // says the PWM was actually commanded. If you see [SET] and never [MOVE],
        // the frame is being dropped between the socket task and the loop.
        Serial.print(F("[MOVE] Servo")); Serial.print(cmd.channel + 1);
        Serial.print(F(" -> ")); Serial.print(cmd.angle); Serial.println(F("deg"));
        topo::nodelink::strlcpy_(pendingSel,   cmd.selectorId, sizeof(pendingSel));
        topo::nodelink::strlcpy_(pendingState, cmd.stateId,    sizeof(pendingState));
        awaitingSettle = true;
        statusled::flashActivity();   // visible confirmation at the gate itself
        g_lastCmdMs = millis();
        statusscreen::note();
        reportState(pendingSel, pendingState, true);
    }

    if (awaitingSettle && !anyServoMoving()) {
        awaitingSettle = false;
        reportState(pendingSel, pendingState, false);
    }
}

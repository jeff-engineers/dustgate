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
//   4. move its actuator to the number the SET carried
//
// TWO PERSONALITIES, ONE PROGRAM (2026-08-28). Which one a board has is decided
// by its pin map, exactly as on the primary:
//
//   HAS_SERVO   four PWM ball valves. A SET carries an ANGLE.
//   HAS_LINEAR  one ST3215 on a serial bus, driving the rack. A SET carries
//               an absolute POSITION IN MM from the datum.
//
// They are #if'd rather than split into two programs because everything around
// the actuator — WiFi, the captive portal, the claim, mDNS, the pixel, the
// optional screen, the watchdog — is identical, and that is most of this file.
// The servo/slider split is three functions and the SET branch.
//
// What it deliberately does NOT have: a topology, a router, a sequencer, tool
// power sensing, a web UI. It never decides WHERE anything should go. SET frames
// arrive already resolved to a channel + a number (see control/NodeLink.h),
// which is exactly why this file can be this short — and why a $5 board can be
// a node.
//
// ── CALIBRATION IS THE ONE EXCEPTION, AND IT IS NEW ARCHITECTURE ─────────────
// CLAUDE.md's rule is that a node gets already-resolved numbers and owns no
// state machine. That still holds for MOVES. It cannot hold for HOMING: a
// homing sweep is a closed loop between an endstop and a servo, sampled every
// few milliseconds, and it cannot round-trip per step over WiFi. So a slider
// node owns its own sweep — the first node in this design with a brain.
//
// It is a state machine ticked from loop(), not a blocking call, for a concrete
// reason: WDT_TIMEOUT_SEC is 10, only loop() pets the watchdog, and a full-span
// sweep at homing speed takes the better part of a minute. A blocking sweep
// would reboot the board somewhere in the middle of it.
//
// WHY IT HOMES AT BOOT, unasked. A step-counting servo has no datum of its own
// and comes back from a power cycle holding nothing (torque off), so a node that
// has just restarted does not know where its carriage is AND is not holding it
// there. There is no safe move to make from that state and no way to answer a
// SET honestly. Sweeping is how it gets an answer. The risk this accepts is a
// node that reboots mid-cut moving its carriage; the alternative — refusing
// every SET until a human walks over — is worse, and the carriage was already
// unheld the moment the power dropped.
//
// FAIL-SAFE: if the primary disappears, every actuator HOLDS. There is no
// timeout that closes gates, no re-homing on reconnect, no autonomous behaviour
// of any kind. Losing the link mid-cut must never slam a gate on a running tool.
//
// Build:  pio run -e xiao_c5           (PWM servo bank)
//         pio run -e xiao_c5_linear    (ST3215 slider)
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
#include "../utils/WakeButton.h"     // the button that lights it; ditto
#include "../motor/ServoSelfTest.h"   // and, held for a second, sweeps every servo

#if HAS_LINEAR
  #include "../motor/st3215/ST3215LinearDriver.h"
  #include "../feedback/LimitSwitchDistance.h"
  #include "../utils/MotionMath.h"
#endif

#if !HAS_SERVO && !HAS_LINEAR
  #error "dustgate_node needs an actuator: -DENABLE_SERVO with SERVO_PWM_PIN_1 for a PWM bank, or -DDUSTGATE_SERVO_BUS for the ST3215 slider"
#endif

static AsyncWebServer server(API_PORT);
static AsyncWebSocket nodeWs("/nodelink");

#if HAS_SERVO
static ServoActuator servos[SERVO_COUNT];
static const int SERVO_PINS[SERVO_COUNT] = {
    SERVO_PWM_PIN_1, SERVO_PWM_PIN_2, SERVO_PWM_PIN_3, SERVO_PWM_PIN_4
};
#endif

#if HAS_LINEAR
static ST3215LinearDriver  motor;
static LimitSwitchDistance feedback;

// ── The globals the shared linear headers expect ────────────────────────────
//
// utils/MotionMath.h and config.h declare these `extern` and the PRIMARY sketch
// defines them. LimitSwitchDistance is shared code, so linking it into the node
// means the node has to define them too.
//
// Only g_homeIsMaxEndstop is load-bearing here: which physical switch is the
// datum, a property of how THIS rail was installed. Homing direction is DERIVED
// from it (homeDirection() in config.h) rather than discovered — a serial bus
// servo cannot be wired backwards, so there is nothing to discover. The rest
// exist to satisfy the header: a node is
// sent absolute millimetres and never looks up a stop, so the stop table stays
// empty on purpose rather than being a copy of the primary's that could drift
// out of date without anyone noticing.
bool  g_homeIsMaxEndstop = false;
float g_stopPositionsMM[NUM_STOPS + 1] = { 0.0f };
int   g_numTrainedStops = 0;
uint8_t g_stopRoles[NUM_STOPS + 1] = { 0 };
float g_measuredStepsPerMM = 0.0f;
long  g_measuredSpanSteps  = 0;
char  g_manifoldModel[16]  = "";

// The sweep, as a state machine. See the CALIBRATION note at the top of the file
// for why it is one and not a blocking call.
enum HomingPhase : uint8_t {
    HOME_NEEDED,     // no datum yet — the boot state, and the state after a fault
    HOME_RUNNING,    // sweeping; feedback.updateHoming() is driving it
    HOME_DONE,       // datum set; moves are accepted
    HOME_FAILED      // the sweep ran its full length without finding a switch
};
static HomingPhase g_homing = HOME_NEEDED;
static uint32_t    g_homingStartedMs = 0;

// A node has no serial console to type `reset` into, so the retry the primary
// gets as a command it has to get on a timer. Same reason as the primary's:
// plugging USB into the board first and the servo lead in second is the ordinary
// bench order, and a bus servo can also be power-cycled independently of the
// board it hangs off — a node latched dead until someone walks over and resets
// it is a worse failure than a slow retry.
//
// 15s because a retry is a handful of bus transactions and a failure is quiet;
// fast enough that plugging the servo in feels like it just works, slow enough
// that a genuinely absent servo does not fill the log.
static const uint32_t kDriveRetryMs = 15000;
static uint32_t g_lastDriveRetryMs = 0;

// Sweep progress, watched by position rather than by isMoving() — see the
// backstop in updateSweep() for why that distinction is load-bearing now that a
// sweep is a train of chunks.
static long     g_homeLastPos    = 0;
static uint32_t g_homeLastMoveMs = 0;
static const uint32_t kHomeStallMs = 10000;

// Reaching the FAR switch while seeking the datum is a FAULT, not a clue. It
// used to mean "the motor is wired backwards" and cost a direction flip — a
// stepper problem (swapped coil pair) that a keyed serial-bus connector cannot
// have. What it means now is that the datum is configured to the wrong end, or
// the servo is not mounted the way HOME_DIRECTION_MOUNT describes; both need a
// person, and neither is fixed by driving the other way.

// A move that arrived before there was a datum to measure it from. ONE slot,
// most-recent-wins: while the carriage is homing the primary's routing has
// almost certainly moved on, and replaying a queue of stale positions would walk
// the gate through every tool that ran during the sweep.
static bool  g_deferredMove = false;
static float g_deferredMm   = 0.0f;
static char  g_deferredSel[48]   = "";
static char  g_deferredState[32] = "";
#endif

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

// Is this board's actuator in motion? The pixel goes orange on it, and the
// arrival STATE frame waits for it, so both personalities have to answer.
static bool actuatorMoving() {
#if HAS_SERVO
    for (int i = 0; i < SERVO_COUNT; i++) if (servos[i].isMoving()) return true;
#endif
#if HAS_LINEAR
    if (motor.isMoving()) return true;
#endif
    return false;
}

#if HAS_LINEAR
// ── The sweep ───────────────────────────────────────────────────────────────
//
// Ticked from loop(). LimitSwitchDistance owns the actual sequence — drive at
// the datum, stop on the switch, back off, zero — and this wrapper owns only
// what a NODE has to add: starting it, giving up on it, and noticing that the
// sweep reached the wrong end.
static void startSweep() {
    g_homing = HOME_RUNNING;
    g_homingStartedMs = millis();
    feedback.resetHoming();
    motor.setMaxSpeed(HOMING_SPEED_STEPS_PER_SEC);
    // The sweep itself is started by feedback.updateHoming() — see the contract
    // note in feedback/FeedbackSystem.h. Commanding it here would defeat the
    // release phase, because on this servo a new command cannot cancel one
    // already in flight.
    g_homeLastPos    = motor.getPosition();
    g_homeLastMoveMs = millis();
    Serial.println(F("[HOME] sweeping for the datum — the carriage will move."));
    Serial.println(F("       (if it is already on the switch it backs off that first)"));
}

// Retry a drive that never came up (or went away). Only ever runs while the node
// is not homed and not sweeping — a working, homed slider is never disturbed.
static void retryDriveIfNeeded() {
    if (g_homing == HOME_DONE || g_homing == HOME_RUNNING) return;
    if (motor.online() && g_homing == HOME_NEEDED) return;   // nothing wrong; the sweep is about to start

    uint32_t now = millis();
    if (now - g_lastDriveRetryMs < kDriveRetryMs) return;
    g_lastDriveRetryMs = now;

    if (motor.reconnect()) {
        Serial.println(F("[NODE] the servo is answering now — homing."));
        feedback.begin(&motor);
        g_homing = HOME_NEEDED;   // the tick below starts the sweep
    }
}

static void updateSweep() {
    if (g_homing != HOME_RUNNING) return;

    // The sweep gave up — an endstop that reads triggered and will not clear.
    // LimitSwitchDistance has already printed why; this makes it a state, so the
    // screen says "not homed" and moves are refused rather than run against a
    // datum that was never found.
    if (feedback.failed()) {
        g_homing = HOME_FAILED;
        Serial.print(F("[HOME] FAILED — "));
        Serial.println(feedback.failure());
        return;
    }

    if (feedback.updateHoming()) {
        motor.setMaxSpeed(MAX_SPEED_STEPS_PER_SEC);
        g_homing = HOME_DONE;
        Serial.println(F("[HOME] datum set. Position is now meaningful."));
        return;
    }

    // The far switch answering instead of the datum: the sweep is going the
    // wrong way, which is now a fault rather than something to correct for.
    // Asked through the feedback object rather than with a local digitalRead, so
    // there is exactly one statement in the build about which level means
    // "triggered" on a normally-closed switch.
    const bool farHit = g_homeIsMaxEndstop ? feedback.readHomeSwitch()
                                           : feedback.readMaxSwitch();
    if (farHit) {
        motor.stop();
        g_homing = HOME_FAILED;
        Serial.println(F("[HOME] FAILED — reached the FAR endstop while seeking the datum."));
        Serial.println(F("       The carriage drove away from home, not toward it. This node"));
        Serial.println(F("       has no direction to flip: a serial bus servo cannot be wired"));
        Serial.println(F("       backwards, and homing direction follows which endstop is the"));
        Serial.println(F("       datum. So either the datum is set to the wrong end, or the"));
        Serial.println(F("       servo is not mounted the way HOME_DIRECTION_MOUNT describes."));
        Serial.println(F("       An unplugged NC switch also reads triggered — check that first."));
        return;
    }

    // The backstop, and it watches POSITION rather than isMoving().
    //
    // isMoving() goes false between chunks — a sweep is a train of commands now,
    // not one long one — so a check on it fires mid-sweep and fails a perfectly
    // healthy home. And it goes false during the release phase, before the sweep
    // has even started. What actually distinguishes "working" from "stuck" is
    // whether the carriage is getting anywhere.
    const long pos = motor.getPosition();
    if (pos != g_homeLastPos) {
        g_homeLastPos    = pos;
        g_homeLastMoveMs = millis();
    } else if (millis() - g_homeLastMoveMs > kHomeStallMs) {
        g_homing = HOME_FAILED;
        Serial.println(F("[HOME] FAILED — the carriage has not moved for 10s and no switch"));
        Serial.println(F("       has been reached. Endstops, a jam, or a servo that is not"));
        Serial.println(F("       actually turning. `status` on a primary prints the mode."));
    }
}
#endif // HAS_LINEAR

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
        // Caps are what the board can PHYSICALLY drive, and the two are
        // mutually exclusive by design (PWM and serial never share a board), so
        // exactly one of these is non-zero.
        topo::nodelink::buildWelcome(reply.to<JsonObject>(), host.c_str(),
                                     BOARD_NAME, "1.0.0",
                                     HAS_SERVO ? SERVO_COUNT : 0,
                                     HAS_LINEAR ? 1 : 0,
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
        } else if (cmd.isServo && !HAS_SERVO) {
            // Refusing is the honest answer — the primary marks the gate
            // un-driveable rather than believing it moved.
            Serial.println(F("[SET] REFUSED — servo move, but this node drives a slider."));
            topo::nodelink::buildAck(reply.to<JsonObject>(), cmd.seq, false,
                                     "no servo bank on this node");
        } else if (!cmd.isServo && !HAS_LINEAR) {
            Serial.println(F("[SET] REFUSED — linear move, but this node drives PWM servos."));
            topo::nodelink::buildAck(reply.to<JsonObject>(), cmd.seq, false,
                                     "no linear actuator on this node");
        } else if (cmd.isServo && (cmd.channel < 0 || cmd.channel >= SERVO_COUNT)) {
            Serial.print(F("[SET] REFUSED — channel ")); Serial.print(cmd.channel);
            Serial.print(F(" out of range (this board has ")); Serial.print(SERVO_COUNT);
            Serial.println(F(")"));
            topo::nodelink::buildAck(reply.to<JsonObject>(), cmd.seq, false, "no such channel");
        } else {
            portENTER_CRITICAL(&cmdMux);
            cmdSlot    = cmd;
            cmdPending = true;
            portEXIT_CRITICAL(&cmdMux);
            // The line the bring-up actually needs: what arrived, and where it
            // is going to land.
            if (cmd.isServo) {
                Serial.print(F("[SET] Servo")); Serial.print(cmd.channel + 1);
                Serial.print(F(": ")); Serial.print(cmd.angle); Serial.print(F("deg"));
#if HAS_SERVO
                Serial.print(F("  (pin ")); Serial.print(SERVO_PINS[cmd.channel]);
                Serial.print(F(", "));
#else
                Serial.print(F("  ("));
#endif
            } else {
                Serial.print(F("[SET] Slider: ")); Serial.print(cmd.positionMm, 1);
                Serial.print(F("mm  ("));
            }
            Serial.print(cmd.selectorId);
            Serial.print(F(" -> ")); Serial.print(cmd.stateId);
            Serial.print(F(", seq ")); Serial.print(cmd.seq); Serial.println(F(")"));
            // ACK means ACCEPTED, not arrived; arrival is a separate STATE frame.
            // An unhomed slider still ACKs here — the move is genuinely accepted,
            // it is just queued behind the sweep (see the drain in loop()). The
            // alternative, a NACK, would make the primary mark a working gate
            // broken for the minute it takes to find the datum.
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
    wakebutton::begin();   // the button that wakes it after the two-minute blank
    // A node owns no collector, so there is nothing to dead-head and no query to
    // register — wakebutton treats an unset query as "not running", which is the
    // right answer here rather than a missing one.
#if HAS_SERVO
    servoselftest::begin(servos, SERVO_COUNT);
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

#if HAS_SERVO
    for (int i = 0; i < SERVO_COUNT; i++) servos[i].begin(SERVO_PINS[i]);
    bootTrace("servos");
#endif
#if HAS_LINEAR
    // Order matters: the endstops have to be readable before the sweep can look
    // at them, and the servo has to be in stepping mode with torque on before it
    // will move at all.
    if (!motor.begin()) {
        // A slider node with no servo on the bus is a node that can do nothing,
        // and it must not pretend otherwise — a WELCOME advertising linear:1 on
        // a board that cannot move would have the primary route tools to a gate
        // that never opens. It stays up so the screen and the log can say why.
        Serial.println(F("[NODE] ⚠ no ST3215 on the bus — this node cannot drive its gate."));
        g_homing = HOME_FAILED;
    }
    feedback.begin(&motor);
    bootTrace("slider");
#endif

    // Advertise for the primary's node picker (GET /api/nodes/discover).
    // The TXT record is what distinguishes a node from the primary's own
    // _http._tcp advert.
    if (MDNS.begin(WiFiProvisioner::getHostname().c_str())) {
        MDNS.addService("dustgate", "tcp", API_PORT);
        MDNS.addServiceTxt("dustgate", "tcp", "role",   "secondary");
        MDNS.addServiceTxt("dustgate", "tcp", "board",  BOARD_NAME);
        MDNS.addServiceTxt("dustgate", "tcp", "servos", String(HAS_SERVO ? SERVO_COUNT : 0).c_str());
        // So the primary's picker can tell a slider node from a servo bank
        // BEFORE pairing with it. The WELCOME's caps is still the authority;
        // this is the same kind of hint as `owner` below.
        MDNS.addServiceTxt("dustgate", "tcp", "linear", HAS_LINEAR ? "1" : "0");
        // Who owns this board, so a primary scanning the network can SAY that a
        // node is spoken for instead of listing it as free and only finding out
        // when the handshake is refused. Empty string = unclaimed.
        //
        // A HINT, not the authority. It is published once, here, from the claim
        // loaded at boot: a node claimed later in its life keeps advertising the
        // old value until it reboots. The refusal in the WELCOME frame is what
        // actually decides ownership (see THE CLAIM above), and it is always
        // current. Publishing a stale hint is safe in the direction that matters
        // — a board wrongly shown as free still refuses the pairing, which is the
        // pre-existing path with its own message.
        MDNS.addServiceTxt("dustgate", "tcp", "owner", g_owner.c_str());
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
// A panel has run on a C5 node (2026-08-22) and on a DevKitC primary
// (2026-08-21), and the C5's wake button lights it. Compiles to nothing on a
// board whose header names no screen pins.
// -----------------------------------------------------------------------------
static void updateStatusScreen() {
    if (!statusscreen::present()) return;

    statusscreen::Facts f;
    f.role   = statusscreen::Role::NODE;
#if HAS_SERVO
    f.selfTestCh    = servoselftest::channel();
    f.selfTestOf    = SERVO_COUNT;
    f.selfTestAngle = servoselftest::angle();
    if (!servoselftest::active()) f.selfTestRefused = servoselftest::refusal();
#endif
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

#if HAS_SERVO
    f.servoCount = SERVO_COUNT;
#endif
#if HAS_LINEAR
    // "not homed" is the line that matters here — it is the state in which the
    // node holds every move it is sent. See Facts::sliderHomed.
    f.sliderFitted = true;
    f.sliderHomed  = (g_homing == HOME_DONE);
    f.sliderMm     = motor.getPosition() / ST3215_COUNTS_PER_MM;
#endif
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
    statusled::setMoving(actuatorMoving());
    statusled::update();
    wakebutton::update();   // before the screen decides whether to be lit
#if HAS_SERVO
    servoselftest::update();
#endif
    updateStatusScreen();

#if HAS_SERVO
    // Advance sweeps and effect the deferred detach.
    for (int i = 0; i < SERVO_COUNT; i++) servos[i].update();
#endif
#if HAS_LINEAR
    // Poll the servo's countdown, and advance the sweep if one is running. Both
    // are cheap and both MUST be ticked from here: the watchdog is only petted
    // in loop(), so nothing below may block.
    motor.update();
    // A servo that arrived late, or came back after being unplugged. Checked
    // before the sweep so a successful retry starts homing on the same pass.
    retryDriveIfNeeded();
    if (g_homing == HOME_NEEDED && motor.online()) startSweep();
    updateSweep();
#endif

    // Drain a pending SET. Servos are only ever touched from here.
    bool have = false;
    topo::nodelink::SetCommand cmd;
    portENTER_CRITICAL(&cmdMux);
    if (cmdPending) { cmd = cmdSlot; cmdPending = false; have = true; }
    portEXIT_CRITICAL(&cmdMux);

    if (have) {
        bool commanded = false;

#if HAS_SERVO
        if (cmd.isServo) {
            servos[cmd.channel].setHoldAtRest(cmd.holdAtRest);
            servos[cmd.channel].moveTo(cmd.angle);
            // Distinct from the [SET] line above: that one says a frame ARRIVED,
            // this says the PWM was actually commanded. If you see [SET] and
            // never [MOVE], the frame is being dropped between the socket task
            // and the loop.
            Serial.print(F("[MOVE] Servo")); Serial.print(cmd.channel + 1);
            Serial.print(F(" -> ")); Serial.print(cmd.angle); Serial.println(F("deg"));
            commanded = true;
        }
#endif
#if HAS_LINEAR
        if (!cmd.isServo) {
            if (g_homing == HOME_DONE) {
                motor.moveTo((long)(cmd.positionMm * ST3215_COUNTS_PER_MM * -HOME_DIRECTION));
                Serial.print(F("[MOVE] Slider -> ")); Serial.print(cmd.positionMm, 1);
                Serial.println(F("mm"));
                commanded = true;
            } else if (g_homing == HOME_FAILED) {
                // Nothing to defer it to. Say so every time rather than dropping
                // it silently: a gate that never moves and never complains is
                // the worst thing this node could do.
                Serial.print(F("[MOVE] REFUSED — no datum (homing failed). "));
                Serial.print(cmd.positionMm, 1); Serial.println(F("mm discarded."));
            } else {
                // Still sweeping. Hold the LATEST position and run it when the
                // datum lands — see g_deferredMove for why only the latest.
                g_deferredMove = true;
                g_deferredMm   = cmd.positionMm;
                topo::nodelink::strlcpy_(g_deferredSel,   cmd.selectorId, sizeof(g_deferredSel));
                topo::nodelink::strlcpy_(g_deferredState, cmd.stateId,    sizeof(g_deferredState));
                Serial.print(F("[MOVE] deferred until the sweep finishes: "));
                Serial.print(cmd.positionMm, 1); Serial.println(F("mm"));
            }
        }
#endif

        if (commanded) {
            topo::nodelink::strlcpy_(pendingSel,   cmd.selectorId, sizeof(pendingSel));
            topo::nodelink::strlcpy_(pendingState, cmd.stateId,    sizeof(pendingState));
            awaitingSettle = true;
            statusled::flashActivity();   // visible confirmation at the gate itself
            g_lastCmdMs = millis();
            statusscreen::note();
            reportState(pendingSel, pendingState, true);
        }
    }

#if HAS_LINEAR
    // The datum has just landed and something was waiting on it.
    if (g_deferredMove && g_homing == HOME_DONE) {
        g_deferredMove = false;
        motor.moveTo((long)(g_deferredMm * ST3215_COUNTS_PER_MM * -HOME_DIRECTION));
        Serial.print(F("[MOVE] Slider -> ")); Serial.print(g_deferredMm, 1);
        Serial.println(F("mm (deferred through the sweep)"));
        topo::nodelink::strlcpy_(pendingSel,   g_deferredSel,   sizeof(pendingSel));
        topo::nodelink::strlcpy_(pendingState, g_deferredState, sizeof(pendingState));
        awaitingSettle = true;
        statusled::flashActivity();
        g_lastCmdMs = millis();
        statusscreen::note();
        reportState(pendingSel, pendingState, true);
    }
#endif

    if (awaitingSettle && !actuatorMoving()) {
        awaitingSettle = false;
        reportState(pendingSel, pendingState, false);
    }
}

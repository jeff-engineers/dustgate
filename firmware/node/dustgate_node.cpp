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
// WHEN IT HOMES: ON DEMAND, NEVER AT BOOT (changed 2026-09-03).
//
// A step-counting servo has no datum of its own and comes back from a power
// cycle holding nothing (torque off), so a node that has just restarted does not
// know where its carriage is. It has to sweep before it can answer a SET
// honestly — the only question is WHEN.
//
// It used to sweep at boot, unasked. That was wrong in the way an unasked
// movement is always wrong: a board that is powered up on a bench, or that
// brown-outs and reboots while someone has their hands near the rack, drives its
// carriage the length of the rail with nobody having asked for anything. The
// note that used to be here reasoned that the alternative was refusing every SET
// until a human walked over, and that refusing was worse. That was a false
// choice: the deferred-move slot below already existed, so the third option —
// sweep WHEN FIRST ASKED, and hold the asking move until the datum lands — costs
// one flag and refuses nothing.
//
// So the sweep starts on either of:
//   * the first SET that needs a datum (the primary routing a tool here), or
//   * (REMOVED 2026-09-17) the wake button held for a second. A gesture nobody
//     knew about, that moved a carriage the length of a rail. Its one real job —
//     clearing HOME_FAILED, which nothing else did — moved to the SET path, so a
//     failed sweep is retried when a gate is actually asked for.
//
// The cost, stated plainly: the FIRST gate selection after a reboot pays for a
// full sweep before the gate moves, which is seconds, not milliseconds. That is
// the right place to spend it — it is a move someone asked for, at a moment they
// are watching, instead of a move nobody asked for at a moment nobody is.
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
#include "../utils/ResetReason.h" // `rst` in the WELCOME — why this node last booted
#include "../utils/BuildStamp.h"  // `fw` in the WELCOME, and the boot banner
#include <ESPmDNS.h>
#include <esp_heap_caps.h>        // bootTrace() — internal-DRAM headroom at each stage
#include "../utils/Watchdog.h"
#include "../utils/OtaGuard.h"        // a fresh OTA image is on probation until it has proved itself
#include <HTTPClient.h>
#include <Update.h>
#include "../outlets/TasmotaOutlet.h"      // plugs this board polls for the primary
#include "../outlets/ShellyGen2Outlet.h"

#ifndef DEBUG_PRINT
  #define DEBUG_PRINT(x)   Serial.print(x)     // RfCollectorPresser.h reports an RMT failure with it
  #define DEBUG_PRINTLN(x) Serial.println(x)
#endif
#include "../control/RfCollectorPresser.h"     // the collector's remote, keyed on this board's pad (PRESS)
#include "../motor/ServoActuator.h"
#include "../control/NodeLink.h"
#include "../utils/BinSensor.h"           // the dust-bin beam's debounce (pure)
#include "BrainLink.h"           // this node dialling its own primary
#include "../utils/StatusLed.h"
#include "../utils/StatusScreen.h"   // optional SSD1306; nothing on a board without one
#include "../utils/WakeButton.h"     // the button that lights it; ditto
#include "../motor/ServoSelfTest.h"   // and, held for a second, sweeps every servo
#ifdef PIN_CT
#include "../sensing/CtSensor.h"      // a clamp, on a board that watches a tool
#include "../sensing/CtTrip.h"        // ...and the one shared answer to "is it running?"
#include "../sensing/ClampJack.h"     // ...and whether one is plugged in at all
#include <driver/gpio.h>              // gpio_pullup_en: D0's pull-up, so an empty pad reads as no clamp
#endif

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

// ONE PRIMARY, SO ONE CLIENT SLOT'S WORTH OF PATIENCE — capped 2026-09-18.
//
// A node belongs to exactly one primary (the claim, below), so it has no use for
// the library's default client pool. Leaving it at the default is what turned a
// misbehaving client into an unrecoverable board: the primary's reconnect had no
// backoff and arrived once a second, ESPAsyncWebServer does not reap dead clients
// on its own, and once the pool filled the node RESET every incoming connection
// — errno 104 at the other end — while still answering mDNS and HTTP perfectly.
// Only power-cycling the NODE cleared it, because the primary was the thing
// filling it.
//
// The primary's backoff (control/RemoteActuatorBus.cpp) is the real fix. This is
// the defensive half: a node should survive ANY client that reconnects too fast,
// including a laptop with a WebSocket console open, not just our own.
//
// ⚠️ NOT USED AS A CLEANUP LIMIT — and the reason is worth keeping.
//
// The first version of this passed the cap to cleanupClients(), which was a bug
// I nearly shipped. That function closes the OLDEST client when the count
// exceeds the limit, and during an ordinary reconnect a node briefly holds two:
// the dead socket not yet reaped, and the new one. At a cap of 2 a third arrival
// evicts the oldest — which can be the ESTABLISHED LINK. Tightening the pool to
// stop disconnects would have caused them.
//
// The library default (8) has the slack this needs, and the real fix is on the
// other side anyway: the primary now backs off instead of reconnecting once a
// second (control/RemoteActuatorBus.cpp, _backoff), so the pool never fills.
//
// Kept as the threshold for the WARNING below, which costs nothing and names the
// condition if it ever recurs: a node has ONE primary, so more than a couple of
// sockets on /nodelink means something is reconnecting in a loop.
static const size_t kMaxLinkClients = 2;

#if HAS_SERVO
static ServoActuator servos[SERVO_COUNT];
static const int SERVO_PINS[SERVO_COUNT] = {
    SERVO_PWM_PIN_LIST
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

// Has anything ASKED for a datum yet? HOME_NEEDED alone is not enough to start a
// sweep — that is the whole of the on-demand rule (see the note at the top of
// this file). Set by the first SET that needs a datum, and by the button hold.
//
// It STAYS set once asked, which is what makes the drive retry below do the
// right thing: a servo that was missing at boot and gets plugged in later
// completes the request that is already outstanding, rather than starting a
// sweep nobody is waiting for.
static bool g_homeAsked = false;

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

// The OTHER backstop: a sweep that keeps moving and never arrives.
//
// kHomeStallMs above watches POSITION, so it catches a carriage that has
// stopped — a jam, a dead servo, a bus that went quiet. It cannot see the
// opposite failure: a sweep travelling perfectly well toward a switch that will
// never fire, because the endstop is unplugged at the far end, the rail is
// longer than anyone declared, or the pinion is turning without the rack. In
// that case position changes every pass and the stall check never trips, right
// up until the runaway guard ends the move — and on a long rack that is minutes.
//
// HOMING_TIMEOUT_MS is derived in config.h from the runaway guard, the sweep
// speed and the bench tracking factor, so changing the rail length or the speed
// moves this with them. ~162 s on the current numbers, which is sized for the
// longest rack the design admits: 8 gates on the 4" manifold, 891 mm.
static_assert(HOMING_TIMEOUT_MS > kHomeStallMs,
              "the overall homing timeout must outlast the stall check, or the "
              "stall check can never be the thing that reports");

// And the primary must outlast BOTH, or it calls the move lost while the node is
// still legitimately sweeping — and the node's far more specific diagnosis never
// reaches anyone. This is the only translation unit that sees both numbers;
// NodeLink.h is pure and does not include config.h.
static_assert(topo::nodelink::kMoveTimeoutMs > HOMING_TIMEOUT_MS,
              "kMoveTimeoutMs (control/NodeLink.h) must exceed HOMING_TIMEOUT_MS "
              "(config.h) — see the note above kMoveTimeoutMs");

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
// Did the owner reach us over a socket WE dialled (brainlink) or one it dialled
// (nodeWs)? Decides where unsolicited frames go, and which side keeps a link up.
static volatile bool     g_ownerOutbound = false;
static const uint32_t    kOutboundId     = 0x7FFF0001u;   // never a real AsyncWebSocket client id

// Where a frame the node starts on its own (SENSE, STATE, OTASTATE) is sent: the
// owner. On our own outbound socket that is the queue brainlink writes from; on an
// inbound one it is the listener's sockets, as it always was.
static void sendToOwner(const String& s) {
    if (g_ownerOutbound) brainlink::send(s.c_str());
    else                 nodeWs.textAll(s);
}

// One link, whichever way it was dialled. The frame handler is written against this
// so it does not care who opened the socket.
struct Conn {
    uint32_t               id;
    IPAddress              remote;
    bool                   outbound;
    AsyncWebSocketClient*  c;      // null when outbound
};
static void connSend(const Conn& conn, const String& s) {
    if (conn.outbound) brainlink::send(s.c_str());
    else if (conn.c)   conn.c->text(s);
}
static void connClose(const Conn& conn) {
    if (!conn.outbound && conn.c) conn.c->close();   // an outbound socket is closed by brainlink when it drops
}

#ifdef PIN_CT
// ── CT tool sensing (tool-sensing RFC §5.6) ─────────────────────────────────
//
// THIS BOARD DECIDES THE BIT. Not a shortcut — §5.4b: a CT measures current,
// watts need a voltage and a power factor it cannot give, and a woodworking
// tool's standby sits under the noise floor, so there is no threshold worth
// putting on the wire. It is also the only arrangement with usable latency,
// since mains-frequency RMS cannot round-trip per sample over WiFi.
//
// The primary tells us WHAT IS WIRED (CONFIG) and we tell it WHAT IT READS
// (SENSE). Nothing here interprets the document — the invariant at the top of
// nodelink.js — because `sensorId` is opaque and only ever echoed back.
static CtSensor g_ct(PIN_CT);
static topo::nodelink::SensorSpec g_sensors[topo::nodelink::kMaxSensorsPerNode];
static size_t   g_sensorCount = 0;
static bool     g_senseOn[topo::nodelink::kMaxSensorsPerNode] = { false };
static bool     g_senseKnown = false;

// What this board was TOLD to squeeze by, as opposed to what it was built with.
//
// ONE CLAMP, ONE PAD — the same assumption tickSensors() already makes — so this
// is per-BOARD rather than per-sensor, taken from the first spec in the CONFIG.
// When a second pad exists it moves into g_sensors[] alongside the channel, and
// this becomes an array; nothing else about the shape changes.
//
// Starts at the compiled-in defaults, which is what a board uses until a primary
// tells it otherwise — including forever, if that primary predates 2026-09-17.
static sensing::TripParams g_tripParams;
static uint32_t g_lastSenseMs = 0;

// The sampling cadence, the floor and the trip point all live in
// sensing/CtTrip.h — ONE copy, shared with the primary, which needs the identical
// decision for a clamp wired to the brain's own board. Read that file before
// changing any of it; the two provisional numbers are in there with their
// reasoning.
static sensing::CtTrip g_ctTrip;

// Is a clamp plugged into the jack — reported as CLAMP whether or not a CONFIG names one (sensing/ClampJack.h).
static sensing::ClampJack g_jack;
static uint32_t g_lastClampMs = 0;
static bool     g_clampResend = false;   // a new link: say it now rather than at the next repeat
#endif

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
    if (g_homing == HOME_RUNNING) return;
    // A DRIVE THAT IS ANSWERING NEEDS NOTHING FROM HERE, whatever the homing
    // state is. This used to read `motor.online() && g_homing == HOME_NEEDED`,
    // which let HOME_FAILED with a perfectly healthy servo fall through to the
    // reconnect below — every 15 seconds, forever.
    //
    // Both things that did were bad. reconnect() sets HOME_NEEDED, and
    // g_homeAsked stays set by design, so loop() started another sweep on the
    // next tick: the "reached the FAR endstop" fault, whose own message says it
    // needs a person and is not fixed by driving the other way, drove the
    // carriage at the far end four times a minute. And reconnect() goes through
    // begin(), which writes the mode and both angle limits to EEPROM — three
    // cell writes every 15s, ~17k a day, against a part with a finite number of
    // them.
    //
    // A failed home is now terminal until someone asks again, which is what the
    // fault message always claimed. The asking is the button hold in setup().
    if (motor.online()) return;

    uint32_t now = millis();
    if (now - g_lastDriveRetryMs < kDriveRetryMs) return;
    g_lastDriveRetryMs = now;

    if (motor.reconnect()) {
        // NOT "— homing": whether a sweep follows depends on whether anything
        // asked for one. A servo plugged in on a quiet bench goes back to ready
        // and sits still; one plugged in with a move already waiting sweeps on
        // the next tick and then runs it.
        Serial.print(F("[NODE] the servo is answering now"));
        Serial.println(g_homeAsked ? F(" — homing, a move is waiting.")
                                   : F(" — idle until something asks for a gate."));
        feedback.begin(&motor);
        g_homing = HOME_NEEDED;
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
        return;
    }

    // Still moving, still nowhere. The stall check above cannot see this one —
    // see the note on HOMING_TIMEOUT_MS. Stop the carriage before saying so: it
    // is by definition still travelling, and the whole reason we are here is
    // that nothing else is going to end the move soon.
    if (millis() - g_homingStartedMs > HOMING_TIMEOUT_MS) {
        motor.stop();
        g_homing = HOME_FAILED;
        Serial.print(F("[HOME] FAILED — no datum after "));
        Serial.print(HOMING_TIMEOUT_MS / 1000);
        Serial.println(F("s. The carriage kept MOVING the whole time, so this is"));
        Serial.println(F("       not a jam: the datum switch never fired. An unplugged NC"));
        Serial.println(F("       switch reads triggered rather than silent, so suspect the"));
        Serial.println(F("       far end — a rail longer than the manifold declares, or a"));
        Serial.println(F("       pinion turning without the rack."));
    }
}
#endif // HAS_LINEAR

// -----------------------------------------------------------------------------
// PLUG POLLING — this board polls the smart plugs of the tools it controls, on the
// primary's behalf (2026-10-03). The primary stops carrying a poll load that grew
// with the shop; a plug is read by the board that already owns the tool's gate.
//
// WHO POLLS WHAT is decided by the primary (plugOwnerOf in control/Shop.h), sent as
// CONFIG sensors of kind "plug". This board interprets nothing: it holds an address,
// a protocol and a threshold, and reports watts. The threshold is applied here only
// so SENSE can go out on CHANGE instead of on every poll.
//
// A poll blocks for up to OUTLET_HTTP_TIMEOUT_MS, so it runs on its OWN task —
// loop() must keep ticking servos and the clamp's 60 Hz window. The task owns the
// outlet objects; the AsyncTCP task only hands it a new list (and a generation
// number to notice it by); loop() only reads results and sends frames.
// -----------------------------------------------------------------------------
struct PlugWatch {
    char  id[topo::nodelink::kMaxSensorIdLen] = {0};
    char  ip[topo::nodelink::kMaxPlugIpLen + 1] = {0};
    bool  tasmota = false;
    float thresholdW = 0.0f;
    // Written by the poll task, read by loop(). A torn read of a float costs one
    // slightly wrong reading, repeated within a poll interval — not worth a lock.
    volatile float    watts = 0.0f;
    volatile bool     reachable = false;
    volatile uint32_t atMs = 0;
    // loop()'s own bookkeeping of what it last SAID.
    bool  sentOn = false, sentFault = false, sentKnown = false;
    float sentWatts = 0.0f;
    uint32_t sentAtMs = 0;
};
static PlugWatch        g_plugs[topo::nodelink::kMaxSensorsPerNode];
static volatile size_t  g_plugCount = 0;
static volatile uint32_t g_plugCfgGen = 0;
static portMUX_TYPE     g_plugMux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t     g_plugTask = nullptr;

// Called from the AsyncTCP task with a validated list. The poll task rebuilds its
// outlets on its next pass; results for a plug that survives the change are kept
// so a re-sent identical CONFIG (every reconnect) does not blank the readings.
static void plugApplyConfig(const topo::nodelink::SensorSpec* specs, size_t n) {
    portENTER_CRITICAL(&g_plugMux);
    PlugWatch next[topo::nodelink::kMaxSensorsPerNode];
    for (size_t i = 0; i < n; i++) {
        topo::nodelink::strlcpy_(next[i].id, specs[i].sensorId, sizeof(next[i].id));
        topo::nodelink::strlcpy_(next[i].ip, specs[i].ip, sizeof(next[i].ip));
        next[i].tasmota = specs[i].plugTasmota;
        next[i].thresholdW = specs[i].thresholdW;
        for (size_t j = 0; j < g_plugCount; j++) {
            if (strcmp(g_plugs[j].id, next[i].id) == 0 && strcmp(g_plugs[j].ip, next[i].ip) == 0) {
                next[i].watts = g_plugs[j].watts; next[i].reachable = g_plugs[j].reachable; next[i].atMs = g_plugs[j].atMs;
                break;
            }
        }
        // sent* stay false: the primary has just re-said what it wants, so tell
        // it what is true now rather than waiting out a repeat interval.
    }
    for (size_t i = 0; i < n; i++) {
        g_plugs[i].~PlugWatch();
        new (&g_plugs[i]) PlugWatch();
        topo::nodelink::strlcpy_(g_plugs[i].id, next[i].id, sizeof(g_plugs[i].id));
        topo::nodelink::strlcpy_(g_plugs[i].ip, next[i].ip, sizeof(g_plugs[i].ip));
        g_plugs[i].tasmota = next[i].tasmota;
        g_plugs[i].thresholdW = next[i].thresholdW;
        g_plugs[i].watts = next[i].watts; g_plugs[i].reachable = next[i].reachable; g_plugs[i].atMs = next[i].atMs;
    }
    g_plugCount = n;
    g_plugCfgGen = g_plugCfgGen + 1;
    portEXIT_CRITICAL(&g_plugMux);
    if (n) Serial.printf("[PLUG] polling %u plug(s) for the primary\n", (unsigned)n);
    else   Serial.println(F("[PLUG] nothing to poll"));
}

static void plugTaskFn(void*);
static BaseType_t g_plugTaskRc = 0;       // what xTaskCreate answered, for /api/plugs
// Created from setup() and retried from loop(): it needs one contiguous 10 KB block, and a node that
// boots with its heap already fragmented (a fresh OTA, WiFi up, the brainlink task holding its own 10 KB)
// would otherwise have NO plug poller for the whole of its uptime, silently — a plug nobody polls reads
// as unreachable and nothing says the poller never started.
static void startPlugTask() {
    if (g_plugTask) return;
    g_plugTaskRc = xTaskCreate(plugTaskFn, "plugpoll", 10240, nullptr, 1, &g_plugTask);
    if (g_plugTaskRc != pdPASS) { g_plugTask = nullptr; Serial.println(F("[PLUG] could not start the plug poller (no memory) - will retry")); }
}

static void plugTaskFn(void*) {
    SmartOutlet* outlets[topo::nodelink::kMaxSensorsPerNode] = {nullptr};
    uint32_t seenGen = 0xFFFFFFFFu;
    for (;;) {
        if (seenGen != g_plugCfgGen) {
            seenGen = g_plugCfgGen;
            for (auto& o : outlets) { delete o; o = nullptr; }
            // Copy the list out under the lock, allocate OUTSIDE it: a malloc in a
            // critical section can block on the heap lock with interrupts off.
            char ips[topo::nodelink::kMaxSensorsPerNode][topo::nodelink::kMaxPlugIpLen + 1];
            char ids[topo::nodelink::kMaxSensorsPerNode][topo::nodelink::kMaxSensorIdLen];
            bool tas[topo::nodelink::kMaxSensorsPerNode];
            size_t cnt;
            portENTER_CRITICAL(&g_plugMux);
            cnt = g_plugCount;
            for (size_t i = 0; i < cnt; i++) {
                memcpy(ips[i], g_plugs[i].ip, sizeof(ips[i]));
                memcpy(ids[i], g_plugs[i].id, sizeof(ids[i]));
                tas[i] = g_plugs[i].tasmota;
            }
            portEXIT_CRITICAL(&g_plugMux);
            for (size_t i = 0; i < cnt; i++) {
                outlets[i] = tas[i]
                    ? static_cast<SmartOutlet*>(new TasmotaOutlet(ips[i], ids[i]))
                    : static_cast<SmartOutlet*>(new ShellyGen2Outlet(ips[i], ids[i]));
            }
        }
        for (size_t i = 0; i < topo::nodelink::kMaxSensorsPerNode; i++) {
            if (!outlets[i] || seenGen != g_plugCfgGen) continue;
            const bool ok = outlets[i]->poll();
            g_plugs[i].reachable = ok;
            g_plugs[i].watts = ok ? outlets[i]->getPowerW() : 0.0f;
            g_plugs[i].atMs = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(OUTLET_POLL_INTERVAL_MS));
    }
}

// loop(): turn readings into SENSE frames — on CHANGE (the on bit, reachability, or
// a real swing in watts) and again every kSenseRepeatMs, so one dropped frame cannot
// leave the primary wrong and so a plug's wattage is never stale to the UI for long.
static void tickPlugs() {
    const size_t n = g_plugCount;
    const uint32_t now = millis();
    for (size_t i = 0; i < n; i++) {
        PlugWatch& p = g_plugs[i];
        if (!p.atMs) continue;                       // not polled yet
        const bool fault = !p.reachable;
        const float w = p.watts;
        const bool on = !fault && w >= p.thresholdW;
        const float swing = fabsf(w - p.sentWatts);
        const bool changed = !p.sentKnown || on != p.sentOn || fault != p.sentFault ||
                             swing >= (p.sentWatts > 50.0f ? p.sentWatts * 0.2f : 10.0f);
        const bool due = (uint32_t)(now - p.sentAtMs) >= topo::nodelink::kSenseRepeatMs;
        if (!changed && !due) continue;
        StaticJsonDocument<256> doc;
        topo::nodelink::buildSense(doc.to<JsonObject>(), p.id, on, -1.0f, -1.0f, -1.0f, -1.0f,
                                   fault, fault ? -1.0f : w, /*plug=*/true);
        String s; serializeJson(doc, s);
        sendToOwner(s);
        if (!p.sentKnown || on != p.sentOn || fault != p.sentFault) {
            Serial.printf("[PLUG] %s %s %.1f W\n", p.id, fault ? "UNREACHABLE" : (on ? "ON " : "off"), w);
        }
        p.sentOn = on; p.sentFault = fault; p.sentKnown = true; p.sentWatts = w; p.sentAtMs = now;
    }
}

// -----------------------------------------------------------------------------
// The collector's jobs on a node (2026-10-04): a dust-bin beam and the remote's
// transmitter. Both were primary-only, which is why a board at the collector had to be
// the brain. The node does the PRIMITIVE and nothing more: it reads the beam and
// reports a debounced bit (SENSE, `on` = FULL), and it keys the transmitter when told
// (PRESS). When to press, whether it worked and when to try again stay the primary's
// policy, because only the primary reads the plug that says whether the blower agreed.
// -----------------------------------------------------------------------------
#if HAS_BIN
static char                  g_binId[topo::nodelink::kMaxSensorIdLen] = "";
static bool                  g_binInvert = true;
static bool                  g_binActive = false;
static topo::BinDebounce     g_binDeb;
static bool                  g_binSentKnown = false;
static bool                  g_binSentOn    = false;
static uint32_t              g_binSentAt    = 0;
#endif

static void binApplyConfig(const topo::nodelink::SensorSpec* bins, size_t n) {
#if HAS_BIN
    if (n) {
        topo::nodelink::strlcpy_(g_binId, bins[0].sensorId, sizeof(g_binId));
        g_binInvert = bins[0].binInvert;
    }
    g_binActive    = n > 0;
    g_binSentKnown = false;           // say it again now: the primary just asked
    Serial.printf("[BIN] %s\n", n ? g_binId : "(not watching)");
#else
    (void)bins; (void)n;
#endif
}

static void tickBin() {
#if HAS_BIN
    if (!g_binActive) return;
    const uint32_t now = millis();
    const bool raw = (digitalRead(PIN_BIN_SENSOR) == LOW);       // LOW = the optocoupler's "full"
    const bool wasFull = g_binDeb.full();
    g_binDeb.sample(g_binInvert ? raw : !raw, now);
    const bool full = g_binDeb.full();
    if (full != wasFull) Serial.printf("[BIN] %s\n", full ? "FULL" : "ok");
    const bool changed = !g_binSentKnown || full != g_binSentOn;
    const bool due     = (uint32_t)(now - g_binSentAt) >= topo::nodelink::kSenseRepeatMs;
    if (!changed && !due) return;
    StaticJsonDocument<192> doc;
    topo::nodelink::buildSense(doc.to<JsonObject>(), g_binId, full);
    String s; serializeJson(doc, s);
    sendToOwner(s);
    g_binSentKnown = true; g_binSentOn = full; g_binSentAt = now;
#endif
}

#if HAS_RF
static topo::nodelink::PressOrder g_pressOrder;
static volatile bool              g_pressPending = false;
static RfCollectorPresser*        g_rfPresser    = nullptr;   // ONE for the pad's lifetime — see configure()
#endif

// loop(): key the transmitter for a pending PRESS and say how it went. The handler only
// records the order — it runs on a network task, and the press blocks ~0.5 s of RMT, which
// is acceptable here for the reason it is on the primary: it happens on a state change,
// never on a tick, and the watchdog is petted either side.
static void runPress() {
#if HAS_RF
    if (!g_pressPending) return;
    const topo::nodelink::PressOrder o = g_pressOrder;
    g_pressPending = false;
    if (!g_rfPresser) g_rfPresser = new RfCollectorPresser(PIN_RF_TX);
    g_rfPresser->configure(o.address, o.data, o.tickUs, (uint16_t)o.repeats);
    watchdog::pet();
    const bool ok = g_rfPresser->press();
    watchdog::pet();
    Serial.printf("[RF] press addr=%u data=%u tick=%luus x%lu -> %s\n", (unsigned)o.address, (unsigned)o.data,
                  (unsigned long)o.tickUs, (unsigned long)o.repeats, ok ? "sent" : "TRANSMIT FAILED");
    StaticJsonDocument<128> d;
    topo::nodelink::buildAck(d.to<JsonObject>(), o.seq, ok, ok ? nullptr : "the transmitter could not send");
    String s; serializeJson(d, s);
    sendToOwner(s);
#endif
}

// -----------------------------------------------------------------------------
// OTA — the primary tells this node to pull a new image (2026-10-03).
//
// The frame handler only RECORDS the order (it runs on the AsyncTCP task, which
// must stay free to answer PINGs and which has a small stack); loop() does the
// download. loop() therefore blocks for the ~15 s a 1.3 MB image takes — safe
// here because a node holds every gate while it is away (the same fail-safe as a
// dropped link), and because the order is REFUSED while anything is moving. The
// watchdog is petted from the progress callback so the wait is not mistaken for
// a hang.
//
// A bad image cannot strand the board: the new slot only becomes bootable after
// its MD5 checks out, and it then runs on probation (utils/OtaGuard.h) — rolled
// back by the bootloader if it resets, and by tick() if it never reaches the
// primary. What it CANNOT recover is an image that boots, reaches the primary and
// then misbehaves; that one needs the cable, which is why updates are manual.
// -----------------------------------------------------------------------------
static topo::nodelink::OtaOrder g_otaOrder;
static IPAddress                g_otaFrom;
static volatile bool            g_otaPending = false;
static bool                     g_otaRunning = false;

static void sendOtaState(const char* state, int pct = -1, const char* err = nullptr) {
    StaticJsonDocument<256> d;
    topo::nodelink::buildOtaState(d.to<JsonObject>(), state, pct, err);
    String s; serializeJson(d, s);
    sendToOwner(s);
}

static void failOta(const char* why) {
    Serial.print(F("[OTA] FAILED — ")); Serial.println(why);
    sendOtaState("fail", -1, why);
    g_otaRunning = false;
}

static void runOta() {
    g_otaPending = false;
    g_otaRunning = true;
    const String url = String("http://") + g_otaFrom.toString() + g_otaOrder.path;
    Serial.print(F("[OTA] pulling ")); Serial.print(url);
    Serial.print(F(" (")); Serial.print(g_otaOrder.size); Serial.println(F(" bytes)"));
    sendOtaState("start", 0);

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(15000);
    if (!http.begin(client, url)) { failOta("could not open the download"); return; }
    const int code = http.GET();
    if (code != 200) {
        static char why[40];
        snprintf(why, sizeof(why), "primary answered %d", code);
        http.end(); failOta(why); return;
    }
    if ((uint32_t)http.getSize() != g_otaOrder.size) {
        http.end(); failOta("image is not the size the primary announced"); return;
    }
    if (!Update.begin(g_otaOrder.size, U_FLASH)) {
        http.end(); failOta("no room in the update slot"); return;
    }
    Update.setMD5(g_otaOrder.md5);
    // OUR OWN READ LOOP, not Update.writeStream(). That call blocks inside the
    // library whenever the data stops, so loop() cannot feed the watchdog and a
    // brain that vanishes mid-download resets the whole node (seen on hardware
    // 2026-10-04: task_wdt 35 s after the pull began). Here the watchdog is fed on
    // every pass, and a transfer that makes no progress for kStallMs is simply a
    // failed update — the node stays on its current image, never reboots, and the
    // half-written slot is abandoned (it is never marked bootable until end()).
    static const uint32_t kStallMs = 8000;
    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    uint32_t got = 0, lastDataMs = millis();
    int lastPct = -10;
    while (got < g_otaOrder.size) {
        watchdog::pet();
        const int avail = stream->available();
        if (avail > 0) {
            const int want = avail < (int)sizeof(buf) ? avail : (int)sizeof(buf);
            const int n = stream->readBytes(buf, want);
            if (n > 0) {
                if (Update.write(buf, n) != (size_t)n) { Update.abort(); http.end(); failOta("flash write failed"); return; }
                got += n;
                lastDataMs = millis();
                const int pct = (int)((uint64_t)got * 100 / g_otaOrder.size);
                if (pct >= lastPct + 10) { lastPct = pct; sendOtaState("progress", pct); }
                continue;
            }
        }
        if (!stream->connected() && stream->available() == 0) break;      // the far end closed on us
        if (millis() - lastDataMs > kStallMs) break;                       // nothing for 8 s
        delay(5);
    }
    http.end();
    if (got != g_otaOrder.size) {
        Update.abort();
        failOta(got ? "download stalled or was cut off" : "primary sent nothing");
        return;
    }
    if (!Update.end(true)) {
        failOta("image rejected (checksum or format)"); return;
    }
    Serial.println(F("[OTA] image written and verified — rebooting into it"));
    sendOtaState("done", 100);
    // Let the frame leave before the radio goes away.
    const unsigned long until = millis() + 600;
    while (millis() < until) { watchdog::pet(); delay(20); }
    ESP.restart();
}

// -----------------------------------------------------------------------------
// NodeLink frame handling (AsyncTCP task — never touches a servo directly)
// -----------------------------------------------------------------------------
static void handleNodeFrame(const Conn& conn, const uint8_t* data, size_t len) {
    // 1024: a CONFIG with four sensors is ~35 members, and this document is the
    // WHOLE frame. ArduinoJson fails a deserialize that does not fit (NoMemory),
    // so a 384 here would have made a full CONFIG read as "no frame" — silence.
    StaticJsonDocument<1024> doc;
    if (deserializeJson(doc, data, len)) return;
    JsonObjectConst f = doc.as<JsonObjectConst>();
    const char* t = f["t"].as<const char*>();
    if (!t) return;

    // 512, not 256, and the extra is not slack. A WELCOME carrying nodeId,
    // board, fw, a claimedBy and a three-member `caps` lands right on 256 — and
    // ArduinoJson does not fail an overflow, it SILENTLY DROPS the member being
    // added. `caps.ct` is written last, so the symptom was a board that reported
    // every capability except the clamp, and a tray that never offered one.
    // Caught by test_nodebus.cpp, 2026-09-15; it would have read as a wiring
    // fault on the bench. 384 → 512 on 2026-09-27 when boot info (upS, rst)
    // joined the WELCOME: at 384 the fullest one dropped `rst`, and the same
    // test caught that too.
    StaticJsonDocument<512> reply;

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
            connClose(conn);
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

        // ONE LINK. A second socket from the owner while the first is healthy is
        // refused by closing it — it used to REPLACE the owner, so any second
        // connection (a seeker, a probe, a primary that reconnected before its old
        // socket timed out) silently knocked the real link out of ownership and every
        // SET after it read "not the owner". Only ever the OUTBOUND socket is
        // protected this way: an inbound owner has no liveness signal here, so it
        // keeps the old rule that the newest handshake wins.
        if (accepted && !conn.outbound && g_ownerOutbound && g_ownerLinked && brainlink::connected()) {
            Serial.println(F("[NODE] HELLO on a second socket while the link we dialled is healthy — closing it."));
            connClose(conn);
            return;
        }
        if (accepted) {
            g_ownerClientId = conn.id;
            g_ownerLinked   = true;
            g_ownerOutbound = conn.outbound;
            if (conn.outbound) {
                brainlink::markJoined();
                // A socket we dialled never fires WS_EVT_CONNECT, so "linked" is
                // decided here, by the handshake the owner just passed.
                g_linkedClientId = conn.id;
                g_primaryLinked  = true;
            }
        }
        // Caps are what the board can PHYSICALLY drive, and the two are
        // mutually exclusive by design (PWM and serial never share a board), so
        // exactly one of these is non-zero.
        // HAS_CT is a pin-map fact, like HAS_SERVO — so a board cannot claim a
        // clamp it has no pad for, and the tray cannot offer one that is not
        // there.
        topo::nodelink::buildWelcome(reply.to<JsonObject>(), host.c_str(),
                                     BOARD_NAME, buildstamp::fw(),   // the Boards page shows it
                                     HAS_SERVO ? SERVO_COUNT : 0,
                                     HAS_LINEAR ? 1 : 0,
                                     g_owner.c_str(), accepted,
#ifdef PIN_CT
                                     1,
#else
                                     0,
#endif
                                     /*pollsPlugs=*/true,
                                     /*dialsIn=*/true,
                                     /*hasRf=*/HAS_RF != 0,
                                     /*hasBin=*/HAS_BIN != 0);
        // Why this node last booted, so the primary's link log can tell a tool
        // switched off at the wall ("poweron"/"brownout") from a crash
        // ("panic"/"task_wdt"). See withBootInfo() in nodelink.js.
        topo::nodelink::addBootInfo(reply.as<JsonObject>(), millis() / 1000UL,
                                    resetreason::now());
#ifdef PIN_CT
        if (accepted) g_clampResend = true;
#endif
    } else if (strcmp(t, "PING") == 0) {
        topo::nodelink::buildPong(reply.to<JsonObject>());
    } else if (strcmp(t, "CONFIG") == 0) {
        // SAME GATE AS SET. An accepted WELCOME is what earns the right to
        // configure, not merely holding a socket — a board anyone could
        // re-point at a different sensor has no claim at all.
        if (!g_ownerLinked || conn.id != g_ownerClientId) {
            Serial.println(F("[CONFIG] REFUSED — not the owner."));
            topo::nodelink::buildAck(reply.to<JsonObject>(), f["seq"] | 0, false,
                                     "not the owner of this node");
        } else {
            const char* err = nullptr;
            size_t n = 0;
            topo::nodelink::SensorSpec parsed[topo::nodelink::kMaxSensorsPerNode];
            if (!topo::nodelink::parseConfigFrame(f, parsed, topo::nodelink::kMaxSensorsPerNode, n, err)) {
                Serial.print(F("[CONFIG] MALFORMED — ")); Serial.println(err ? err : "?");
                topo::nodelink::buildAck(reply.to<JsonObject>(), f["seq"] | 0, false, err);
            } else {
                // One list on the wire, two consumers here: clamps (this board's
                // own ADC) and plugs (smart plugs on the network that this board
                // polls for the primary — 2026-10-03). Split them once.
                topo::nodelink::SensorSpec cts[topo::nodelink::kMaxSensorsPerNode];
                topo::nodelink::SensorSpec plugs[topo::nodelink::kMaxSensorsPerNode];
                topo::nodelink::SensorSpec bins[topo::nodelink::kMaxSensorsPerNode];
                size_t ctN = 0, plugN = 0, binN = 0;
                for (size_t i = 0; i < n; i++) {
                    if (parsed[i].isPlug)     plugs[plugN++] = parsed[i];
                    else if (parsed[i].isBin) bins[binN++]   = parsed[i];
                    else                      cts[ctN++]     = parsed[i];
                }
#if !HAS_BIN
                if (binN) {
                    Serial.println(F("[CONFIG] REFUSED — no bin pad on this board."));
                    topo::nodelink::buildAck(reply.to<JsonObject>(), f["seq"] | 0, false,
                                             "no bin pad on this node");
                } else
#endif
#ifndef PIN_CT
                if (ctN) {
                    // Honest refusal beats silence: the layout believes this board
                    // watches a clamp, and it physically cannot.
                    Serial.println(F("[CONFIG] REFUSED — no CT pad on this board."));
                    topo::nodelink::buildAck(reply.to<JsonObject>(), f["seq"] | 0, false,
                                             "no sensor hardware on this node");
                } else
#endif
                {
#ifdef PIN_CT
                    // ALL OR NOTHING, and a WHOLE new list — parseConfigFrame has
                    // already refused anything partial.
                    for (size_t i = 0; i < ctN; i++) g_sensors[i] = cts[i];
                    g_sensorCount = ctN;
                    // Fall back per FIELD, not per frame: a primary that sends two
                    // of the three is not an error, and substituting a whole default
                    // set for a partial one would quietly discard what it did send.
                    // Zero is the sentinel and parseConfigFrame has already refused
                    // any real value that could look like one.
                    g_tripParams = sensing::TripParams();
                    if (ctN) {
                        if (cts[0].tripRatio  != 0.0f) g_tripParams.tripRatio  = cts[0].tripRatio;
                        if (cts[0].minCounts  != 0.0f) g_tripParams.minCounts  = cts[0].minCounts;
                        if (cts[0].clearRatio != 0.0f) g_tripParams.clearRatio = cts[0].clearRatio;
                    }
                    for (size_t i = 0; i < topo::nodelink::kMaxSensorsPerNode; i++) g_senseOn[i] = false;
                    // Force a report on the next tick rather than waiting out a
                    // repeat interval: the primary has just said what it is
                    // watching and should not sit through kSenseRepeatMs of not
                    // knowing whether a saw is already running.
                    g_senseKnown  = false;
                    g_lastSenseMs = 0;
                    Serial.print(F("[CONFIG] "));
                    if (!ctN) Serial.println(F("(nothing to watch)"));
                    else {
                        for (size_t i = 0; i < ctN; i++) {
                            Serial.print(g_sensors[i].sensorId);
                            Serial.print(F("@ch")); Serial.print(g_sensors[i].channel);
                            Serial.print(i + 1 < ctN ? F(", ") : F("\n"));
                        }
                        // Print what is IN FORCE, not what arrived: a primary that
                        // sent nothing and a primary that sent the same numbers this
                        // board already had look identical on the wire and must not
                        // look identical in the log.
                        Serial.print(F("[CONFIG] trip ")); Serial.print(g_tripParams.tripRatio, 2);
                        Serial.print(F("x floor, min ")); Serial.print(g_tripParams.minCounts, 1);
                        Serial.print(F(" counts, release ")); Serial.print(g_tripParams.clearRatio, 2);
                        Serial.println(F(" of trip"));
                    }
#endif
                    plugApplyConfig(plugs, plugN);
                    binApplyConfig(bins, binN);
                    topo::nodelink::buildAck(reply.to<JsonObject>(), f["seq"] | 0, true);
                }
            }
        }
    } else if (strcmp(t, "SET") == 0) {
        topo::nodelink::SetCommand cmd;
        const char* err = nullptr;
        // THE ENFORCEMENT. A WELCOME that was accepted is what earns the right
        // to command; merely holding a socket does not. Checked on every SET
        // rather than once at connect, because that is the frame that moves a
        // real valve — and because a client id can be reused after a reconnect.
        if (!g_ownerLinked || conn.id != g_ownerClientId) {
            Serial.print(F("[SET] REFUSED — client #")); Serial.print(conn.id);
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
    } else if (strcmp(t, "OTA") == 0) {
        // OWNER ONLY, like SET: replacing this board's firmware is the most
        // consequential thing a socket can ask for.
        topo::nodelink::OtaOrder order;
        const char* err = nullptr;
        if (!g_ownerLinked || conn.id != g_ownerClientId) {
            Serial.println(F("[OTA] REFUSED — not the owner"));
            topo::nodelink::buildOtaState(reply.to<JsonObject>(), "fail", -1, "not the owner of this node");
        } else if (!topo::nodelink::parseOtaFrame(f, order, err)) {
            Serial.print(F("[OTA] MALFORMED — ")); Serial.println(err ? err : "?");
            topo::nodelink::buildOtaState(reply.to<JsonObject>(), "fail", -1, err);
        } else if (!otaguard::hasSlots()) {
            Serial.println(F("[OTA] REFUSED — one app slot; flash this board once by cable"));
            topo::nodelink::buildOtaState(reply.to<JsonObject>(), "fail", -1, "one app slot - flash by cable once");
        } else if (otaguard::onProbation()) {
            topo::nodelink::buildOtaState(reply.to<JsonObject>(), "fail", -1, "running image not yet proven");
        } else if (g_otaPending || g_otaRunning) {
            topo::nodelink::buildOtaState(reply.to<JsonObject>(), "fail", -1, "an update is already running");
        } else if (actuatorMoving()) {
            topo::nodelink::buildOtaState(reply.to<JsonObject>(), "fail", -1, "a gate is moving");
        } else {
            g_otaOrder   = order;
            g_otaFrom    = conn.remote;
            g_otaPending = true;        // loop() takes it from here
            return;                     // runOta() reports; no reply from this task
        }
    } else if (strcmp(t, "PRESS") == 0) {
        // OWNER ONLY, like SET: this operates a motor's contactor through a remote.
        topo::nodelink::PressOrder po;
        const char* err = nullptr;
        if (!g_ownerLinked || conn.id != g_ownerClientId) {
            Serial.println(F("[PRESS] REFUSED — not the owner."));
            topo::nodelink::buildAck(reply.to<JsonObject>(), f["seq"] | 0, false, "not the owner of this node");
        } else if (!topo::nodelink::parsePressFrame(f, po, err)) {
            Serial.print(F("[PRESS] MALFORMED — ")); Serial.println(err ? err : "?");
            topo::nodelink::buildAck(reply.to<JsonObject>(), f["seq"] | 0, false, err);
#if HAS_RF
        } else if (g_pressPending) {
            topo::nodelink::buildAck(reply.to<JsonObject>(), po.seq, false, "a press is already waiting");
        } else {
            g_pressOrder   = po;
            g_pressPending = true;      // loop() keys it and ACKs
            return;
        }
#else
        } else {
            topo::nodelink::buildAck(reply.to<JsonObject>(), po.seq, false, "no transmitter on this board");
        }
#endif
    } else if (strcmp(t, "ALERT") == 0) {
        // OWNER ONLY: what this board's pixel says about the shop is the brain's call, and a stranger's word on it is
        // not. No reply — the brain re-sends the whole state on every link-up, so a lost frame heals itself.
        bool full = false; const char* err = nullptr;
        if (!g_ownerLinked || conn.id != g_ownerClientId) {
            Serial.println(F("[ALERT] ignored — not the owner."));
        } else if (!topo::nodelink::parseAlertFrame(f, full, err)) {
            Serial.print(F("[ALERT] MALFORMED — ")); Serial.println(err ? err : "?");
        } else {
            if (full != statusled::binAlert()) Serial.printf("[ALERT] %s\n", full ? "a dust bin on this board's system is FULL" : "bin clear");
            statusled::setBinAlert(full);
        }
        return;
    } else if (strcmp(t, "REFUSE") == 0) {
        // The primary declined a socket we dialled. Nothing to answer: brainlink goes
        // back to seeking, on its own backoff.
        Serial.print(F("[NODE] primary refused the link: ")); Serial.println(f["reason"] | "?");
        return;
    } else if (strcmp(t, "WHERE") == 0) {
        // The primary says where it is. Acted on ONLY for our owner: a WHERE from
        // anyone else is a stranger telling us where to knock, and while a refused
        // JOIN could not hurt, there is no reason to spend a socket on it.
        topo::nodelink::WhereOrder w; const char* err = nullptr;
        if (!topo::nodelink::parseWhereFrame(f, w, err)) {
            Serial.print(F("[WHERE] MALFORMED — ")); Serial.println(err ? err : "?");
        } else if (g_owner.length() && g_owner.equalsIgnoreCase(w.primaryId)) {
            Serial.print(F("[WHERE] the primary is at ")); Serial.print(w.ip); Serial.print(':'); Serial.println(w.port);
            brainlink::hint(w.ip, (uint16_t)w.port);
            topo::nodelink::buildAck(reply.to<JsonObject>(), 0, true);
        } else {
            Serial.print(F("[WHERE] ignored — from ")); Serial.print(w.primaryId);
            Serial.print(F(", this node belongs to ")); Serial.println(g_owner);
            return;
        }
    } else {
        return;   // unknown frame — ignore rather than guess
    }

    String s; serializeJson(reply, s);
    connSend(conn, s);
}

static void onNodeWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client,
                          AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        Serial.print(F("[NODE] Primary connected — client #"));
        Serial.print(client->id());
        Serial.print(F(" from ")); Serial.print(client->remoteIP());
        Serial.print(F(", ")); Serial.print(nodeWs.count()); Serial.println(F(" open"));
        // LOUD WHEN THE POOL IS CROWDED. A node at its limit refuses connections
        // at the TCP layer, which is invisible here and reads as a network fault
        // from the other end. If this line ever appears, something is
        // reconnecting far faster than it should — see kMaxLinkClients.
        if (nodeWs.count() > kMaxLinkClients) {
            Serial.print(F("[NODE] ⚠ too many link clients ("));
            Serial.print(nodeWs.count());
            Serial.println(F(") — something is reconnecting in a loop."));
        }
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

    const Conn conn{client->id(), client->remoteIP(), false, client};
    handleNodeFrame(conn, data, len);
}

#ifdef PIN_CT
// ── the sensing tick ────────────────────────────────────────────────────────
//
// Called every loop(); does real work only when there is something to watch.
// Non-blocking in the way that matters: CtSensor::read() samples for a fixed
// window (60 ms here, ~3.5 cycles at 60 Hz) and the watchdog is petted either
// side of it.
static void tickSensors() {
    if (!g_sensorCount) return;

    // The settle gate, the moving-actuator hold, the cadence, the floor and the
    // trip point are all CtTrip's — see sensing/CtTrip.h. What is left here is
    // the only part that is a NODE's business: turning the bit into a frame.
    const sensing::CtTrip::Tick t =
        g_ctTrip.update(g_ct, millis(), actuatorMoving(), g_tripParams, &watchdog::pet);
    if (!t.sampled) return;

    const uint32_t now = millis();
    // ON CHANGE, and again every kSenseRepeatMs. The change is what makes a tool
    // switching on a sub-second event; the repeat only stops ONE dropped frame
    // leaving the primary permanently wrong.
    bool due = !g_senseKnown || (uint32_t)(now - g_lastSenseMs) >= topo::nodelink::kSenseRepeatMs;
    for (size_t i = 0; i < g_sensorCount; i++) {
        // One clamp, one pad: every configured sensor on this board reads the
        // same ADC today. When a second pad exists this is where it branches.
        const bool changed = (g_senseOn[i] != t.on) || !g_senseKnown;
        if (!changed && !due) continue;
        g_senseOn[i] = t.on;
        // Telemetry in AMPS, converted HERE because this is the only place that
        // knows this board's measured amps-per-count. Everything CtTrip works in
        // is counts; nothing downstream should have to own a scale factor.
        // Negative stays negative — absent, not zero. See buildSense().
        const float aPer   = t.aPerCount;
        const float amps   = (aPer > 0.0f) ? t.rmsCounts * aPer : -1.0f;
        const float floorA = (aPer > 0.0f && g_ctTrip.floorLearnt())
                             ? g_ctTrip.floorCounts() * aPer : -1.0f;
        const float tripA  = (aPer > 0.0f && t.trip > 0.0f) ? t.trip * aPer : -1.0f;
        // 256, not 192: four optional floats and a bool were added on
        // 2026-09-17. ArduinoJson drops members SILENTLY on overflow, and a
        // dropped `on` would be a tool that never opens its gate.
        StaticJsonDocument<256> doc;
        topo::nodelink::buildSense(doc.to<JsonObject>(), g_sensors[i].sensorId,
                                   t.on, t.level, amps, floorA, tripA, t.floorFault);
        String s; serializeJson(doc, s);
        sendToOwner(s);
        if (changed) {
            Serial.print(F("[CT] ")); Serial.print(g_sensors[i].sensorId);
            Serial.print(t.on ? F(" ON  ") : F(" off "));
            Serial.print(t.rmsCounts, 1);
            // "trip 0.0" would read as a board with an absurdly low threshold,
            // which is the opposite of what a refused floor means.
            if (t.floorFault) Serial.println(F(" counts, NO FLOOR — clamp faulted"));
            else { Serial.print(F(" counts, trip ")); Serial.println(t.trip, 1); }
        }
    }
    if (due) g_lastSenseMs = now;
    g_senseKnown = true;
}
#endif

#ifdef PIN_CT
// ── the clamp jack ──────────────────────────────────────────────────────────
// Once a second: the mean of D0 over one mains cycle (sensing/ClampJack.h). Sent on a change, on a new link, and every
// kSenseRepeatMs, so one lost frame cannot leave the brain wrong about it.
static void tickClampJack() {
    const uint32_t now = millis();
    bool changed = false;
    if (g_jack.due(now)) {
        watchdog::pet();
        const uint32_t mv = sensing::ClampJack::probeMeanMv([] { return analogReadMilliVolts(PIN_CT); },
                                                           [](uint32_t us) { delayMicroseconds(us); });
        changed = g_jack.update(mv, now);
        if (changed) {
            Serial.print(F("[CT] jack: ")); Serial.print(g_jack.plugged() ? F("clamp plugged in (") : F("EMPTY — no clamp ("));
            Serial.print(mv); Serial.println(F(" mV)"));
        }
    }
    if (!g_jack.known()) return;
    if (!changed && !g_clampResend && (uint32_t)(now - g_lastClampMs) < topo::nodelink::kSenseRepeatMs) return;
    StaticJsonDocument<64> doc;
    topo::nodelink::buildClamp(doc.to<JsonObject>(), g_jack.plugged());
    String out; serializeJson(doc, out);
    sendToOwner(out);
    g_lastClampMs = now; g_clampResend = false;
}
#endif

static void reportState(const char* selectorId, const char* stateId, bool moving) {
    StaticJsonDocument<192> doc;
    topo::nodelink::buildState(doc.to<JsonObject>(), selectorId, stateId, moving);
    String s; serializeJson(doc, s);
    sendToOwner(s);
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

// ── this node dialling its primary (BrainLink.h) ────────────────────────────
static String ownerNow() { return g_owner; }
static bool   inboundOwnerUp() { return g_ownerLinked && !g_ownerOutbound; }
static void onBrainFrame(const uint8_t* d, size_t n) {
    const Conn conn{kOutboundId, brainlink::remote(), true, nullptr};
    handleNodeFrame(conn, d, n);
}
static void onBrainState(bool up) {
    if (up) return;                      // the HELLO that follows decides "linked"
    if (g_ownerOutbound) g_ownerLinked = false;
    if (g_linkedClientId == kOutboundId) g_primaryLinked = false;
    Serial.println(F("[NODE] Primary disconnected (our socket) — holding all gates."));
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
#if HAS_LINEAR
    // NO HOLD ACTION ON A SLIDER, as of 2026-09-17 (jeff: "we can ditch that long
    // press to calibrate sliders, I didn't even realise that was there").
    //
    // A gesture nobody knows about is not a feature, and this one moved a
    // CARRIAGE the length of a rail. The on-demand rule at the top of this file
    // is unchanged and is the path that actually gets used: the first SET that
    // needs a datum asks for the sweep.
    //
    // ITS ONE REAL JOB HAS MOVED RATHER THAN GONE. The hold was the only thing
    // that cleared HOME_FAILED, so without a replacement a node that failed its
    // sweep could not be re-homed without a power cycle. A SET now clears it —
    // see the HOME_FAILED branch in the move handler. That is a better trigger
    // anyway: it retries when someone actually asks for a gate, instead of
    // requiring a person to be standing at the board.
#endif

    Serial.println(F("=== DustGate node (secondary) ==="));
    Serial.print(F("Build: ")); Serial.print(buildstamp::fw());
    Serial.print(F("  (")); Serial.print(buildstamp::date()); Serial.print(F(" "));
    Serial.print(buildstamp::time()); Serial.println(F(")"));
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
    // What the link task is doing, over the network — a node's serial is rarely attached.
    // What the plug poller is doing, for the question "why does the brain say this plug is unreachable":
    // is the task alive (stack headroom), what was configured, and how old is each answer.
    server.on("/api/plugs", HTTP_GET, [](AsyncWebServerRequest* req) {
        String o = "{\"started\":" + String(g_plugTask ? "true" : "false") + ",\"rc\":" + String((int)g_plugTaskRc) +
                   ",\"largestBlock\":" + String((unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)) +
                   ",\"stackFree\":" + String(g_plugTask ? (unsigned)uxTaskGetStackHighWaterMark(g_plugTask) : 0u);
        o += ",\"gen\":" + String((unsigned long)g_plugCfgGen) + ",\"count\":" + String((unsigned)g_plugCount) + ",\"plugs\":[";
        for (size_t i = 0; i < g_plugCount; i++) {
            const PlugWatch& p = g_plugs[i];
            o += String(i ? "," : "") + "{\"id\":\"" + p.id + "\",\"ip\":\"" + p.ip + "\",\"tasmota\":" + (p.tasmota ? "true" : "false") +
                 ",\"ageMs\":" + String(p.atMs ? (long)(millis() - p.atMs) : -1L) + ",\"reachable\":" + (p.reachable ? "true" : "false") +
                 ",\"watts\":" + String((float)p.watts, 1) + "}";
        }
        o += "]}";
        req->send(200, "application/json", o);
    });
    server.on("/api/brainlink", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", brainlink::statusJson());
    });
    // After the listener: a node that is owned dials its primary from here on.
    brainlink::begin(WiFiProvisioner::getHostname().c_str(), onBrainFrame, onBrainState,
                     ownerNow, inboundOwnerUp);
    bootTrace("server");
    Serial.print(F("[NODE] Listening on ws://"));
    Serial.print(WiFiProvisioner::getHostname());
    Serial.println(F(".local/nodelink"));

    // Watchdog armed last, after the blocking WiFi connect — same discipline as
    // the primary sketch.
    watchdog::begin();

#ifdef PIN_CT
    // Start the CT's bias-settle clock from the moment the board is actually
    // running. Everything above this blocks (a WiFi connect alone can take
    // ~12 s), so on a normal boot the window has already elapsed by the time
    // anything asks — it bites only on the path it exists for, a fast boot that
    // reaches a reading before the rail is up.
    g_ct.begin();
    // D0's own pull-up, so a pad with NOTHING wired to it reads railed high — "no clamp" — rather than floating. Set
    // after a first read, so the ADC's own pin setup cannot undo it. Against the bias divider's 500 Ω it moves the
    // midpoint ~18 mV, which the RMS subtracts. ⚠ unverified on a C5: that the ADC leaves the pull-up enabled.
    (void)analogReadMilliVolts(PIN_CT);
    gpio_pullup_en((gpio_num_t)PIN_CT);
#endif
#if HAS_BIN
    pinMode(PIN_BIN_SENSOR, INPUT_PULLUP);   // the optocoupler's output; LOW = the bin is full
#endif
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
    // Divided by -HOME_DIRECTION as well, because moveTo() MULTIPLIES by it on
    // the way in. Without that the screen showed the negation of the commanded
    // position — a gate at 250mm read -250 — which is a poor thing to be reading
    // while deciding whether homing worked. Every other steps→mm conversion in
    // the tree already does this (firmware.ino, HttpApiServer, SerialDebugControl).
    f.sliderMm     = motor.getPosition() / ST3215_COUNTS_PER_MM / (-HOME_DIRECTION);
#endif
    // The OWNER, not "whoever is connected": that is the name this node will
    // still be waiting for after a reboot, and the useful thing to read when it
    // is waiting.
    if (g_primaryLinked && g_owner.length()) f.primaryHost = g_owner.c_str();

    if (g_lastCmdMs) f.lastCmdSec = (int)((millis() - g_lastCmdMs) / 1000);

    statusscreen::update(f);

    // The plug poller idles until a CONFIG gives it something to read. 10 KB: an HTTPClient and a
    // parsed Status reply live on this stack, and the plughttp seam (2026-10-05) put a few hundred
    // bytes more on it than the direct HTTPClient calls did — 6 KB overflowed on the first poll
    // (GET /api/plugs reported stackFree 0 and no plug ever answered). Check /api/plugs for headroom.
    startPlugTask();
}

void loop() {
    watchdog::pet();
    { static uint32_t lastTry = 0; if (!g_plugTask && millis() - lastTry > 5000) { lastTry = millis(); startPlugTask(); } }
    WiFiProvisioner::maintain();

    // `provision {...}` over the USB cable. A node has no console and needs
    // none — this is the ONE command the flashing tool sends, and without it
    // `dev.sh flash-node <name>` prompted for a hostname, said it was flashing
    // as that name, and silently did not write it. See pollSerialProvision().
    WiFiProvisioner::pollSerialProvision();

    // REQUIRED, not housekeeping. ESPAsyncWebServer never reaps disconnected
    // WebSocket clients on its own — without this call they accumulate until the
    // server stops accepting new ones. A primary that can't connect retries every
    // second forever (kReconnectMinMs), so a node left running beside a failing
    // link burns through client slots fast, and the symptom is the confusing one:
    // a node that answers a laptop fine while refusing the primary indefinitely.
    // BARE, i.e. the library default of 8 — see kMaxLinkClients for why passing a
    // tight limit here is a trap rather than a safeguard.
    nodeWs.cleanupClients();

    // OTA: a pending order runs here (see runOta), and a fresh image is judged
    // by whether this node is on WiFi AND has been adopted by its primary.
    if (g_otaPending && !g_otaRunning) runOta();
    otaguard::tick(WiFi.status() == WL_CONNECTED && g_ownerLinked);
    tickPlugs();
    runPress();
    tickBin();
    // The alert is the owner's word, held only while the owner is there to change it: a board left blinking by a brain
    // that went away would say "full" about a bin nobody can see any more.
    if (!g_ownerLinked && statusled::binAlert()) statusled::setBinAlert(false);

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

#ifdef PIN_CT
    tickSensors();
    tickClampJack();
#endif
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
    if (g_homeAsked && g_homing == HOME_NEEDED && motor.online()) startSweep();
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
            // The shop's pulse range rides every servo SET (NodeLink.h, kDefaultServoMinUs); one range for the board.
            if (cmd.minUs) ServoActuator::setPulseRange(cmd.minUs, cmd.maxUs);
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
            // ASKED TO FIND HOME AGAIN (SET.home, 2026-10-07): setting the slider up from the app starts here, because a
            // datum left over from before a jam or a slipped pinion is a count nobody should calibrate against. Drop it
            // and take the ordinary home-then-move path below. A sweep already running is left alone: it IS a fresh home.
            if (cmd.home && g_homing != HOME_RUNNING) {
                Serial.println(F("[MOVE] asked to find home again before this move"));
                g_homing = HOME_NEEDED;
            }
            if (g_homing == HOME_DONE) {
                motor.moveTo((long)(cmd.positionMm * ST3215_COUNTS_PER_MM * -HOME_DIRECTION));
                Serial.print(F("[MOVE] Slider -> ")); Serial.print(cmd.positionMm, 1);
                Serial.println(F("mm"));
                commanded = true;
            } else if (g_homing == HOME_FAILED) {
                // A SET CLEARS A FAILED HOME AND TRIES AGAIN — this took over from
                // the wake-button hold on 2026-09-17, and it is the better
                // trigger: it retries when someone actually asks for a gate,
                // rather than requiring a person at the board. The sweep re-reads
                // the endstops from scratch, so if the wiring has been fixed it
                // now works, and if it has not it fails the same way and says so.
                //
                // Still LOUD, for the reason the old refusal was: a gate that
                // never moves and never complains is the worst thing this node
                // could do. The difference is that this one is trying.
                Serial.print(F("[MOVE] no datum (homing failed) — retrying the sweep for "));
                Serial.print(cmd.positionMm, 1); Serial.println(F("mm"));
                g_homing = HOME_NEEDED;
                g_homeAsked    = true;
                g_deferredMove = true;
                g_deferredMm   = cmd.positionMm;
                topo::nodelink::strlcpy_(g_deferredSel,   cmd.selectorId, sizeof(g_deferredSel));
                topo::nodelink::strlcpy_(g_deferredState, cmd.stateId,    sizeof(g_deferredState));
            } else {
                // HOME_NEEDED or HOME_RUNNING. Either way the move is held until
                // there is a datum to measure it from; the difference is that
                // HOME_NEEDED also has to ASK for one, which is the on-demand
                // rule at the top of this file. loop() starts the sweep on the
                // next tick — not here, because this runs on the AsyncTCP task
                // and the sweep must only ever be driven from loop().
                g_homeAsked = true;
                // Hold the LATEST position and run it when the datum lands —
                // see g_deferredMove for why only the latest.
                g_deferredMove = true;
                g_deferredMm   = cmd.positionMm;
                topo::nodelink::strlcpy_(g_deferredSel,   cmd.selectorId, sizeof(g_deferredSel));
                topo::nodelink::strlcpy_(g_deferredState, cmd.stateId,    sizeof(g_deferredState));
                Serial.print(g_homing == HOME_RUNNING
                             ? F("[MOVE] deferred until the sweep finishes: ")
                             : F("[MOVE] no datum yet — homing first, then: "));
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

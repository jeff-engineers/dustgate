// =============================================================================
// firmware.ino — Main sketch
// Target: Adafruit ESP32-S2 Feather + Adafruit TMC2209 Breakout (#6121)
//
// Configuration is entirely in config.h.
//
// Required libraries (install via Arduino Library Manager):
//   - AccelStepper (by Mike McCauley)
//   - TMCStepper (by teemuatlut)
//   - EEPROM (built-in with ESP32 Arduino core)
//
// State machine:
//   STARTUP → HOMING → IDLE → MOVING → AT_STOP
//                                  ↑___________|
// =============================================================================

#include <EEPROM.h>
#include "utils/Watchdog.h"  // main-loop task watchdog (unattended-hang recovery)
#include "config.h"
#include "utils/MotionMath.h"
#include "utils/StatusLed.h"
#include "utils/StatusScreen.h"   // optional SSD1306; compiles to nothing unfitted
#include "utils/WakeButton.h"    // the button that lights it; ditto
#include "motor/ServoSelfTest.h"  // and, held for a second, sweeps every servo
#include "motor/MotorDriver.h"
#include "feedback/FeedbackSystem.h"
#include "control/ControlInput.h"
#include "control/FaultPolicy.h"   // which begin() failure costs which capability
#include "training/CalibrationStore.h"

// WiFi provisioning — included for any mode that needs network access.
// Handles first-boot captive portal and subsequent NVS credential lookup.
#if defined(CONTROL_SMART_OUTLET) || defined(ENABLE_HTTP_API)
  #include "utils/WiFiProvisioner.h"
#endif

// HTTP API server — runs alongside any control mode when ENABLE_HTTP_API is set.
#ifdef ENABLE_HTTP_API
  #include "api/HttpApiServer.h"
  HttpApiServer apiServer;
#endif

// =============================================================================
// Runtime stop positions — single source of truth used by all feedback systems.
// Populated at startup from EEPROM (if valid) or STOP_DISTANCES_MM in config.h.
// =============================================================================
float g_stopPositionsMM[NUM_STOPS + 1];

// Highest stop index actually trained/saved — see MotionMath.h for why this
// is tracked separately from g_stopPositionsMM's raw values.
int g_numTrainedStops = 0;

// Runtime gate count — set by setup wizard via set_num_gates API, stored in NVS.
// MUST remain <= NUM_STOPS; array bounds are determined at compile time.
int g_numActiveStops = 0;   // 0 = unconfigured

// Homing direction — loaded from NVS so the user can flip it via setup wizard without
// recompiling.  All existing code references HOME_DIRECTION which now expands to this.
int g_homeDirection = HOME_DIRECTION_DEFAULT;

// Auto motor-direction detect: set true once per homing sequence after we've
// already flipped direction in response to the FAR endstop tripping, so a second
// far-hit is treated as a real fault instead of an infinite flip loop. Reset when
// a fresh homing sequence begins (see loop() entry detection).
bool g_homeDirCorrected = false;

bool g_notHomedWarnShown = false; // suppress repeated "not homed" warnings

// Dual-endstop calibration + port roles (declared extern in MotionMath.h).
uint8_t g_stopRoles[NUM_STOPS + 1];
float   g_measuredStepsPerMM = 0.0f;   // 0 = not calibrated → status reports nominal
long    g_measuredSpanSteps  = 0;      // 0 = not calibrated
char    g_manifoldModel[16]  = "custom";
bool    g_homeIsMaxEndstop   = false;  // which endstop is the HOME datum = the user's
                                       // LEFT. false = D10/PIN_ENDSTOP_HOME, true = D11/
                                       // PIN_ENDSTOP_MAX. Chosen during first-home setup
                                       // so the carriage ALWAYS homes to the user's left;
                                       // gates are then numbered 1..N left→right from it.

// Default all roles: home at 0, unassigned elsewhere.
static void resetStopRoles() {
    for (int i = 0; i <= NUM_STOPS; i++) g_stopRoles[i] = (i == 0) ? ROLE_HOME : ROLE_UNASSIGNED;
}


void loadCalibration() {
    CalibrationData cal;
    if (CalibrationStore::load(cal)) {
        for (int i = 0; i <= NUM_STOPS; i++) {
            g_stopPositionsMM[i] = (i <= (int)cal.numStops) ? cal.stopMM[i]
                                                             : cal.stopMM[cal.numStops];
        }
        // Restore active gate count from cal (may be overridden by NVS below in setup)
        if (cal.numStops > 0 && (int)cal.numStops <= NUM_STOPS)
            g_numActiveStops = (int)cal.numStops;
        g_numTrainedStops = (int)cal.numStops;
        // Dual-endstop fields: roles, manifold model, measured span/steps-per-mm.
        for (int i = 0; i <= NUM_STOPS; i++) g_stopRoles[i] = cal.stopRole[i];
        strlcpy(g_manifoldModel, cal.manifoldModel, sizeof(g_manifoldModel));
        g_measuredStepsPerMM = cal.measuredStepsPerMM;
        g_measuredSpanSteps  = (long)(cal.maxTravelMM * cal.measuredStepsPerMM);
        g_homeIsMaxEndstop   = (cal.homeIsMaxEndstop != 0);
        DEBUG_PRINTLN(F("Loaded calibration from EEPROM."));
        CalibrationStore::print(cal);
    } else {
        // No calibration yet — zero all positions. Setup wizard will call
        // save_stop for each gate to populate them via the HTTP API.
        memset(g_stopPositionsMM, 0, sizeof(g_stopPositionsMM));
        g_numTrainedStops = 0;
        resetStopRoles();
        strlcpy(g_manifoldModel, "custom", sizeof(g_manifoldModel));
        g_measuredStepsPerMM = 0.0f;
        g_measuredSpanSteps  = 0;
        // Keep g_homeIsMaxEndstop as-is: the datum side is chosen during first-home
        // setup and persisted independently below.
        DEBUG_PRINTLN(F("No calibration data — awaiting setup wizard."));
    }
}

// ── Manifold profile (mirror shared/device-model MANIFOLD_PROFILES) ──────────
// Fills gatesMm[1..gateCount] and spanMm for a known model. Returns false for
// 'custom'/unknown (→ manual jog, no auto-placement).
static bool manifoldProfile(const char* model, int gateCount, float* gatesMm, float& spanMm) {
    float first, pitch, endMargin;
    if (strcmp(model, "rockler-2.5") == 0) {
        first = MANIFOLD_2_5_FIRST_GATE_OFFSET_MM; pitch = MANIFOLD_2_5_GATE_PITCH_MM; endMargin = MANIFOLD_2_5_END_MARGIN_MM;
    } else if (strcmp(model, "rockler-4") == 0) {
        first = MANIFOLD_4_FIRST_GATE_OFFSET_MM;   pitch = MANIFOLD_4_GATE_PITCH_MM;   endMargin = MANIFOLD_4_END_MARGIN_MM;
    } else {
        return false;
    }
    for (int i = 1; i <= gateCount; i++) gatesMm[i] = first + (i - 1) * pitch;
    spanMm = first + (gateCount - 1) * pitch + endMargin;
    return true;
}

// Reference-sweep parameters, captured when a /api/calibrate request is consumed
// and used by the STATE_HOMING → STATE_CALIBRATING flow.
char  g_calModel[16] = "custom";
int   g_calGateCount = 0;
bool  g_calibratePending = false;   // calibrate requested → home, then sweep

// Physical gate-to-gate pitch (mm) for a manifold model, or 0 for custom/unknown.
static float manifoldPitchMm(const char* model) {
    if (strcmp(model, "rockler-2.5") == 0) return MANIFOLD_2_5_GATE_PITCH_MM;
    if (strcmp(model, "rockler-4")   == 0) return MANIFOLD_4_GATE_PITCH_MM;
    return 0.0f;
}

// A model string is "recognised" if it names a real profile or the explicit
// 'custom' fallback. Anything else (e.g. a typo like 'rockler2.5') still runs as
// custom — span recorded, no auto-placement — but the caller warns so the typo
// isn't silently swallowed.
static bool isKnownManifoldModel(const char* model) {
    return manifoldPitchMm(model) > 0.0f || strcmp(model, "custom") == 0;
}

// Rockler manifolds ship in 2-gate units → physical gate count is EVEN. Round an
// odd request up (the extra port is a spare the user caps/leaves unused). Gated on
// having a real pitch profile, so 'custom' (no fixed geometry) is left as entered.
// Mirrors shared/device-model physicalGateCount().
static int physicalGateCount(const char* model, int n) {
    if (manifoldPitchMm(model) > 0.0f && (n % 2) != 0) n += 1;
    if (n > NUM_STOPS) n = NUM_STOPS;
    return n;
}

// Finish the reference sweep: given the measured far-endstop trigger position (in
// steps, from home datum 0), place all gates and persist. See the placement
// derivation in docs/dual-endstop-calibration.md. Span-based: absorbs per-build
// steps/mm + endstop-location variance; pitch is the fixed manifold property.
static void finishCalibrationSweep(long farTriggerSteps) {
    long farSpanSteps     = farTriggerSteps < 0 ? -farTriggerSteps : farTriggerSteps; // home→far
    long triggerSpanSteps = farSpanSteps + HOME_BACKOFF_STEPS;   // near→far triggers
    float spm = stepsPerMM();                                    // nominal (validated ~0.3%)

    g_measuredSpanSteps  = triggerSpanSteps;
    g_measuredStepsPerMM = spm;
    g_numActiveStops     = g_calGateCount;
    strlcpy(g_manifoldModel, g_calModel, sizeof(g_manifoldModel));
    resetStopRoles();
    for (int i = 1; i <= NUM_STOPS; i++) g_stopPositionsMM[i] = 0.0f;

    float pitchMm = manifoldPitchMm(g_calModel);
    if (pitchMm > 0.0f) {
        // Center the (N-1)*pitch gate array in the measured trigger-to-trigger span.
        // Work in mm-from-home (positive = away from home). Near trigger sits
        // backoffMm toward home from the datum (negative); gate1 is slack/2 past it.
        float backoffMm = (float)HOME_BACKOFF_STEPS / spm;
        float slackMm   = ((float)triggerSpanSteps / spm) - (float)(g_calGateCount - 1) * pitchMm;
        float gate1Mm   = -backoffMm + slackMm / 2.0f;
        for (int i = 1; i <= g_calGateCount && i <= NUM_STOPS; i++) {
            g_stopPositionsMM[i] = gate1Mm + (float)(i - 1) * pitchMm;
        }
        g_numTrainedStops = g_calGateCount;
        // Gates are numbered from the home datum outward. Because home is always the
        // user's LEFT endstop, Gate 1 is always the leftmost gate — no reversal.
        DEBUG_PRINT(F("[CAL] placed gates: gate1=")); Serial.print(gate1Mm, 2);
        DEBUG_PRINT(F("mm pitch=")); Serial.print(pitchMm, 1);
        DEBUG_PRINT(F("mm span=")); Serial.print((float)triggerSpanSteps / spm, 1);
        DEBUG_PRINTLN(F("mm"));
    } else {
        // Custom manifold — record the span but leave gate positions for manual jog.
        g_numTrainedStops = 0;
        DEBUG_PRINTLN(F("[CAL] custom manifold — span recorded, gates via manual jog."));
    }

    CalibrationData cal;
    cal.magic   = CALIB_MAGIC;
    cal.version = CALIB_VERSION;
    cal.numStops = (uint8_t)g_calGateCount;
    cal.maxTravelMM = (float)triggerSpanSteps / spm;
    cal.measuredStepsPerMM = spm;
    for (int i = 0; i <= NUM_STOPS; i++) cal.stopMM[i]   = g_stopPositionsMM[i];
    for (int i = 0; i <= NUM_STOPS; i++) cal.stopRole[i] = g_stopRoles[i];
    cal.stopMM[0] = 0.0f;
    cal.homeIsMaxEndstop = g_homeIsMaxEndstop ? 1 : 0;
    strlcpy(cal.manifoldModel, g_calModel, sizeof(cal.manifoldModel));
    CalibrationStore::save(cal);
}

// Persist the runtime home direction to NVS. Uses the same "api_cfg" namespace/key
// that setup() and HttpApiServer read, so an auto-flip survives reboot.
static void persistHomeDirection() {
    Preferences prefs;
    prefs.begin("api_cfg", false);
    prefs.putInt("home_dir", g_homeDirection);
    prefs.end();
}

// Persist g_homeIsMaxEndstop (which endstop is the home datum) into CalibrationData,
// creating a minimal record if none exists yet so the choice survives a reboot even
// before calibration has run.
static void persistHomeDatum() {
    CalibrationData cal;
    if (!CalibrationStore::load(cal)) {
        cal.magic              = CALIB_MAGIC;
        cal.version            = CALIB_VERSION;
        cal.numStops           = (uint8_t)g_numActiveStops;
        cal.maxTravelMM        = 0.0f;
        cal.measuredStepsPerMM = stepsPerMM();
        memset(cal.stopMM, 0, sizeof(cal.stopMM));
        for (int i = 0; i <= NUM_STOPS; i++) cal.stopRole[i] = (i == 0) ? ROLE_HOME : ROLE_UNASSIGNED;
        strlcpy(cal.manifoldModel, g_manifoldModel, sizeof(cal.manifoldModel));
        for (int i = 0; i <= NUM_STOPS; i++) cal.stopMM[i] = g_stopPositionsMM[i];
        cal.stopMM[0] = 0.0f;
    }
    cal.homeIsMaxEndstop = g_homeIsMaxEndstop ? 1 : 0;
    CalibrationStore::save(cal);
}

// The two raw endstop pins, and the ONLY place they are read. Everything above
// them — the datum/far mapping, the debounced logging, the status JSON — goes
// through these, so a board with no endstop pins needs no #if of its own.
//
// A board without a rack reads both as FALSE, not "triggered". That is the
// opposite of the fail-safe convention on a real switch (NC, so open reads as
// triggered and a broken wire stops the motor) and deliberately so: there is no
// wire to break and no motor to stop, and "triggered" would put an unfitted
// board into the over-travel branch on its first pass of loop().
#if HAS_LINEAR
static inline bool readEndstopHome() { return digitalRead(PIN_ENDSTOP_HOME) == HIGH; }
static inline bool readEndstopMax()  { return digitalRead(PIN_ENDSTOP_MAX)  == HIGH; }
#else
static inline bool readEndstopHome() { return false; }
static inline bool readEndstopMax()  { return false; }
#endif

// Physical read of the endstop currently serving as the HOME datum (the user's left).
static inline bool datumSwitchTriggered() {
    return g_homeIsMaxEndstop ? readEndstopMax() : readEndstopHome();
}
// Physical read of the far endstop (opposite the datum).
static inline bool farSwitchTriggered() {
    return g_homeIsMaxEndstop ? readEndstopHome() : readEndstopMax();
}

// -- Motor driver --
// Which one is a property of the BOARD, not of the env: HAS_LINEAR comes from
// whether the pin map wires the serial-servo bus. See motor/NullMotorDriver.h.
#if HAS_LINEAR
  // No linear driver in the tree yet — failing here beats failing at the linker.
  #error "HAS_LINEAR board with no motor driver — see firmware/attic/linear/README.md"
#else
  #include "motor/NullMotorDriver.h"
  NullMotorDriver motor;
#endif

// -- servo bring-up (ball-valve gates) --
// Four positional servos on the reserved PWM pins (25/26/27/14 on DevKitC).
// Signal only — servos powered from an EXTERNAL 5–6V rail, grounds common.
#include "motor/ServoActuator.h"
#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)
ServoActuator g_servos[SERVO_COUNT];
static const int SERVO_PINS[SERVO_COUNT] = { SERVO_PWM_PIN_1, SERVO_PWM_PIN_2, SERVO_PWM_PIN_3, SERVO_PWM_PIN_4 };
#endif

// -- Feedback system --
// Same seam as the motor above. The #error this used to carry is gone with it:
// "no feedback type" is now a legitimate board rather than a misconfiguration.
// The only arm today — LimitSwitchDistance is in attic/linear/, and comes back
// with the ST3215 slider.
#include "feedback/NullFeedback.h"
NullFeedback feedback;

// -- Control input --
#ifdef CONTROL_SMART_OUTLET
  #include "control/SmartOutletControl.h"
  SmartOutletControl control;

  // Outlet discovery — used by the /api/outlets/discover handling in loop()
  // below. Must run on this (the main loop) task; see HttpApiServer.cpp's
  // /api/outlets/discover route comment for why.
  #include "utils/MdnsQuery.h"
  #include "outlets/ShellyGen2Outlet.h"
  #include "outlets/ShellyDeviceName.h"
  #include "outlets/PlugClaim.h"        // RFC §8 — who owns a plug (discovery reports it)
#elif defined(CONTROL_SERIAL_DEBUG)
  #include "control/SerialDebugControl.h"
  SerialDebugControl control;
#else
  #error "No control type defined in config.h"
#endif

// Supplemental serial command processor — active alongside any primary control
// mode when ENABLE_SERIAL_COMMANDS is set.  When CONTROL_SERIAL_DEBUG is the
// active mode the primary `control` object already handles serial input; no
// second instance is needed.
#if defined(ENABLE_SERIAL_COMMANDS) && !defined(CONTROL_SERIAL_DEBUG)
  #include "control/SerialDebugControl.h"
  SerialDebugControl _serialCmds;
#endif

// =============================================================================
// E-stop
// Software-only: set via the 'estop' serial command or HTTP API. No physical
// e-stop button — this hardware isn't powerful enough to need one.
// =============================================================================
volatile bool g_eStopTriggered = false;

// ── Faults are PER CAPABILITY ──────────────────────────────────────────────
//
// Three independent things can fail in begin(), and they disable different
// amounts of the shop. Collapsing them into one latch meant a board whose WiFi
// mutex failed to allocate also refused to home the rack — two subsystems that
// share no wire.
//
//   g_faultMotor     TMC2209 UART handshake. No stepper.
//   g_faultEndstops  limit switches. There IS a motor, but homing it without a
//                    reference is how you drive a carriage into its own end.
//   g_faultOutlets   WiFi / Shelly plugs. Nothing mechanical about it, and
//                    unlike the other two it can come back on its own —
//                    WiFiProvisioner::maintain() reconnects unattended.
//
// What each blocks:
//
//   linear motion (home, jog, move, calibrate)  motor OR endstops
//   servo gates + routing                        NEITHER — see below
//   the FAULT indicator                          motor OR endstops
//
// SERVO GATES ARE DELIBERATELY UNGATED BY ALL OF THIS. They are on their own
// rail with their own driver, and a shop whose stepper died still opens and
// closes every ball valve it has. Nothing in LocalActuatorBus / TopologyRuntime
// / NodeBus consults these flags, and nothing should start: on a bench board,
// stepper power comes and goes far more often than the shop is actually broken,
// and taking the gates down with it is the failure that costs an afternoon.
bool g_faultMotor    = false;
bool g_faultEndstops = false;
bool g_faultOutlets  = false;

// Derived: "do not command the stepper". Latched for the boot like the facts it
// is built from. Kept as its own variable rather than a function because every
// linear-motion entry point already reads it by this name, and the point of the
// split is to change WHICH failures set it — not to churn twenty call sites.
bool          g_hardwareFault  = false;

// Which begin() stage failed, latched for the life of the boot. The one-shot
// [INIT] line at startup scrolls away long before anyone notices motion is
// refused, so every refusal repeats it: "motor" is the TMC2209 UART handshake,
// "endstops" the limit switches, "outlets" WiFi/Shelly.
char          g_faultStages[40] = "";

// =============================================================================
// State machine
// =============================================================================
enum State {
    STATE_STARTUP,
    STATE_HOMING,
    STATE_IDLE,
    STATE_MOVING,
    STATE_AT_STOP,
    STATE_CALIBRATING,   // dual-endstop reference sweep (home already done)
    STATE_DISABLED,
    STATE_ERROR
};

State currentState = STATE_STARTUP;
int   currentStop  = -1;
int   targetStop   = 0;

// =============================================================================
// Idle power-off — see HttpApiServer::idleTimeoutSec(). Reset on every real
// move/home command; if it goes unrefreshed past the configured timeout while
// otherwise idle, the driver is fully disabled and the position marked
// unknown, forcing a rehome (reuses the existing "not homed" gating below)
// before the next move rather than sitting energized indefinitely.
// =============================================================================
unsigned long g_lastActivityMs = 0;
bool          g_driverAsleep   = false;

// Forward declarations
void issueMove(int stop);

// Whether this board has a carriage to move at all. A compile-time constant
// rather than #if at each site, for the same reason NullMotorDriver is a null
// object: the state machine stays readable and the compiler folds the branches
// away. HAS_LINEAR comes from the pin map — see config.h.
//
// NullMotorDriver already makes the MOVE harmless; this is about the STATE
// machine around it. Without it a rackless board announces "Moving to stop 0",
// drops into STATE_MOVING, is told by NullFeedback that it has arrived, and logs
// "Arrived at stop 0" — a complete round trip through machinery for hardware it
// hasn't got, once per outlet event. Seen on a XIAO C5 primary, 2026-08-22.
static const bool kRackPresent = (HAS_LINEAR != 0);
void startHoming();
void setHomedLeft(bool homedLeft);

// =============================================================================
// Topology runtime — the routing brain wired to actual hardware.
//
// Until now the routing engine (TopologyRouter/Sequencer/Controller) existed
// only for the host conformance tests: nothing in the sketch included it, and
// GET /api/status served an idle stub. This is the wiring that makes a
// stored topology actually move valves.
//
//   SmartOutletControl (plug watts)
//        → TopologyRuntime (brain + move queue)
//            → NodeBus (which board drives this selector?)
//                → LocalActuatorBus → g_servos[] / the rack
//
// NodeBus registers no remotes yet, so every selector resolves to the local bus.
// Adding a secondary board later registers a RemoteActuatorBus and changes
// nothing above this line — that's the whole point of the seam.
// =============================================================================
#include "control/LocalActuatorBus.h"
#include "control/NodeBus.h"
#include "control/RemoteActuatorBus.h"
#include "control/TopologyRuntime.h"
#include "control/NodeRegistry.h"
#include "control/TopologyStore.h"

// The rack, as LocalActuatorBus needs to see it. Deliberately does NOT reuse
// issueMove()/STATE_MOVING: those are the stop-index path, and a topology
// linear state is an absolute mm position with no stop index behind it. The
// always-on endstop over-travel supervisor at the top of loop() still protects
// this move, and stop-following is suppressed while a topology is loaded
// (see STATE_IDLE / STATE_AT_STOP) so the two can never fight over the motor.
class SketchLinearDrive : public topo::LinearDrive {
public:
    bool moveToMm(float mm) override {
        if (g_hardwareFault || g_eStopTriggered) return false;
        if (currentStop == -1) return false;      // not homed — mm has no datum yet
        g_lastActivityMs = millis();
        g_driverAsleep   = false;
        motor.moveTo((long)(mm * stepsPerMM() * -HOME_DIRECTION));
        return true;
    }
    bool isMoving() const override { return motor.isMoving(); }
};

static SketchLinearDrive      g_linearDrive;
static topo::LocalActuatorBus g_localBus;
static topo::NodeBus          g_nodeBus;
static topo::TopologyRuntime  g_topoRuntime;
static topo::TopologyStore    g_topoStoreSketch;   // read-only view; the API server owns writes

// Secondary links. A fixed pool rather than dynamic allocation: each one owns a
// FreeRTOS task and a socket, and the RFC caps the design at 2–4 nodes — so the
// worst case is three secondaries plus this board.
#define MAX_SECONDARY_NODES 3
static topo::RemoteActuatorBus g_remoteBuses[MAX_SECONDARY_NODES];
static int                     g_remoteCount = 0;

// Which boards this primary is paired with. Persisted in NVS, independent of any
// topology — see NodeRegistry.h.
static topo::NodeRegistry      g_nodeRegistry;

// Dial every PAIRED node. Driven by NodeRegistry (NVS), not by the topology —
// see NodeRegistry.h for why the two were separated. Called once at boot and
// again whenever the pairing set changes, NOT on topology adopt: a layout edit
// must never tear down a healthy link.
//
// Keyed by host throughout. The link neither knows nor cares what a layout calls
// this board; NodeBus maps controllerId → host when a topology is adopted.
// Host with a user-confirmed takeover waiting to be sent, or "" for none.
// Consumed by syncPairedNodes BEFORE the bus dials, because the HELLO that
// carries the claim goes out on connect — arming after begin() would be a race
// against the socket, and losing it would silently drop the one thing the user
// explicitly asked for.
static String g_pendingTakeoverHost;

static void syncPairedNodes(const char* primaryId) {
    g_nodeBus.clearRemotes();
    for (int i = 0; i < g_remoteCount; i++) g_remoteBuses[i].end();
    g_remoteCount = 0;

    for (int i = 0; i < g_nodeRegistry.count() && g_remoteCount < MAX_SECONDARY_NODES; i++) {
        const char* host = g_nodeRegistry.host(i);
        if (!host || !*host) continue;
        topo::RemoteActuatorBus* bus = &g_remoteBuses[g_remoteCount++];
        if (g_pendingTakeoverHost.length() && g_pendingTakeoverHost == host) {
            bus->requestTakeover();          // one-shot; cleared as the HELLO is built
            g_pendingTakeoverHost = "";
            DEBUG_PRINT(F("[NODE] TAKEOVER armed for ")); DEBUG_PRINTLN(host);
        }
        bus->begin(host, primaryId, host, 80);
        g_nodeBus.registerRemote(std::string(host), bus);
    }
    DEBUG_PRINT(F("[NODE] Paired nodes dialling: ")); DEBUG_PRINTLN(g_remoteCount);
}

// Map the topology's controllerIds onto paired hosts. This is ALL a layout now
// contributes to off-board routing: which board drives which gate. A controller
// naming a host nobody paired stays unresolved, and NodeBus reports its gates as
// un-commandable rather than pretending a move landed.
static void syncControllerAliases() {
    g_nodeBus.clearAliases();
    for (JsonObjectConst c : g_topoRuntime.topology()["controllers"].as<JsonArrayConst>()) {
        if (!topo::_eq(c["role"], "secondary")) continue;
        JsonObjectConst link = c["link"];
        // Only wifi-ws is implemented. An esp-now node is valid in the schema but
        // has no transport yet, so leave it unaliased.
        if (!topo::_eq(link["transport"], "wifi-ws")) continue;
        const char* id   = c["id"].as<const char*>();
        const char* host = link["host"].as<const char*>();
        if (!id || !host || !*host) continue;
        g_nodeBus.setAlias(std::string(id), std::string(host));
        if (!g_nodeRegistry.has(host)) {
            // Worth saying out loud: the layout references a board that was never
            // paired, so its gates won't move and the reason isn't visible on the
            // canvas. Pair it from /boards.
            DEBUG_PRINT(F("[NODE] Topology names an UNPAIRED board: ")); DEBUG_PRINTLN(host);
        }
    }
}

#ifdef CONTROL_SMART_OUTLET
// Register the layout's plugs with the poller.
//
// The canvas writes plugs into the topology (`tool.sensor.outlet`,
// `collector.control.outlet`), but SmartOutletControl only ever polls the slots
// stored in its own NVS slots — so a tool paired on the canvas was watched by
// nothing, and the blower was still switched by whatever plug happened to be
// stored there. Under a topology, the LAYOUT is the source of truth
// for both; this is what makes that true on the device.
//
// Tool slots are RAM-only on purpose (configureOutlet doesn't touch NVS; only
// saveSlot does). They're rebuilt from the layout on every adopt — including at
// boot — so persisting them would just be a second copy to keep in sync. The
// stopIndex is 0 throughout: it belongs to the stop-index gate mapping, whose
// automation is already inert while a topology is loaded.
// Last state pushed to each collector plug, so loop() only commands one on
// change instead of every pass. Cleared whenever a layout is adopted — slot i
// may then be a different system's blower, and a stale "already asserted" would
// be an answer about the wrong one.
static bool g_dcAsserted[COLLECTOR_COUNT] = {false};
static bool g_dcHave[COLLECTOR_COUNT]     = {false};

static void syncTopologyOutlets() {
    JsonObjectConst doc = g_topoRuntime.topology();
    for (int i = 0; i < COLLECTOR_COUNT; i++) g_dcHave[i] = false;

    // Walk MACHINES, not tool elements. A machine owns the plug, and a machine
    // with two ports (cabinet + overarm on a table saw) is still one plug — so
    // iterating ports here would try to register the same outlet twice and burn
    // a slot doing it. For a schemaVersion-1 document machineIds() yields the
    // tool elements, so this is exactly what it always was.
    int slot = 0;
    for (const std::string& mid : topo::machineIds(doc)) {
        JsonObjectConst m = topo::machineDoc(doc, mid);
        JsonObjectConst o = m["sensor"]["outlet"];
        const char* ip = o["ip"].as<const char*>();
        if (!ip || !*ip) continue;                       // manual machine — nothing to poll
        if (slot >= SMART_OUTLET_COUNT) {
            DEBUG_PRINTLN(F("[Outlets] More paired machines than outlet slots — extras ignored."));
            break;
        }
        control.configureOutlet(slot++, o["gen"] | 2, ip,
                                m["name"] | mid.c_str(),
                                /*stopIndex=*/0,
                                o["thresholdW"] | (float)OUTLET_DEFAULT_THRESHOLD_W,
                                o["host"] | "");
    }
    // Drop any slot the layout no longer names, so an unpaired machine stops
    // being polled without waiting for a reboot.
    for (int i = slot; i < SMART_OUTLET_COUNT; i++) control.removeOutlet(i);
    DEBUG_PRINT(F("[Outlets] Layout plugs registered: ")); DEBUG_PRINTLN(slot);

    // One blower switch per system, in the same order systemIds() reports, so
    // slot i belongs to system i throughout (see the collector assert in loop()).
    //
    // A system with no plug means "I start that collector by hand" — a legitimate
    // shop. For slot 0 that means leaving whatever plug is already stored alone
    // rather than silently un-configuring a working blower; the other slots hold
    // nothing persistent, so there is nothing to preserve.
    std::vector<std::string> sysIds = g_topoRuntime.systemIds();
    for (size_t i = 0; i < sysIds.size(); i++) {
        if (i >= COLLECTOR_COUNT) {
            DEBUG_PRINTLN(F("[Outlets] More systems than collector slots — extras ignored."));
            break;
        }
        JsonObjectConst o = g_topoRuntime.collectorOutlet(sysIds[i]);
        const char* ip = o["ip"].as<const char*>();
        if (ip && *ip) {
            if (!control.collectorIs((int)i, ip))
                control.configureCollector((int)i, o["gen"] | 2, ip, o["host"] | "");
        } else if (i == 0) {
            DEBUG_PRINTLN(F("[Outlets] Layout names no collector plug — keeping the stored one."));
        } else {
            control.removeCollector((int)i);
        }
    }
    for (size_t i = sysIds.size(); i < COLLECTOR_COUNT; i++) control.removeCollector((int)i);
}
#endif

static void adoptStoredTopology() {
    // NOTE: no topology no longer means no links. Pairing is persisted separately
    // (NodeRegistry.h) precisely so a wiped or absent layout can't silently
    // un-pair the shop — only the controllerId→host aliases go away, which is all
    // a layout ever owned.
    if (!g_topoStoreSketch.exists()) { g_nodeBus.clearAliases(); g_topoRuntime.clear(); return; }
    String raw = g_topoStoreSketch.load();
    std::string err;
    if (g_topoRuntime.adopt(raw.c_str(), raw.length(), err)) {
        // Learn which controller id this board answers to. Stage 1 is
        // single-board, so that's the topology's primary by definition
        // (validateMinimal guarantees exactly one). When secondary builds land,
        // this becomes a build-flag/NVS decision rather than an assumption.
        for (JsonObjectConst c : g_topoRuntime.topology()["controllers"].as<JsonArrayConst>()) {
            if (topo::_eq(c["role"], "primary")) {
                g_nodeBus.setLocal(&g_localBus, c["id"] | "primary");
                break;
            }
        }
        DEBUG_PRINT(F("[V2] Topology adopted — routing is live. This board = "));
        DEBUG_PRINTLN(g_nodeBus.ownControllerId().c_str());
        syncControllerAliases();
#ifdef CONTROL_SMART_OUTLET
        syncTopologyOutlets();
#endif
    } else {
        DEBUG_PRINT(F("[V2] Topology REJECTED: ")); DEBUG_PRINTLN(err.c_str());
        g_nodeBus.clearAliases();
        g_topoRuntime.clear();
    }
}

// =============================================================================
// setup()
// =============================================================================
void setup() {
    Serial.begin(SERIAL_BAUD);
#if BOARD_HAS_NATIVE_USB
    // Native USB-CDC (S2/S3): Serial isn't ready until the host enumerates it —
    // wait for the monitor to connect, up to 5s, so boot logs aren't lost.
    // Open Serial Monitor before or immediately after flashing to catch them.
    unsigned long t0 = millis();
    while (!Serial && (millis() - t0) < 5000) { delay(10); }
#endif
    delay(100); // brief settle after connection
    DEBUG_PRINTLN(F("=== DustGate ==="));
    // Name the build, not the wiring diagram. This said "ESP32 + TMC2209" on
    // every target — including boards with no stepper — so the first line of
    // every boot log was wrong about what you were looking at.
    DEBUG_PRINT(F("Target: ")); DEBUG_PRINT(F(BOARD_NAME));
#if HAS_LINEAR
    DEBUG_PRINTLN(F(" + slider"));
#else
    DEBUG_PRINTLN(F(" (servo only)"));
#endif

    // ADC: 12-bit (0-4095), 3.3V reference
    analogReadResolution(12);

    // Before WiFi, so the pixel is already saying something during the blocking
    // connect below — the DevKitC has no other sign it got past reset unless a
    // serial monitor happens to be attached.
    statusled::begin();
    statusled::set(statusled::BOOTING);
    statusled::update();

    // The screen, where one is plugged in. Reports itself either way: every
    // build carries the driver now, so "no panel on the bus" is an ordinary
    // outcome and worth one line on serial rather than a dark rectangle.
    if (statusscreen::begin()) {
        DEBUG_PRINTLN(F("[SCREEN] panel answered at 0x3C — drawing"));
    } else {
#if defined(PIN_OLED_SDA) && defined(PIN_OLED_SCL)
        DEBUG_PRINTLN(F("[SCREEN] declared, but nothing answered — run 'i2c' to see what is on the bus; disabled"));
#endif
    }

    // The button that wakes it back up after the two-minute blank. Nothing on a
    // board without one, and it never does anything but light the glass.
    wakebutton::begin();

    // WiFi provisioning — must run before any WiFi-dependent control mode.
    // If WIFI_STA_SSID is hardcoded in config.h it is used directly.
    // Otherwise stored NVS credentials are tried; if none exist or connection
    // fails, a captive portal AP ("DustGate-Setup") is started and this call
    // blocks until the user provides credentials (then reboots).
#if defined(CONTROL_SMART_OUTLET) || defined(ENABLE_HTTP_API)
    // The portal's loop never returns, so without this tick the pixel would sit
    // frozen on BOOTING for as long as the board waits to be told which WiFi to
    // join — the one state that definitely needs a human to walk over.
    WiFiProvisioner::setPortalTick([]() {
        statusled::set(statusled::PORTAL);
        statusled::update();
        // The portal loop never returns, and this is the one screen a stranger
        // has to follow — "join DustGate-Setup, then open 192.168.4.1". Without
        // a tick here it would sit blank for exactly as long as that matters.
        statusscreen::Facts f;
        f.status   = statusled::PORTAL;
        f.apName   = WIFI_PORTAL_SSID;
        f.portalIp = "192.168.4.1";
        statusscreen::update(f);
    });
    WiFiProvisioner::begin();
    WiFiProvisioner::setPortalTick(nullptr);
#endif

    // Load calibration before feedback system initialises
    CalibrationStore::begin(); // Required on ESP32: allocates EEPROM flash region
    loadCalibration();

    // Load runtime config from NVS — takes priority over cal defaults above.
    // Uses the same namespace ("api_cfg") as HttpApiServer to avoid opening
    // multiple Preferences namespaces for the same flash partition.
    {
        Preferences prefs;
        prefs.begin("api_cfg", true);
        g_homeDirection  = prefs.getInt("home_dir",  HOME_DIRECTION_DEFAULT);
        int nvsGates     = prefs.getInt("num_gates", 0); // 0 = not saved yet
        prefs.end();
        if (nvsGates >= 1 && nvsGates <= NUM_STOPS) g_numActiveStops = nvsGates;
    }
    DEBUG_PRINT(F("[CFG] homeDirection=")); Serial.print(g_homeDirection);
    DEBUG_PRINT(F("  numActiveStops="));   Serial.println(g_numActiveStops);

    // Report each stage separately: a bare "INIT FAILED" can't tell a TMC2209
    // UART problem from an endstop or a WiFi/outlet one, which are three very
    // different pieces of wiring to go stare at.
    const bool okMotor    = motor.begin();
    const bool okFeedback = feedback.begin(&motor);
    const bool okControl  = control.begin();

    // What each failure COSTS is FaultPolicy.h's decision, not this function's —
    // it is a table with a truth table's worth of cases, and the sketch is the
    // one file that can't be host-tested. Here we only record what happened.
    faults::Stages stages;
    stages.motor    = !okMotor;
    stages.endstops = !okFeedback;
    stages.outlets  = !okControl;
#if !HAS_LINEAR
    // The board's pin map wires no slider, so `motor` is NullMotorDriver and
    // begin() returned true because nothing failed. Clearing the stage before
    // anything reports on it is what keeps a servo-only board from announcing a
    // motor failure that is the build working as designed.
    const bool kRackFitted = false;
    stages.motor = false;
#else
    const bool kRackFitted = true;
#endif
    const faults::Policy policy = faults::decide(stages, kRackFitted);

    g_faultMotor    = stages.motor;
    g_faultEndstops = stages.endstops;
    g_faultOutlets  = stages.outlets;

    const bool ok = !stages.motor && !stages.endstops && !stages.outlets;
    if (!ok) {
        DEBUG_PRINT(F("[INIT] motor="));      Serial.print(okMotor    ? "ok" : "FAIL");
        DEBUG_PRINT(F("  feedback="));        Serial.print(okFeedback ? "ok" : "FAIL");
        DEBUG_PRINT(F("  control="));         Serial.println(okControl ? "ok" : "FAIL");
        snprintf(g_faultStages, sizeof(g_faultStages), "%s%s%s",
                 okMotor    ? "" : "motor(TMC2209 UART) ",
                 okFeedback ? "" : "endstops ",
                 okControl  ? "" : "outlets(WiFi/Shelly) ");
    }
#if defined(ENABLE_SERIAL_COMMANDS) && !defined(CONTROL_SERIAL_DEBUG)
    _serialCmds.begin();   // supplemental serial processor (non-fatal if begin() returns false)
#endif
#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)
    for (int i = 0; i < SERVO_COUNT; i++) g_servos[i].begin(SERVO_PINS[i]);  // bind pins; attach on first move
    servoselftest::begin(g_servos, SERVO_COUNT);
    // The button's long press needs to know whether the collector is pulling
    // before it sweeps every gate. A lambda rather than a stored flag: the
    // answer has to be true AT THE MOMENT OF THE HOLD, not whenever setup ran.
    wakebutton::setCollectorQuery([]() { return g_topoRuntime.collectorOn(); });
    DEBUG_PRINTLN(F("[SERVO] Bring-up ready — 'servo <1-4> <angle>' (external 5-6V rail, common GND)."));
    DEBUG_PRINTLN(F("[SERVO] Hold the wake button 1s to sweep every channel."));
#endif

    // A hardware fault disables ONE CAPABILITY, not the whole device. Do NOT
    // return here: loop() unconditionally pumps the API server and the routing
    // runtime, and returning early left HttpApiServer::_mutex uncreated — the
    // first consumeXxx() then asserted inside xQueueSemaphoreTake and the board
    // sat in a reboot loop, which is also what made the serial prompt
    // unreachable for debugging the very wiring that faulted.
    //
    // Continuing is safe because every LINEAR motion entry point checks
    // g_hardwareFault (issueMove(), SketchLinearDrive::moveToMm()). Servos are on
    // an independent rail and check nothing, so a servo topology still routes on
    // a board whose stepper or endstops are broken.
    //
    // The two latches below are SEPARATE FACTS, which is the whole point of the
    // policy: "do not command the stepper" and "something is wrong here" used to
    // be set together, so a rackless board pulsed red forever over hardware it
    // was never built with, and a WiFi failure refused to home a healthy rack.
    g_hardwareFault = policy.linearMotion;
    currentState    = policy.errorState ? STATE_ERROR : STATE_IDLE;

    if (policy.errorState) {
        DEBUG_PRINTLN(F("INIT: linear motion FAILED — check wiring and config.h"));
        DEBUG_PRINTLN(F("Linear moves are disabled. Servo gates and routing still run."));
    } else {
        DEBUG_PRINTLN(F("Init OK. Type 'enable' to home and start."));
    }
    if (g_faultOutlets) {
        // Neither latch. The plugs are a network problem: WiFiProvisioner::
        // maintain() reconnects unattended, so a boot-time latch would outlive
        // the fault it describes. The pixel's NO_WIFI state already says it.
        DEBUG_PRINTLN(F("[INIT] Outlets (WiFi/Shelly) failed — plugs are offline."));
        DEBUG_PRINTLN(F("       Motion and gates are unaffected; the link may recover on its own."));
    }

    // Say what the board can still DO, not just what failed. A stage name tells
    // you where to point a multimeter; this tells you whether the shop works —
    // and it is the line that makes "the stepper is dead but my gates still
    // move" visible at boot instead of something you have to infer.
    DEBUG_PRINT(F("[INIT] capabilities: linear="));
    Serial.print(g_hardwareFault ? "refused" : "ok");
    DEBUG_PRINT(F("  gates=ok  plugs="));
    Serial.println(g_faultOutlets ? "offline" : "ok");

#if !HAS_LINEAR
    // policy.linearMotion is already true here — no rack means every motion
    // entry point must keep refusing, and reusing the existing latch is what
    // keeps that small: issueMove() and SketchLinearDrive::moveToMm() check it
    // by name, so nothing new has to be threaded through the homing, jog,
    // calibration and sweep paths.
    //
    // Several serial/API handlers report the latch as "Hardware fault at boot —
    // failed: <stages>". With nothing genuinely broken that string is empty and
    // the message reads like a bug, so name the real reason. Only when empty:
    // a true failure above owns this field and must not be overwritten.
    if (g_faultStages[0] == '\0')
        snprintf(g_faultStages, sizeof(g_faultStages), "no slider on this board");
    DEBUG_PRINTLN(F("[INIT] No slider on this board — motion disabled by design; "
                    "servos and routing are live."));
#endif

#ifdef ENABLE_HTTP_API
    if (!apiServer.begin()) {
        DEBUG_PRINTLN(F("[API] HTTP server failed to start."));
    } else {
        DEBUG_PRINT(F("[API] Listening on port 80.  Key: "));
        Serial.println(apiServer.apiKey());
    }
#endif

    // -- routing runtime -------------------------------------------
    // After the API server, which mounts LittleFS (where the topology lives).
#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)
    for (int i = 0; i < SERVO_COUNT; i++) g_localBus.bindServo(i, &g_servos[i]);
#endif
    g_localBus.bindLinear(&g_linearDrive);
    // This board answers to the topology's primary controller. Selectors with no
    // controllerId (every single-board topology) also land here.
    g_nodeBus.setLocal(&g_localBus, "primary");
    g_topoRuntime.begin(&g_nodeBus);
    g_topoStoreSketch.begin();

    // Pairing FIRST, and independent of whether a layout exists: a paired board
    // should come up linked (green at the node) on a primary that has never been
    // told what the shop looks like. That ordering is the whole point of the
    // split — see NodeRegistry.h.
    g_nodeRegistry.begin();
    syncPairedNodes(WiFiProvisioner::getHostname().c_str());

    adoptStoredTopology();

    // Arm the task watchdog on the main loop LAST — after all the blocking init
    // above (WiFi connect can block up to ~12s, calibration load, etc.), so none
    // of it trips a spurious reset. From here loop() must pet it each pass.
    watchdog::begin();   // watches this task (the Arduino loopTask)
}

// =============================================================================
// loop()
// =============================================================================
// =============================================================================
// updateStatusLed() — translate the state machine into the shared colour
// vocabulary (utils/StatusLed.h). Called once per loop() pass.
//
// Motion outranks status, matching the node: while something is physically
// moving, THAT is what the person standing at the gate needs to see. Underneath
// it the ranking is worst-first — a fault hides a WiFi problem hides "no layout
// yet" — because the top item is always the one to go deal with.
//
// "Ready" here means routing is live, not merely that the board booted. A
// primary with no topology stored has nothing to route, so it sits on blue: it
// is working, it just has no shop yet. That distinction is exactly what the old
// single LED could not express — it was dark for both.
//
// It also means every PAIRED board is answering — see allNodesLinked() below.
// =============================================================================

// True when every board this primary is paired with is currently linked.
//
// Green is reserved for "everything works" (StatusLed.h), and on a multi-board
// shop this primary working is only half of that: if a node is dark, the gates
// on it will not move, and the failure shows up as a tool that doesn't pull —
// long after you walked away from the board that could have told you.
//
// Keyed on PAIRING, not on the topology. The registry is what this board "knows
// about": a node paired but not yet given any gates is still a board that is
// supposed to be there, and a node that has silently dropped off WiFi is the
// case this exists to catch.
//
// A missing node is BLUE, not red — "on WiFi, not ready to work", the same as
// having no layout yet. It is usually a node still booting or a router
// hiccuping, both of which resolve themselves, and red is reserved for what the
// board cannot fix. The link retries on its own, so the colour comes back.
//
// No nodes paired = trivially true, which keeps a single-board shop on exactly
// the behaviour it had before.
//
// SAMPLED, not polled every pass. info() takes the bus mutex and copies strings,
// and it contends with that node's own NodeLink task; loop() runs thousands of
// times a second and an indicator does not need to. Same cadence as the node
// status publish, which is already the fastest anything reads this.
static bool allNodesLinked() {
    static bool          cached     = true;
    static unsigned long lastCheck  = 0;
    static int           lastDarkId = -1;   // which node we last reported dark

    if (lastCheck != 0 && millis() - lastCheck < V2_STATUS_PUBLISH_MS) return cached;
    lastCheck = millis();

    int dark = -1;
    for (int i = 0; i < g_remoteCount; i++) {
        if (!g_remoteBuses[i].info().connected) { dark = i; break; }
    }

    // Log the TRANSITION, both directions. A node dropping out is otherwise
    // silent on the primary — the only evidence is a gate that stops moving,
    // which sends you looking at the gate.
    if (dark != lastDarkId) {
        if (dark >= 0) {
            DEBUG_PRINT(F("[NODE] dark: "));
            DEBUG_PRINT(g_remoteBuses[dark].host());
            DEBUG_PRINTLN(F(" — indicator holds blue until it answers"));
        } else {
            DEBUG_PRINT(F("[NODE] all paired boards linked ("));
            DEBUG_PRINT(g_remoteCount);
            DEBUG_PRINTLN(F(")"));
        }
        lastDarkId = dark;
    }

    cached = (dark < 0);
    return cached;
}

// -----------------------------------------------------------------------------
// The optional status screen. Everything it knows comes from here, and it knows
// nothing the pixel doesn't — the state word IS statusled::state(), spelled.
//
// UNVERIFIED, like everything else about the screen: no panel has been wired to
// a board. Compiles to nothing on a build without one, so this function costs an
// unfitted board a call to an empty inline.
//
// The gaps, stated rather than faked: the runtime publishes THAT the shop is
// transitioning, not WHICH selector is in flight, so the make-before-break
// screen in docs/mockups/oled-status.html can name the tool but not yet its two
// gates. Naming a plausible gate would be worse than leaving the line out.
// -----------------------------------------------------------------------------
static void updateStatusScreen() {
    if (!statusscreen::present()) return;

    statusscreen::Facts f;
    f.role   = statusscreen::Role::PRIMARY;
    f.selfTestCh      = servoselftest::channel();
    f.selfTestOf      = SERVO_COUNT;
    f.selfTestAngle   = servoselftest::angle();
    if (!servoselftest::active()) f.selfTestRefused = servoselftest::refusal();
    f.status = statusled::state();
    f.motion = statusled::motion();

#if defined(CONTROL_SMART_OUTLET) || defined(ENABLE_HTTP_API)
    static String host;
    if (!host.length()) host = WiFiProvisioner::getHostname();
    f.hostname = host.c_str();
    // A static, not the String WiFi.SSID() returns: that temporary dies at the
    // end of this statement and Facts holds a bare pointer into it.
    static String ssid;
    ssid = WiFi.SSID();
    f.ssid = ssid.length() ? ssid.c_str() : nullptr;
    if (WiFi.status() == WL_CONNECTED) {
        // Four bars from RSSI. The thresholds are the usual ones and not worth
        // agonising over: this glyph answers "is the wifi ok", not "what is my
        // link margin".
        const int rssi = WiFi.RSSI();
        f.wifiBars = rssi >= -60 ? 4 : rssi >= -70 ? 3 : rssi >= -80 ? 2 : rssi >= -90 ? 1 : 0;
    } else {
        // Half of all WiFi faults are the wrong SSID, so the NO_WIFI screen
        // says which network it is looking for — the core keeps the configured
        // one here even while disconnected.
        f.wifiBars = 0;
    }
#endif

    // Gates: how many of the shop's selectors can actually be driven right now.
    // "ready" is onlineFor(), not "configured" — a gate on a node that stopped
    // answering is exactly the one you want counted as missing.
    if (g_topoRuntime.loaded()) {
        int total = 0, ready = 0;
        for (const topo::SystemView& sys : topo::systemsOf(g_topoRuntime.topology())) {
            for (JsonObjectConst el : sys.elements) {
                if (!el["type"].is<const char*>()) continue;
                if (strcmp(el["type"].as<const char*>(), "selector") != 0) continue;
                total++;
                if (g_nodeBus.onlineFor(el)) ready++;
            }
        }
        f.gatesTotal = total;
        f.gatesReady = ready;
        f.collectorOn = g_topoRuntime.collectorOn();

        // The tool by ITS name, never a port or machine id — that is what the
        // woodworker recognises, and it is the same rule the canvas follows.
        static std::string toolName;
        toolName.clear();
        std::vector<std::string> active = g_topoRuntime.activeMachines();
        if (!active.empty()) {
            JsonObjectConst m = topo::machineDoc(g_topoRuntime.topology(), active.front());
            const char* n = m["name"].as<const char*>();
            toolName = (n && *n) ? n : active.front();
            f.toolName = toolName.c_str();
        }
    }

    // Nodes: linked / paired. Zero paired nodes leaves the line off entirely
    // rather than printing "nodes 0/0", which reads like a fault on a shop that
    // simply has one board.
    if (g_remoteCount > 0) {
        int linked = 0;
        for (int i = 0; i < g_remoteCount; i++)
            if (g_remoteBuses[i].info().connected) linked++;
        f.nodesTotal  = g_remoteCount;
        f.nodesLinked = linked;

        // A dark board is the fault worth naming: which one, and for how long.
        static std::string darkHost;
        for (int i = 0; i < g_remoteCount; i++) {
            topo::RemoteActuatorBus::NodeInfo n = g_remoteBuses[i].info();
            if (n.connected) continue;
            darkHost = g_remoteBuses[i].host();
            f.darkNode = darkHost.c_str();
            // "last seen 42s ago" is the number that says whether this is a
            // board that just rebooted or one that has been off all morning.
            // Zero means it has never answered since boot, which is a different
            // sentence, so the line is left off rather than reading "0s ago".
            if (n.lastSeenMs) f.darkForSec = (int)((millis() - n.lastSeenMs) / 1000);
            break;
        }
    }

    statusscreen::update(f);
}

static void updateStatusLed() {
    switch (currentState) {
        case STATE_HOMING:      statusled::setMotion(statusled::HOMING);      break;
        case STATE_CALIBRATING: statusled::setMotion(statusled::CALIBRATING); break;
        case STATE_MOVING:      statusled::setMotion(statusled::MOVING);      break;
        default:
            // Servo sweeps don't touch currentState (they're not rack moves), so
            // ask the bank directly or a ball-valve-only shop would never show
            // motion at all.
            {
                bool servoMoving = false;
#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)
                for (int i = 0; i < SERVO_COUNT; i++)
                    if (g_servos[i].isMoving()) { servoMoving = true; break; }
#endif
                statusled::setMotion(servoMoving ? statusled::MOVING : statusled::STILL);
            }
            break;
    }

#if !HAS_LINEAR
    // g_hardwareFault is set unconditionally on these builds to keep motion
    // refused (see setup(): FaultPolicy returns linearMotion = true whenever
    // there is no rack, which is the correct answer to "may I move?" and the
    // wrong answer to "is anything broken?"). So it can no longer speak for the
    // indicator — STATE_ERROR does. A real failure of the endstops or the
    // outlets still reaches this branch, because that path sets STATE_ERROR too.
    //
    // The latch answers "may I move?", not "is anything broken?" — and the
    // indicator wants the second question. Getting that wrong sat a rackless
    // board in FAULT from boot over a stepper it was never built with.
    if (currentState == STATE_ERROR) {
#else
    if (g_hardwareFault || currentState == STATE_ERROR) {
#endif
        statusled::set(statusled::FAULT);
    }
#if defined(CONTROL_SMART_OUTLET) || defined(ENABLE_HTTP_API)
    else if (WiFi.status() != WL_CONNECTED) {
        statusled::set(statusled::NO_WIFI);
    }
#endif
    else if (!g_topoRuntime.loaded() || !allNodesLinked()) {
        statusled::set(statusled::ONLINE);
    } else {
        statusled::set(statusled::READY);
    }

    statusled::update();
    // Before the screen decides whether to be lit, so a press shows up on this
    // pass rather than the next one.
    wakebutton::update();
    servoselftest::update();   // after the button, so a hold starts on this pass
    updateStatusScreen();
}

void loop() {
    watchdog::pet();   // we're alive this iteration

    // Keep WiFi alive unattended — nudges a reconnect if the link has dropped
    // and the core's auto-reconnect hasn't brought it back (e.g. AP rebooted).
#if defined(CONTROL_SMART_OUTLET) || defined(ENABLE_HTTP_API)
    WiFiProvisioner::maintain();
#endif

    // Run background processing for control input (HTTP server, etc.)
    control.update();

#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)
    // Effect any deferred servo auto-detach (move-then-detach; see ServoActuator).
    for (int i = 0; i < SERVO_COUNT; i++) g_servos[i].update();
#endif

    // -- routing runtime ------------------------------------------------
    // Feed live tool power in, pump the move queue out. Issues at most one move
    // per pass and never while one is in flight — that serialization IS the
    // one-servo-at-a-time current budget (docs/architecture-rfc.md §7).
#ifdef ENABLE_HTTP_API
    if (apiServer.consumeTopologyChanged()) adoptStoredTopology();
#endif
    if (g_topoRuntime.loaded()) {
#ifdef CONTROL_SMART_OUTLET
        // Map each configured plug to its tool and hand over its wattage. Reading
        // getPowerW()/host()/ip() from this task is safe (see SmartOutlet.h);
        // the routing decision itself stays entirely on the main loop.
        for (int i = 0; i < control.outletCount(); i++) {
            SmartOutlet* o = control.outlet(i);
            if (!o) continue;
            std::string machineId = g_topoRuntime.machineForOutlet(o->host(), o->ip());
            if (!machineId.empty()) g_topoRuntime.setMachinePower(machineId, o->getPowerW());
        }
#endif
        // millis() is passed in rather than read inside: the runtime is pure
        // (host-testable, no Arduino.h). It drives the collector coast-down.
        g_topoRuntime.update(millis());

#ifdef CONTROL_SMART_OUTLET
        // Assert each system's collector through the manual-override path: while
        // a topology is loaded, routing owns the decision and the stop-based
        // automation must not countermand it. Only pushed on change so it doesn't
        // spam the plug.
        //
        // Slot i ↔ systemIds()[i], the same pairing syncTopologyOutlets() used to
        // register them. systemIds() comes straight from document order, so the
        // two agree by construction for a given layout — and adopting a new one
        // clears g_dcHave, because slot i may now be a different blower entirely
        // and "same as last time" would then be a claim about the wrong system.
        {
            std::vector<std::string> sysIds = g_topoRuntime.systemIds();
            for (size_t i = 0; i < sysIds.size() && i < COLLECTOR_COUNT; i++) {
                bool want = g_topoRuntime.collectorOn(sysIds[i]);
                if (!g_dcHave[i] || want != g_dcAsserted[i]) {
                    control.setCollectorManual((int)i, want);
                    g_dcAsserted[i] = want; g_dcHave[i] = true;
                }
            }
        }
#endif
    }

    // -- Endstop over-travel safety — runs BEFORE motor.update() -----------------
    // Must run before the step, not after: motor.update() steps the carriage
    // regardless of currentState, so reacting *after* it let one step through per
    // loop — jogging into an already-triggered switch drifted the position +1 step
    // each time. Checking here clamps the target before any step is taken.
    // Directional: stops travel *toward* a triggered switch but allows backing
    // AWAY to release it. Homing drives into the home switch on purpose;
    // STATE_MOVING has its own endstop handling, so both are skipped here.
#if HAS_LINEAR
    // Applies to jogs AND commanded moves (STATE_MOVING). Homing drives into the
    // home switch on purpose; the calibration sweep manages the far switch itself.
    if (motor.isMoving() && currentState != STATE_HOMING && currentState != STATE_CALIBRATING) {
        long dtg = motor.distanceToGo();               // signed steps to target
        // HOME_DIRECTION points toward the datum, so +HOME_DIRECTION*dtg > 0 = toward
        // the datum, -HOME_DIRECTION*dtg > 0 = toward the far end. The switch on each
        // side is datum/far-relative (not fixed D10/D11), so this stays correct
        // whichever endstop is the home datum.
        bool towardFar  = (dtg * (long)(-HOME_DIRECTION)) > 0; // away from datum
        bool towardHome = (dtg * (long)( HOME_DIRECTION)) > 0;
        if (farSwitchTriggered() && towardFar) {
            motor.stop();
            DEBUG_PRINTLN(F("[SAFETY] Far endstop triggered — halted travel toward far end."));
        } else if (datumSwitchTriggered() && towardHome) {
            motor.stop();
            DEBUG_PRINTLN(F("[SAFETY] Home endstop triggered — halted travel toward home."));
        }
    }
#endif

    motor.update();

    // -- Endstop transition logging — AFTER update() so the logged position is
    //    the post-step position at the actual trigger point. Change-gated (jogs
    //    included, since jogs never enter STATE_MOVING).
#if HAS_LINEAR
    {
        // Debounced transition logging. When parked exactly on the home datum the
        // switch sits right at its trigger edge and chatters open/triggered; a raw
        // change gate logs every flicker. Only log a transition once the new level
        // has held stable for ENDSTOP_DEBOUNCE_MS.
        static const unsigned long ENDSTOP_DEBOUNCE_MS = 40;
        static bool esInit = false, lastHome = false, lastFar = false;
        static bool candHome = false, candFar = false;
        static unsigned long candHomeSince = 0, candFarSince = 0;
        unsigned long nowMs = millis();
        bool home = readEndstopHome();   // HIGH = triggered (NC open)
        bool far  = readEndstopMax();
        if (!esInit) {
            lastHome = candHome = home; lastFar = candFar = far;
            candHomeSince = candFarSince = nowMs; esInit = true;
        }
        // Track how long the current raw reading has been steady.
        if (home != candHome) { candHome = home; candHomeSince = nowMs; }
        if (far  != candFar ) { candFar  = far;  candFarSince  = nowMs; }
        if (candHome != lastHome && (nowMs - candHomeSince) >= ENDSTOP_DEBOUNCE_MS) {
            lastHome = candHome;
            DEBUG_PRINT(F("[ENDSTOP] Home: ")); DEBUG_PRINT(lastHome ? F("TRIGGERED") : F("open"));
            DEBUG_PRINT(F("  pos=")); DEBUG_PRINTLN(motor.getPosition());
        }
        if (candFar != lastFar && (nowMs - candFarSince) >= ENDSTOP_DEBOUNCE_MS) {
            lastFar = candFar;
            DEBUG_PRINT(F("[ENDSTOP] Far: ")); DEBUG_PRINT(lastFar ? F("TRIGGERED") : F("open"));
            DEBUG_PRINT(F("  pos=")); DEBUG_PRINTLN(motor.getPosition());
        }
    }
#endif

    // -- E-stop (software-latched, no physical button): highest priority ------
    if (g_eStopTriggered) {
        motor.stop();
        motor.enable(false);
        if (currentState != STATE_ERROR) {
            currentState = STATE_ERROR;
            DEBUG_PRINTLN(F(""));
            DEBUG_PRINTLN(F("!!! E-STOP ACTIVE — motor disabled."));
            DEBUG_PRINTLN(F("!!! Type 'home' to clear and re-home."));
            DEBUG_PRINTLN(F(""));
        }
    }

    // -- Serial commands (CONTROL_SERIAL_DEBUG or supplemental ENABLE_SERIAL_COMMANDS) --
#if defined(CONTROL_SERIAL_DEBUG) || defined(ENABLE_SERIAL_COMMANDS)
    // _SC aliases the active serial command object:
    //   CONTROL_SERIAL_DEBUG  → primary `control` (already a SerialDebugControl)
    //   ENABLE_SERIAL_COMMANDS → supplemental `_serialCmds` instance
    #ifdef CONTROL_SERIAL_DEBUG
      #define _SC control
    #else
      #define _SC _serialCmds
    #endif

    _SC.readRequestedStop(); // process serial input before checking flags

    if (_SC.consumeEStop()) {
        if (!g_eStopTriggered) {
            DEBUG_PRINTLN(F(""));
            DEBUG_PRINTLN(F("!!! E-STOP command received."));
            DEBUG_PRINTLN(F("!!! Type 'home' to clear and re-home."));
            DEBUG_PRINTLN(F(""));
        }
        g_eStopTriggered = true;
    }

    if (_SC.consumeHomeRequest() && currentState != STATE_HOMING) {
        if (g_hardwareFault) {
            DEBUG_PRINT(F("[ERROR] Hardware fault at boot — failed: "));
            Serial.print(g_faultStages);
            DEBUG_PRINTLN(F(" — fix wiring and reset before homing."));
        } else {
            g_eStopTriggered = false;
            g_notHomedWarnShown = false;
            motor.enable(true);
            currentState = STATE_HOMING;
            startHoming();
        }
    }

    if (_SC.consumeGconfRequest()) {
        motor.printDriverRegs();
    }

    // Serial 'calibrate <model> <gates>' — same home→sweep flow as POST /api/calibrate.
    {
        char calModel[16]; int calGates = 0;
        if (_SC.consumeCalibrateRequest(calModel, sizeof(calModel), calGates)) {
            if (g_hardwareFault) {
                DEBUG_PRINTLN(F("[CAL] Hardware fault — fix wiring and reset first."));
            } else if (currentState == STATE_HOMING || currentState == STATE_CALIBRATING ||
                       currentState == STATE_MOVING) {
                DEBUG_PRINTLN(F("[CAL] Busy — retry when idle."));
            } else {
                if (!isKnownManifoldModel(calModel)) {
                    DEBUG_PRINT(F("[CAL] Unknown model '")); DEBUG_PRINT(calModel);
                    DEBUG_PRINTLN(F("' — treating as custom (span only, no auto-placement)."));
                }
                strlcpy(g_calModel, calModel, sizeof(g_calModel));
                g_calGateCount     = physicalGateCount(calModel, calGates);
                if (g_calGateCount != calGates) {
                    DEBUG_PRINT(F("[CAL] Rounded to ")); DEBUG_PRINT(g_calGateCount);
                    DEBUG_PRINTLN(F(" gates (manifold ships in pairs; extra is a spare)."));
                }
                g_calibratePending = true;
                g_eStopTriggered   = false;
                motor.enable(true);
                DEBUG_PRINTLN(F("[CAL] Homing, then sweeping to far endstop..."));
                startHoming();
                currentState = STATE_HOMING;
            }
        }
    }

    {
        bool homedLeft = false;
        if (_SC.consumeHomeSideRequest(homedLeft)) {
            if (g_hardwareFault) {
                DEBUG_PRINTLN(F("[CFG] Hardware fault — fix wiring and reset first."));
            } else {
                setHomedLeft(homedLeft);
            }
        }
    }

#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)
    {
        int sIdx = 0, sAngle = 0; bool sDetach = false;
        if (_SC.consumeServoRequest(sIdx, sAngle, sDetach)) {
            ServoActuator& s = g_servos[sIdx - 1];   // sIdx validated 1..4 by the parser
            if (sDetach) s.detach();
            else         s.moveTo(sAngle);
        }
    }
#endif

    {
        float jogMM = 0.0f;
        if (_SC.consumeJogRequest(jogMM)) {
            if (g_hardwareFault) {
                DEBUG_PRINTLN(F("[ERROR] Hardware fault — fix wiring and reset before jogging."));
            } else {
                // Jog is a raw calibration move — clears e-stop and error state so the
                // state machine doesn't immediately call motor.stop() on the next tick.
                g_eStopTriggered = false;
                currentState = STATE_IDLE;
                long delta = (long)(jogMM * stepsPerMM() * -HOME_DIRECTION);
                long target = motor.getPosition() + delta;
                motor.enable(true);
                motor.moveTo(target);
                Serial.print(F("[JOG] delta=")); Serial.print(delta);
                Serial.print(F(" steps  target=")); Serial.println(target);
            }
        }
    }

    if (_SC.consumeClearCalRequest()) {
        CalibrationStore::erase();
        loadCalibration();
        // Without this, currentStop (and therefore homed, which is derived
        // from it) survived a clearcal untouched — so the UI kept reporting
        // "homed at gate N" from before the reset even though numActiveStops
        // just dropped to 0 and no gates exist to be at. That stale combo is
        // exactly what made the manifold visualizer's flow arrow appear
        // pointing at a gate index with no corresponding DOM element.
        currentStop = -1;
        targetStop  = 0;
        DEBUG_PRINTLN(F("Calibration cleared. config.h defaults loaded."));
    }

    // Supplemental mode: translate serial position commands into direct moves.
    // In CONTROL_SERIAL_DEBUG mode STATE_IDLE already handles this via
    // control.readRequestedStop(). Only fires when the requested stop changes
    // (tracked via static) to avoid re-issuing on every loop tick.
    #if defined(ENABLE_SERIAL_COMMANDS) && !defined(CONTROL_SERIAL_DEBUG)
    {
        static int _scLastActioned = -1;
        int serialStop = _SC.readRequestedStop();
        // Fire from AT_STOP too, not just IDLE — otherwise after landing on a gate
        // (which leaves us in STATE_AT_STOP) the next stop command is ignored until
        // a re-home.
        // kRackPresent: this is a stop-index move, so it means nothing on a board
        // with no carriage. It is also the one such path that fires WITHOUT any
        // input — SerialDebugControl's _requestedStop starts at 0 and
        // _scLastActioned at -1, so the first pass through STATE_IDLE issues a
        // move to stop 0 on its own. On a rack that is harmless (it is already
        // there); on a XIAO C5 primary it printed a whole move/arrive round trip
        // into the boot log for hardware that doesn't exist, which is what gave
        // this away on 2026-08-22.
        if (kRackPresent &&
            serialStop >= 0 && serialStop != _scLastActioned && !g_eStopTriggered &&
            (currentState == STATE_IDLE || currentState == STATE_AT_STOP)) {
            _scLastActioned = serialStop;
            targetStop = serialStop;
            issueMove(serialStop);
            // In CONTROL_SMART_OUTLET mode the outlet poller runs concurrently.
            // Without a manual override it sees no active tool and returns home
            // after OUTLET_OFF_DEBOUNCE_MS.  setManualOverride() holds the stop
            // until a real outlet power event clears it.
            #ifdef CONTROL_SMART_OUTLET
            control.setManualOverride(serialStop);
            #endif
        }
    }
    #endif

    #undef _SC
#endif // CONTROL_SERIAL_DEBUG || ENABLE_SERIAL_COMMANDS

    // -- HTTP API commands ----------------------------------------------------
#ifdef ENABLE_HTTP_API
    if (apiServer.consumeEStopRequest()) {
        if (!g_eStopTriggered) {
            DEBUG_PRINTLN(F("!!! E-STOP (HTTP API)."));
        }
        g_eStopTriggered = true;
    }

    if (apiServer.consumeHomeRequest() && currentState != STATE_HOMING) {
        if (g_hardwareFault) {
            DEBUG_PRINT(F("[API] Hardware fault at boot — failed: "));
            Serial.print(g_faultStages);
            DEBUG_PRINTLN(F(" — fix wiring and reset before homing."));
        } else {
            g_eStopTriggered   = false;
            g_notHomedWarnShown = false;
            motor.enable(true);
            currentState = STATE_HOMING;
            startHoming();
        }
    }

    {
        int moveStop = -1;
        if (apiServer.consumeMoveRequest(moveStop) && !g_hardwareFault &&
            currentState != STATE_HOMING) {
            if (moveStop >= 1 && moveStop <= NUM_STOPS && g_stopRoles[moveStop] == ROLE_BLOCKED) {
                // Blocked ports (capped, or reserved as a feed) are never move
                // targets — see docs/dual-endstop-calibration.md.
                DEBUG_PRINT(F("[API] Move: gate blocked, ignoring: "));
                DEBUG_PRINTLN(moveStop);
            } else if (moveStop >= 0 && moveStop <= g_numActiveStops) {
                targetStop = moveStop;
#ifdef CONTROL_SMART_OUTLET
                // In outlet mode, a manual move sets an override so the poll
                // task doesn't immediately revert to the outlet-selected stop.
                // Override clears automatically when the next tool powers on.
                control.setManualOverride(moveStop);
#endif
                issueMove(targetStop);
            } else {
                DEBUG_PRINT(F("[API] Move: stop out of range: "));
                DEBUG_PRINTLN(moveStop);
            }
        }
    }

    {
        float jogMM = 0.0f;
        if (apiServer.consumeJogRequest(jogMM) && !g_hardwareFault) {
            g_eStopTriggered = false;
            currentState = STATE_IDLE;
            long delta  = (long)(jogMM * stepsPerMM() * -HOME_DIRECTION);
            long target = motor.getPosition() + delta;
            motor.enable(true);
            motor.moveTo(target);
            DEBUG_PRINT(F("[API] Jog delta=")); Serial.print(delta);
            DEBUG_PRINT(F(" steps  target=")); Serial.println(target);
        }
    }

    if (apiServer.consumeClearCalRequest()) {
        CalibrationStore::erase();
        loadCalibration();
        g_numActiveStops = 0;  // return to unconfigured — setup wizard can restart
        // Same reasoning as the serial 'clearcal' handler above: currentStop
        // (and homed, derived from it) previously survived a clearcal
        // untouched, so the UI kept reporting "homed at gate N" from before
        // Start Over even though there are now zero gates to be at — which is
        // what made the manifold visualizer's flow arrow render pointing at a
        // gate index with no corresponding DOM element (or, on a later
        // restart, at a stale/mismatched position once gates existed again).
        currentStop = -1;
        targetStop  = 0;
#ifdef CONTROL_SMART_OUTLET
        // "Start Over" in the setup wizard means a full reset — without this,
        // the previous run's tool-to-gate outlet mappings (names, IPs,
        // thresholds) silently survived in NVS and kept driving gate
        // selection even after the wizard restarted from gate-count 0.
        control.clearAllOutlets();
#endif
        DEBUG_PRINTLN(F("[API] Calibration cleared. Gate count reset to 0."));
    }

    {
        int stopIdx = -1;
        if (apiServer.consumeSetStopRequest(stopIdx) && currentState == STATE_IDLE) {
            // Convert current motor position (steps) to mm.
            // HOME_DIRECTION inverts the step sign: positive steps are away from home.
            float currentMM = (float)motor.getPosition() / stepsPerMM() / (-HOME_DIRECTION);

            // Authoritative overlap guard (see MIN_STOP_SEPARATION_MM). Reject a
            // save that lands on top of another already-saved gate — home (0)
            // and the slot being (re)saved itself are excluded. Only meaningful
            // for gates within the active count; positions beyond it are stale.
            bool conflict = false;
            for (int j = 1; j <= g_numActiveStops && j <= NUM_STOPS; j++) {
                if (j == stopIdx) continue;
                if (g_stopPositionsMM[j] == 0.0f) continue; // unsaved slot
                if (fabsf(currentMM - g_stopPositionsMM[j]) < MIN_STOP_SEPARATION_MM) {
                    conflict = true;
                    DEBUG_PRINT(F("[API] Rejected stop "));  Serial.print(stopIdx);
                    DEBUG_PRINT(F(" at "));                   Serial.print(currentMM, 2);
                    DEBUG_PRINT(F(" mm — too close to stop ")); Serial.print(j);
                    DEBUG_PRINT(F(" ("));                     Serial.print(g_stopPositionsMM[j], 2);
                    DEBUG_PRINTLN(F(" mm). Jog further away and retry."));
                    break;
                }
            }
            // Only persist when the position clears the overlap guard —
            // otherwise leave calibration untouched (can't return here: the
            // rest of loop() must still run this iteration).
            if (!conflict) {
                g_stopPositionsMM[stopIdx] = currentMM;

                // Persist to CalibrationStore; reload other fields from existing data.
                CalibrationData cal;
                if (!CalibrationStore::load(cal)) {
                    // No valid cal yet — fill in what we know
                    cal.magic              = CALIB_MAGIC;
                    cal.version            = CALIB_VERSION;
                    cal.numStops           = 0;
                    cal.maxTravelMM        = 0.0f;
                    cal.measuredStepsPerMM = stepsPerMM();
                    memset(cal.stopMM, 0, sizeof(cal.stopMM));
                    // Dual-endstop fields: default roles (home at 0, unassigned elsewhere),
                    // custom manifold — so a save path never persists garbage.
                    for (int i = 0; i <= NUM_STOPS; i++) cal.stopRole[i] = (i == 0) ? ROLE_HOME : ROLE_UNASSIGNED;
                    cal.homeIsMaxEndstop = g_homeIsMaxEndstop ? 1 : 0;
                    strlcpy(cal.manifoldModel, "custom", sizeof(cal.manifoldModel));
                }
                cal.stopMM[stopIdx] = currentMM;
                if (stopIdx > (int)cal.numStops) cal.numStops = (uint8_t)stopIdx;
                CalibrationStore::save(cal);

                if (stopIdx > g_numTrainedStops) g_numTrainedStops = stopIdx;

                // Keep runtime count in sync (expand; never shrink during a session)
                if (stopIdx > g_numActiveStops) g_numActiveStops = stopIdx;

                DEBUG_PRINT(F("[API] Stop ")); Serial.print(stopIdx);
                DEBUG_PRINT(F(" saved at "));  Serial.print(currentMM, 2);
                DEBUG_PRINTLN(F(" mm"));
            }
        }
    }

    // Motor direction (runtime NVS override)
    {
        int newDir = 0;
        if (apiServer.consumeSetDirectionRequest(newDir)) {
            g_homeDirection = newDir;
            DEBUG_PRINT(F("[API] Motor direction: "));
            DEBUG_PRINTLN(newDir > 0 ? F("normal") : F("inverted"));
        }
    }

    // Active gate count (runtime NVS override)
    {
        int newGates = 0;
        if (apiServer.consumeSetNumGatesRequest(newGates)) {
            // Clear saved positions beyond the new count so a stale gate can't
            // reappear as a phantom proximity conflict if the count is later
            // raised again (positions live in RAM here; the EEPROM copy is
            // cleaned up below).
            for (int i = newGates + 1; i <= NUM_STOPS; i++) {
                g_stopPositionsMM[i] = 0.0f;
            }
            g_numActiveStops = newGates;

            // Trim the persisted calibration to match, so a reboot doesn't
            // restore the old (higher) gate count from cal.numStops.
            CalibrationData cal;
            if (CalibrationStore::load(cal) && (int)cal.numStops > newGates) {
                cal.numStops = (uint8_t)newGates;
                for (int i = newGates + 1; i <= NUM_STOPS; i++) {
                    cal.stopMM[i] = 0.0f;
                }
                CalibrationStore::save(cal);
            }

            DEBUG_PRINT(F("[API] Active gates: "));
            DEBUG_PRINTLN(g_numActiveStops);
        }
    }

    // Reference-sweep calibration (dual endstop). Kicks off a home → sweep flow:
    // this just records the request + re-homes; the sweep motion runs in
    // STATE_HOMING → STATE_CALIBRATING. See docs/dual-endstop-calibration.md.
    {
        char model[16]; int gateCount = 0;
        if (apiServer.consumeCalibrateRequest(model, sizeof(model), gateCount)) {
            if (g_hardwareFault) {
                DEBUG_PRINTLN(F("[CAL] Hardware fault — fix wiring and reset first."));
            } else if (gateCount < 1 || gateCount > NUM_STOPS) {
                DEBUG_PRINTLN(F("[CAL] Bad gate count — ignored."));
            } else if (currentState == STATE_HOMING || currentState == STATE_CALIBRATING ||
                       currentState == STATE_MOVING) {
                DEBUG_PRINTLN(F("[CAL] Busy — retry when idle."));
            } else {
                if (!isKnownManifoldModel(model)) {
                    DEBUG_PRINT(F("[CAL] Unknown model '")); DEBUG_PRINT(model);
                    DEBUG_PRINTLN(F("' — treating as custom (span only, no auto-placement)."));
                }
                strlcpy(g_calModel, model, sizeof(g_calModel));
                g_calGateCount     = physicalGateCount(model, gateCount);
                if (g_calGateCount != gateCount) {
                    DEBUG_PRINT(F("[CAL] Rounded to ")); DEBUG_PRINT(g_calGateCount);
                    DEBUG_PRINTLN(F(" gates (manifold ships in pairs; extra is a spare)."));
                }
                g_calibratePending = true;
                // Match the serial path: clear a latched e-stop and ensure the
                // driver is powered so calibrate works even from ERROR/idle-sleep.
                g_eStopTriggered   = false;
                motor.enable(true);
                DEBUG_PRINT(F("[CAL] Requested: ")); DEBUG_PRINT(model);
                DEBUG_PRINT(F(" x")); DEBUG_PRINT(gateCount);
                DEBUG_PRINTLN(F(" — homing, then sweeping to far endstop."));
                startHoming();
                currentState = STATE_HOMING;
            }
        }
    }

    // Port-role change (dual endstop / topology).
    {
        int roleIdx = -1, roleVal = 0;
        if (apiServer.consumePortRoleRequest(roleIdx, roleVal)) {
            if (roleIdx >= 1 && roleIdx <= NUM_STOPS) {
                g_stopRoles[roleIdx] = (uint8_t)roleVal;
                CalibrationData cal;
                if (CalibrationStore::load(cal)) {
                    cal.stopRole[roleIdx] = (uint8_t)roleVal;
                    CalibrationStore::save(cal);
                }
                DEBUG_PRINT(F("[API] Port role: gate ")); DEBUG_PRINT(roleIdx);
                DEBUG_PRINT(F(" = ")); DEBUG_PRINTLN(roleVal);
            }
        }
    }

    // Home-side answer (POST /api/config/orientation {homedLeft}). Ensures the home
    // datum is the user's LEFT endstop, re-homing if the carriage came up on the right.
    {
        bool homedLeft = true;
        if (apiServer.consumeOrientationRequest(homedLeft)) {
            setHomedLeft(homedLeft);
        }
    }

#ifdef ENABLE_HTTP_API
    // Servo jog (POST /api/servo/jog) — the gate configurator driving one servo so
    // the user can watch the valve and capture where it lands. Channel is range-checked
    // in the handler; ServoActuator does the easing and the deferred detach.
    //
    // A jog carrying a controllerId belongs to a SECONDARY and is relayed over
    // NodeLink. It must be dispatched by id, not by channel: the primary and every
    // node number their channels 0-3, so a gate moved to a node kept jogging the
    // primary's servo on the same channel — the configurator looked functional
    // while the valve being calibrated never twitched.
    {
        int jogCh = 0, jogAngle = 0; bool jogDetach = false; String jogCtrl;
        if (apiServer.consumeServoJogRequest(jogCh, jogAngle, jogDetach, jogCtrl)) {
            bool remote = jogCtrl.length() > 0 &&
                          jogCtrl != g_nodeBus.ownControllerId().c_str();
            if (remote) {
                topo::RemoteActuatorBus* bus = nullptr;
                for (int i = 0; i < g_remoteCount; i++) {
                    if (jogCtrl == g_remoteBuses[i].nodeId()) { bus = &g_remoteBuses[i]; break; }
                }
                if (!bus) {
                    DEBUG_PRINT(F("[UI] Jog for unknown controller: ")); DEBUG_PRINTLN(jogCtrl);
                } else if (jogDetach) {
                    // No detach over the wire — the node de-energizes on its own once
                    // the sweep settles (holdAtRest is false on a jog). Nothing to do.
                } else if (!bus->jog(jogCh, jogAngle)) {
                    DEBUG_PRINT(F("[UI] Jog refused by ")); DEBUG_PRINTLN(jogCtrl);
                }
            }
#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)
            else if (jogDetach) g_servos[jogCh].detach();
            else                g_servos[jogCh].moveTo(jogAngle);
#endif
        }
    }
#endif

    // -- Manual tool switch (POST /api/tool) ----------------------------------
    // The Live view's tool rows. Runs before the runtime is pumped below, so a tap
    // is acted on in the same pass rather than the next one.
#ifdef ENABLE_HTTP_API
    {
        String toolId; bool toolOn = false;
        if (apiServer.consumeToolManualRequest(toolId, toolOn)) {
            bool known = g_topoRuntime.setToolManual(std::string(toolId.c_str()), toolOn);
            DEBUG_PRINT(F("[UI] Manual tool ")); DEBUG_PRINT(toolId);
            if (!known) DEBUG_PRINTLN(F(" — NO SUCH TOOL in the layout (ignored)"));
            else        DEBUG_PRINTLN(toolOn ? F(" ON") : F(" off"));
        }
    }
#endif

    // -- Manual blower switch (POST /api/collector) ---------------------------
    // The Live view's collector card. Through the RUNTIME, not through
    // SmartOutletControl: the collector assert further down drives every slot from
    // g_topoRuntime.collectorOn(), so a plug switched directly here would be
    // switched back on the same pass. See HttpApiServer's route comment.
#ifdef ENABLE_HTTP_API
    {
        String sysId; bool blowerOn = false;
        if (apiServer.consumeCollectorManualRequest(sysId, blowerOn)) {
            std::string id = sysId.c_str();
            if (id.empty()) {
                // No system named — a shop with one blower has one answer.
                std::vector<std::string> ids = g_topoRuntime.systemIds();
                if (!ids.empty()) id = ids.front();
            }
            bool known = g_topoRuntime.setCollectorManual(id, blowerOn);
            DEBUG_PRINT(F("[UI] Manual blower ")); DEBUG_PRINT(id.c_str());
            if (!known) DEBUG_PRINTLN(F(" — NO SUCH SYSTEM in the layout (ignored)"));
            else        DEBUG_PRINTLN(blowerOn ? F(" ON") : F(" off"));
        }
    }
#endif

    // -- Pair / un-pair a secondary (POST /api/nodes/pair) --------------------
    // Registry write + redial, on the main loop because dialling spawns a task and
    // tears sockets down. Independent of the topology by design (NodeRegistry.h).
#ifdef ENABLE_HTTP_API
    {
        String pairHost, pairName; bool pairRemove = false, pairTakeover = false;
        if (apiServer.consumeNodePairRequest(pairHost, pairName, pairRemove, pairTakeover)) {
            bool changed = pairRemove ? g_nodeRegistry.remove(pairHost.c_str())
                                      : g_nodeRegistry.add(pairHost.c_str(), pairName.c_str());
            DEBUG_PRINT(pairRemove ? F("[NODE] Un-pair ") : F("[NODE] Pair "));
            DEBUG_PRINT(pairHost);
            DEBUG_PRINTLN(changed ? F(" — ok") : F(" — refused (full, or not paired)"));

            // A TAKEOVER rides the NEXT HELLO, so it is recorded here and armed
            // inside syncPairedNodes() just before that bus dials. One-shot by
            // construction (RemoteActuatorBus clears it as it sends), so a
            // reconnect loop can never repeat the claim — the user's
            // confirmation buys exactly one attempt.
            if (changed && pairTakeover && !pairRemove) g_pendingTakeoverHost = pairHost;
            if (changed) {
                syncPairedNodes(WiFiProvisioner::getHostname().c_str());
                // Re-resolve controllerId→host: a topology naming this board was
                // unresolvable while it was unpaired, and should start working now.
                if (g_topoRuntime.loaded()) syncControllerAliases();
            }
        }
    }
#endif

    // -- Node discovery (GET /api/nodes/discover) -----------------------------
    // Runs here, on the main loop task, for the same reason outlet discovery does
    // (see that block below): mDNS blocks, and holding an async request object
    // across a blocking scan on a detached task risks a use-after-free.
#ifdef ENABLE_HTTP_API
    if (apiServer.consumeNodeDiscoverRequest()) {
        DynamicJsonDocument doc(2048);
        JsonArray out = doc.to<JsonArray>();

        static const int kMaxNodeHits = 8;
        MdnsHit hits[kMaxNodeHits];
        // Same window as outlet discovery, and for the same reason — see
        // DISCOVER_MDNS_TIMEOUT_MS in config.h. This path had its own hand-tuned
        // number (three 800ms attempts, itself a bump from two 600ms ones that
        // were "marginal") and kept losing the same argument: a node that
        // answers, but not inside the window, reads as "no boards found" with
        // nothing to distinguish it from an empty network. Tuning it twice
        // without fixing the shape is what made it worth sharing the constant.
        for (int attempt = 0; attempt < DISCOVER_MDNS_ATTEMPTS; attempt++) {
            int n = mdnsQueryDustgateTcp(DISCOVER_MDNS_TIMEOUT_MS, hits, kMaxNodeHits);
            DEBUG_PRINT(F("[NODES] attempt ")); DEBUG_PRINT(attempt + 1);
            DEBUG_PRINT(F(": ")); DEBUG_PRINT(n); DEBUG_PRINTLN(F(" hit(s)"));
            for (int i = 0; i < n; i++) {
                // Logged individually: a hit REJECTED for its role looks exactly
                // like no hit at all in the response, and that distinction is the
                // whole difference between "node is silent" and "node answered but
                // its TXT records didn't survive the query".
                DEBUG_PRINT(F("  host=")); DEBUG_PRINT(hits[i].hostname);
                DEBUG_PRINT(F(" ip=")); DEBUG_PRINT(hits[i].ip);
                DEBUG_PRINT(F(" role=")); DEBUG_PRINT(hits[i].role);
                DEBUG_PRINT(F(" board=")); DEBUG_PRINTLN(hits[i].board);
                // Only offer actual secondaries. A primary advertising itself (or
                // another shop's brain) must never appear as an actuator target.
                if (hits[i].role != "secondary") continue;
                bool seen = false;
                for (JsonObject e : out) if (e["host"] == hits[i].hostname) seen = true;
                if (seen) continue;
                JsonObject e = out.createNestedObject();
                e["host"]   = hits[i].hostname;
                e["ip"]     = hits[i].ip;
                e["board"]  = hits[i].board;
                e["servos"] = hits[i].servos;
                // A board that belongs to another shop's primary is still worth
                // LISTING — hiding it makes a board that is right there, powered
                // and answering, look like it never showed up, and the two have
                // completely different fixes. Named, with the same two fields
                // GET /api/nodes already uses for a refused link, so the UI has
                // one vocabulary for "someone else has this" wherever it appears.
                //
                // Ours doesn't count: a node this board already owns is simply a
                // node you can pair, and the paired list filters it out anyway.
                const String& owner = hits[i].owner;
                if (owner.length() &&
                    !owner.equalsIgnoreCase(WiFiProvisioner::getHostname())) {
                    e["claimedBy"] = owner;
                    e["takeable"]  = true;
                }
            }
            if (attempt < 2) delay(150);
        }
        String body; serializeJson(doc, body);
        apiServer.respondNodeDiscover(body);
    }
#endif

    // -- NodeLink SECONDARY execution -------------------------------------------
    // This board acting as a dumb actuator bank for someone else's primary. The
    // frame already carries a channel and an absolute angle/mm — there is no
    // routing, no topology and no state lookup to do here, which is exactly the
    // asymmetry that lets a cheap servo-only board be a node.
    //
    // A board configured as a primary (topology loaded) shouldn't also be
    // receiving SETs; if it somehow is, both would command the same servos, so
    // the primary's own routing wins and remote SETs are refused.
    {
        static char pendingSel[48]   = "";
        static char pendingState[32] = "";
        static bool awaitingSettle   = false;

        topo::nodelink::SetCommand cmd;
        if (apiServer.consumeNodeSet(cmd)) {
            bool driven = false;
            if (g_topoRuntime.loaded()) {
                DEBUG_PRINTLN(F("[NODE] Refusing SET — this board is a primary."));
            } else if (cmd.isServo) {
#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)
                if (cmd.channel >= 0 && cmd.channel < SERVO_COUNT) {
                    g_servos[cmd.channel].setHoldAtRest(cmd.holdAtRest);
                    g_servos[cmd.channel].moveTo(cmd.angle);
                    driven = true;
                }
#endif
            } else {
                driven = g_linearDrive.moveToMm(cmd.positionMm);
            }

            if (driven) {
                strlcpy(pendingSel,   cmd.selectorId, sizeof(pendingSel));
                strlcpy(pendingState, cmd.stateId,    sizeof(pendingState));
                awaitingSettle = true;
                apiServer.reportNodeState(pendingSel, pendingState, true);
            } else {
                // Nothing moved, so report arrival immediately — otherwise the
                // primary would sit on a busy() bus until its move timeout.
                apiServer.reportNodeState(cmd.selectorId, cmd.stateId, false);
            }
        }

        // Report arrival once the actuator settles, so the primary's move queue
        // advances on real completion rather than on a fixed guess.
        if (awaitingSettle && !g_localBus.busy()) {
            awaitingSettle = false;
            apiServer.reportNodeState(pendingSel, pendingState, false);
        }
    }

    // Enable / disable — TODO: add ControlInput::setEnabled() to the base class
    // so this works for all modes, not just serial debug.

#ifdef CONTROL_SMART_OUTLET
    {
        HttpApiServer::OutletConfigCmd cmd;
        if (apiServer.consumeOutletConfigRequest(cmd)) {
            control.configureOutlet(cmd.slot, cmd.generation, cmd.ip, cmd.name,
                                    cmd.stopIndex, cmd.thresholdW, cmd.host);
        }
        int delSlot = -1;
        if (apiServer.consumeOutletDeleteRequest(delSlot)) {
            control.removeOutlet(delSlot);
        }
        if (apiServer.consumeOutletSaveRequest()) {
            control.saveAll();
        }

        HttpApiServer::DustCollectorCmd dcCmd;
        if (apiServer.consumeDustCollectorConfigRequest(dcCmd)) {
            control.configureDustCollector(dcCmd.generation, dcCmd.ip, dcCmd.host);
        }
        if (apiServer.consumeDustCollectorDeleteRequest()) {
            control.removeDustCollector();
        }
        bool dcSwitchOn = false;
        if (apiServer.consumeDustCollectorSwitchRequest(dcSwitchOn)) {
            control.setDcManual(dcSwitchOn);
        }

        // Outlet discovery — runs synchronously here (main loop task) rather
        // than in a spawned FreeRTOS task; see HttpApiServer.cpp's
        // /api/outlets/discover route comment. Blocks the main loop for the
        // duration of the scan (mDNS query + a couple short HTTP probes per
        // match), which is fine since this only runs on an explicit,
        // infrequent wizard action while the system is otherwise idle.
        if (apiServer.consumeDiscoverRequest()) {
            DynamicJsonDocument doc(2048);
            JsonArray results = doc.to<JsonArray>();

            // mDNS/UDP query is lossy — retry a few times and merge unique
            // hosts by IP, so a device only needs to answer once across all
            // attempts to show up in the final list.
            String hitIp[DISCOVER_MAX_RESULTS];
            String hitHost[DISCOVER_MAX_RESULTS];
            int    hitGen[DISCOVER_MAX_RESULTS];  // advertised "gen" (2, 3, ...); 1 = Gen1 candidate
            int    hitCount = 0;

            // Merge by IP. A device can answer on both services and across
            // several attempts; first sighting wins, but a later one may fill
            // in a hostname or upgrade an assumed Gen1 to its advertised gen.
            auto addHit = [&](const String& host, const String& ip, int gen) {
                if (ip.length() == 0 || ip == "0.0.0.0") return;  // SRV/A never resolved
                for (int j = 0; j < hitCount; j++) {
                    if (hitIp[j] != ip) continue;
                    if (gen > hitGen[j])          hitGen[j]  = gen;
                    if (hitHost[j].length() == 0) hitHost[j] = host;
                    return;
                }
                if (hitCount >= DISCOVER_MAX_RESULTS) return;
                hitIp[hitCount]   = ip;
                hitHost[hitCount] = host;
                hitGen[hitCount]  = gen;
                hitCount++;
            };

            // ── Pass 1: _shelly._tcp — Gen2+ only, and unambiguous ──────────
            // Only Shelly devices advertise this service, so every hit counts
            // with no hostname guessing (which used to break the moment a user
            // renamed a device in the Shelly app).
            for (int attempt = 0; attempt < DISCOVER_MDNS_ATTEMPTS; attempt++) {
                MdnsHit mdnsHits[DISCOVER_MAX_RESULTS];
                int n = mdnsQueryShellyTcp(DISCOVER_MDNS_TIMEOUT_MS, mdnsHits, DISCOVER_MAX_RESULTS);
                DEBUG_PRINT(F("[DISCOVER] attempt "));
                DEBUG_PRINT(attempt + 1);
                DEBUG_PRINT(F("/"));
                DEBUG_PRINT(DISCOVER_MDNS_ATTEMPTS);
                DEBUG_PRINT(F(": mDNS _shelly._tcp query returned "));
                DEBUG_PRINT(n);
                DEBUG_PRINTLN(F(" host(s):"));

                for (int i = 0; i < n; i++) {
                    // Service is Shelly-exclusive; missing TXT just means an
                    // older Gen2 firmware, so assume the Gen2 RPC dialect.
                    int gen = mdnsHits[i].gen > 0 ? mdnsHits[i].gen : 2;
                    DEBUG_PRINT(F("  - "));
                    DEBUG_PRINT(mdnsHits[i].hostname.length() ? mdnsHits[i].hostname : String("(no hostname)"));
                    DEBUG_PRINT(F("  "));
                    DEBUG_PRINT(mdnsHits[i].ip);
                    DEBUG_PRINT(F("  [shelly gen="));
                    DEBUG_PRINT(gen);
                    DEBUG_PRINTLN(F("]"));
                    addHit(mdnsHits[i].hostname, mdnsHits[i].ip, gen);
                }

                if (attempt < DISCOVER_MDNS_ATTEMPTS - 1) delay(DISCOVER_MDNS_RETRY_DELAY_MS);
            }

            // (Gen1 is not supported, so there's no _http._tcp fallback pass —
            // _shelly._tcp above is the sole, unambiguous source.)

            DEBUG_PRINT(F("[DISCOVER] "));
            DEBUG_PRINT(hitCount);
            DEBUG_PRINTLN(F(" unique host(s) across all attempts — probing:"));

            for (int i = 0; i < hitCount; i++) {
                const String& ip   = hitIp[i];
                const String& host = hitHost[i];

                // Gen2+ only (Gen1 dropped); Gen3 shares the Gen2 RPC dialect,
                // so a single Gen2 probe covers every supported device.
                int apiGen = (hitGen[i] >= 3) ? 3 : 2;
                ShellyGen2Outlet probe(ip.c_str(), "discover");
                bool  ok = probe.poll();
                float pw = probe.getPowerW();

                String devName = ok ? fetchShellyDeviceName(ip.c_str(), apiGen) : String();
                DEBUG_PRINT(F("  - ")); DEBUG_PRINT(host); DEBUG_PRINT(F("  "));
                DEBUG_PRINT(ip);
                DEBUG_PRINT(F("  probe -> reachable="));
                DEBUG_PRINT(ok ? F("yes") : F("no"));
                DEBUG_PRINT(F(" gen="));
                DEBUG_PRINT(ok ? apiGen : 0);
                DEBUG_PRINT(F(" name="));
                DEBUG_PRINTLN(devName.length() ? devName : String("(none set)"));

                // WHO OWNS IT (RFC §8). Asked here, at discovery, because this
                // is the list someone picks from — a plug that belongs to
                // another brain has to arrive already labelled, not fail
                // mysteriously after being chosen.
                String wsServer; bool wsEnabled = false;
                const bool claimKnown = ok && probe.readPushConfig(wsServer, wsEnabled);
                plugclaim::Claim claim;
                if (claimKnown) {
                    claim = plugclaim::decide(wsServer.c_str(), wsEnabled,
                                              control.ourHost(), devName.c_str(),
                                              control.ourName());
                }

                JsonObject o = results.createNestedObject();
                o["ip"]        = ip;
                o["hostname"]  = host;
                // The name with any "· owner" suffix stripped: the suffix is our
                // bookkeeping and would otherwise show up in the picker as part
                // of the tool's name, then get saved back and doubled.
                o["name"]      = claimKnown ? String(claim.label.c_str()) : devName;
                o["reachable"] = ok;
                o["powerW"]    = pw;
                o["gen"]       = ok ? apiGen : 0;
                if (claimKnown) {
                    o["claim"]    = plugclaim::stateName(claim.state);
                    o["owner"]    = claim.owner;
                    o["holder"]   = claim.holder;
                    o["pickable"] = claim.pickable;
                    o["takeable"] = claim.takeable;
                    if (!claim.reason.empty()) o["claimReason"] = claim.reason;
                } else {
                    // "We couldn't ask" is its own answer, and must not read as
                    // "nobody owns it" — that reading is how a plug gets taken.
                    o["claim"]    = "unknown";
                    o["pickable"] = false;
                    o["takeable"] = false;
                    o["claimReason"] = "couldn't read its push config";
                }
            }
            if (hitCount == 0) {
                DEBUG_PRINTLN(F("  (no Shelly devices on either _shelly._tcp or _http._tcp — check the "
                                 "outlets are powered, joined to WiFi, and that mDNS is enabled in the "
                                 "Shelly app's device settings)"));
            }

            String out; serializeJson(doc, out);
            apiServer.respondDiscover(out);
        }
    }

    // Outlet ping — probe a single IP on the main loop (see consumePingRequest
    // in HttpApiServer for why it's here rather than a spawned task).
    {
        char pingIp[40];
        if (apiServer.consumePingRequest(pingIp, sizeof(pingIp))) {
            // Gen2+ only (Gen1 dropped): this project's supported hardware
            // (e.g. the reference Plug US G4) speaks the Gen2/RPC dialect.
            ShellyGen2Outlet gen2(pingIp, "ping");
            bool  ok  = gen2.poll();
            float pw  = gen2.getPowerW();
            int   gen = 2;
            String devName = ok ? fetchShellyDeviceName(pingIp, gen) : String();

            DEBUG_PRINT(F("[PING] ")); DEBUG_PRINT(pingIp);
            DEBUG_PRINT(F(" -> reachable=")); DEBUG_PRINT(ok ? F("yes") : F("no"));
            DEBUG_PRINT(F(" gen=")); DEBUG_PRINT(ok ? gen : 0);
            DEBUG_PRINT(F(" name=")); DEBUG_PRINTLN(devName.length() ? devName : String("(none set)"));

            StaticJsonDocument<192> resp;
            resp["reachable"] = ok;
            resp["powerW"]    = pw;
            resp["gen"]       = ok ? gen : 0;
            resp["name"]      = devName;  // app-assigned Shelly device name, "" if unset
            String out; serializeJson(resp, out);
            apiServer.respondPing(out);
        }
    }

    // Plug TAKEOVER (RFC §8) — the user was shown what breaks and said yes.
    //
    // All this does is set a one-shot approval on the outlet; the actual
    // Ws.SetConfig happens on the next provisioning pass, through exactly the
    // same code path as an ordinary plug. There is no second way to repoint a
    // plug, which is what keeps "never steal" true everywhere else.
    {
        char takeIp[40];
        if (apiServer.consumeTakeoverRequest(takeIp, sizeof(takeIp))) {
            // Recorded against the ADDRESS, not against an outlet object. A plug
            // that isn't paired with anything yet has no outlet — and used to be
            // refused here, which sent the user off to pair it and come back —
            // and an outlet that DOES exist is rebuilt from scratch by the next
            // topology adopt, which is what pairing performs. Both cases now
            // land the same way: the approval waits at the address until a plug
            // there is provisioned. See SmartOutletControl::approveTakeoverByIp.
            control.approveTakeoverByIp(takeIp);
            control.requestProvision();   // don't wait for the next reconcile
            DEBUG_PRINT(F("[TAKEOVER] Approved for ")); DEBUG_PRINT(takeIp);
            DEBUG_PRINTLN(F(" — its push target is repointed on the next pass, "
                            "whenever a plug there is provisioned."));
        }
    }

    // ------------------------------------------------------------------
    // Plug RENAME — POST /api/outlets/name.
    //
    // On the main loop for the same reason ping is: Switch.SetConfig blocks on a
    // device that may not answer.
    //
    // The suffix rule is the interesting part. A plug that is already OURS gets
    // "<label> · <our mDNS name>", because in the Shelly app there is no DustGate
    // UI to explain why a plug is spoken for. A plug that is merely UNCLAIMED —
    // renamed from the tray before it is paired with any tool — gets the bare
    // label: stamping ownership on a plug we have not taken would be a claim we
    // have not earned, and pairing will add the suffix when it is true.
    // ------------------------------------------------------------------
    {
        char nameIp[40], nameLabel[48];
        bool nameTakeover = false;
        if (apiServer.consumeOutletNameRequest(nameIp, sizeof(nameIp),
                                               nameLabel, sizeof(nameLabel),
                                               nameTakeover)) {
            StaticJsonDocument<256> resp;
            ShellyGen2Outlet plug(nameIp, "rename");

            if (!plug.poll()) {
                resp["ok"]    = false;
                resp["error"] = "not responding";
                DEBUG_PRINT(F("[RENAME] ")); DEBUG_PRINT(nameIp);
                DEBUG_PRINTLN(F(" is not answering — name unchanged."));
            } else {
                String  wsServer; bool wsEnabled = false;
                String  devName = fetchShellyDeviceName(nameIp, 2);
                const bool claimKnown = plug.readPushConfig(wsServer, wsEnabled);
                plugclaim::Claim claim;
                if (claimKnown) {
                    claim = plugclaim::decide(wsServer.c_str(), wsEnabled,
                                              control.ourHost(), devName.c_str(),
                                              control.ourName());
                }

                // Same rule that governs repointing, applied to the name: never
                // write a plug someone else owns — UNLESS a human said to. That
                // is what `nameTakeover` is, and mayRepoint()'s `confirmed`
                // argument has always existed for exactly this shape of answer.
                //
                // A read we could not make is "we don't know", and stays a no in
                // both cases: overriding a refusal is a decision about a KNOWN
                // owner, and we cannot show the user whose plug it is if we could
                // not ask. Confirming a question nobody was able to pose is not
                // consent.
                //
                // Note this only ever writes the NAME. The plug keeps reporting to
                // whoever owns it; nothing over there stops working. Repointing is
                // a separate, louder act — POST /api/outlets/takeover.
                if (!claimKnown || !plugclaim::mayRepoint(claim, nameTakeover)) {
                    // String(), not c_str(): ArduinoJson stores a bare const char*
                    // BY REFERENCE, and claim/full die at the end of this block
                    // while serializeJson runs after it.
                    String why = claimKnown ? String(claim.reason.c_str())
                                            : String("could not read who owns this plug");
                    resp["ok"]    = false;
                    resp["error"] = why;
                    DEBUG_PRINT(F("[RENAME] Refused for ")); DEBUG_PRINT(nameIp);
                    DEBUG_PRINT(F(" — ")); DEBUG_PRINTLN(why);
                } else {
                    // The suffix says "this plug is being USED by that brain", so
                    // it goes on only when that is true. A plug renamed under an
                    // override still belongs to whoever it reports to, and an
                    // unclaimed plug renamed before pairing is not ours yet
                    // either — both get the bare label, and pairing adds the
                    // suffix later, when it has become true.
                    const bool ours  = (claim.state == plugclaim::State::Ours);
                    std::string full = ours
                        ? plugclaim::formatName(nameLabel, control.ourName())
                        : std::string(nameLabel);
                    bool ok = plug.setName(full.c_str());
                    resp["ok"]    = ok;
                    resp["name"]  = String(full.c_str());   // what landed, suffix and all
                    resp["label"] = String(nameLabel);
                    if (!ok) resp["error"] = "the plug refused the name";
                    DEBUG_PRINT(F("[RENAME] ")); DEBUG_PRINT(nameIp);
                    DEBUG_PRINT(F(" -> \"")); DEBUG_PRINT(full.c_str());
                    DEBUG_PRINT(F("\" ")); DEBUG_PRINTLN(ok ? F("ok") : F("FAILED"));

                    // Keep the in-memory outlet's label in step, so the next
                    // status push doesn't report the name we just replaced.
                    SmartOutlet* configured = control.outletByIp(nameIp);
                    if (ok && configured) configured->setName(full.c_str());
                }
            }
            String out; serializeJson(resp, out);
            apiServer.respondOutletName(out);
        }
    }

    // ------------------------------------------------------------------
    // Plug RELEASE — POST /api/outlets/release. The device half of unpairing.
    //
    // Best-effort BY DESIGN: the layout half (deleting sensor.outlet) is a plain
    // topology write the UI does regardless, because a plug you have unplugged is
    // exactly when you want to detach it. So this reports what it managed and
    // never blocks the unpair — see the endpoint comment in HttpApiServer.cpp.
    // ------------------------------------------------------------------
    {
        char relIp[40];
        if (apiServer.consumeOutletReleaseRequest(relIp, sizeof(relIp))) {
            StaticJsonDocument<256> resp;
            SmartOutlet* configured = control.outletByIp(relIp);

            // A poll-only plug was never written to — no suffix of ours on its
            // name, no Ws config we touched. Saying "released" would imply we
            // reached into someone else's device, which we did not.
            if (configured && configured->isPollOnly()) {
                resp["ok"]       = true;
                resp["released"] = false;
                resp["note"]     = "polled only — nothing was written to this plug";
                DEBUG_PRINT(F("[RELEASE] ")); DEBUG_PRINT(relIp);
                DEBUG_PRINTLN(F(" was poll-only — nothing to hand back."));
            } else {
                ShellyGen2Outlet plug(relIp, "release");
                if (!plug.poll()) {
                    resp["ok"]       = false;
                    resp["released"] = false;
                    resp["error"]    = "not responding";
                    DEBUG_PRINT(F("[RELEASE] ")); DEBUG_PRINT(relIp);
                    DEBUG_PRINTLN(F(" is not answering — it keeps our name and push target."));
                } else {
                    // Name first, then push — the same ordering pairing uses, and
                    // for the same reason: a Ws write makes the plug reopen its
                    // socket and a name write landing on top of that gets lost.
                    String devName = fetchShellyDeviceName(relIp, 2);
                    std::string lbl, own;
                    plugclaim::parseName(devName.c_str(), lbl, own);
                    bool nameOk = true;
                    if (!own.empty() && own == control.ourName()) {
                        nameOk = plug.setName(lbl.c_str());
                        delay(150);
                    }

                    const char* restore = configured ? configured->previousPushUrl() : "";
                    bool pushOk = plug.releasePush(restore);

                    resp["ok"]       = nameOk && pushOk;
                    resp["released"] = true;
                    resp["restored"] = (restore && *restore);
                    if (!nameOk) resp["error"] = "the plug kept our name";
                    else if (!pushOk) resp["error"] = "the plug kept pushing to us";
                    DEBUG_PRINT(F("[RELEASE] ")); DEBUG_PRINT(relIp);
                    DEBUG_PRINT(F(" name=")); DEBUG_PRINT(nameOk ? F("ok") : F("FAILED"));
                    DEBUG_PRINT(F(" push=")); DEBUG_PRINTLN(pushOk ? F("ok") : F("FAILED"));
                }
            }
            String out; serializeJson(resp, out);
            apiServer.respondOutletRelease(out);
        }
    }
#endif // CONTROL_SMART_OUTLET

    // ------------------------------------------------------------------
    // Idle power-off — only while genuinely at rest (home or a gate), never
    // mid-move/homing. Reuses the existing "not homed" gating (currentStop ==
    // -1) to force a rehome on the next move instead of adding a new state.
    // ------------------------------------------------------------------
    {
        int timeoutSec = apiServer.idleTimeoutSec();
        if (timeoutSec > 0 && !g_driverAsleep &&
            (currentState == STATE_IDLE || currentState == STATE_AT_STOP) &&
            currentStop != -1 &&
            (millis() - g_lastActivityMs) > (unsigned long)timeoutSec * 1000UL) {
            motor.enable(false);
            currentStop      = -1;
            currentState     = STATE_IDLE;
            g_driverAsleep   = true;
            g_notHomedWarnShown = false;
            DEBUG_PRINT(F("[Power] Idle ")); DEBUG_PRINT(timeoutSec);
            DEBUG_PRINTLN(F("s — driver powered off. Home to resume."));
        }
    }
#endif // ENABLE_HTTP_API

    // Detect the start of a fresh homing sequence so auto motor-direction detect
    // gets one flip per sequence (a re-home triggered by the flip itself stays in
    // STATE_HOMING and must not reset the guard).
    {
        static State prevState = STATE_STARTUP;
        if (currentState == STATE_HOMING && prevState != STATE_HOMING) g_homeDirCorrected = false;
        prevState = currentState;
    }

    switch (currentState) {

        case STATE_STARTUP:
            break;

        // ------------------------------------------------------------------
        case STATE_HOMING:

            // Auto motor-direction detect: homing must drive toward the HOME DATUM.
            // If the FAR endstop trips instead, the motor is wired backwards — flip
            // the direction, persist, and re-home once. A second far-hit is a real
            // fault (wiring), so error out rather than loop.
            if (farSwitchTriggered() && !datumSwitchTriggered()) {
                motor.stop();
                if (!g_homeDirCorrected) {
                    g_homeDirCorrected = true;
                    g_homeDirection = -g_homeDirection;
                    persistHomeDirection();
                    DEBUG_PRINTLN(F("[HOME] Far endstop hit while homing — motor was backwards. Flipped direction, re-homing."));
                    startHoming();
                } else {
                    DEBUG_PRINTLN(F("[HOME] Far endstop hit again after flip — check endstop/motor wiring."));
                    currentState = STATE_ERROR;
                }
                break;
            }

            if (feedback.updateHoming()) {
                currentStop = 0;
                if (g_calibratePending) {
                    // Homed → begin the reference sweep. Drive well past the
                    // largest plausible span toward the far end; the far endstop
                    // trips first (detected in STATE_CALIBRATING).
                    float pitch = manifoldPitchMm(g_calModel);
                    if (pitch <= 0.0f) pitch = MANIFOLD_2_5_GATE_PITCH_MM; // custom bound
                    float boundMm = 10.0f + (float)g_calGateCount * pitch + 40.0f;
                    long boundTarget = (long)(boundMm * stepsPerMM()) * (-HOME_DIRECTION);
                    // Sweep at the gentler homing speed — safer approach to the far
                    // switch. Restored to MAX_SPEED before the return-home move.
                    motor.setMaxSpeed(HOMING_SPEED_STEPS_PER_SEC);
                    motor.moveTo(boundTarget);
                    DEBUG_PRINT(F("[CAL] Homed. Sweeping to far endstop (bound "));
                    Serial.print(boundMm, 0); DEBUG_PRINTLN(F("mm)..."));
                    currentState = STATE_CALIBRATING;
                } else {
                    DEBUG_PRINTLN(F("Homed. Entering IDLE."));
                    currentState = STATE_IDLE;
                }
            }
            break;

        // ------------------------------------------------------------------
        case STATE_CALIBRATING: {
            // Sweeping toward the far endstop (moveTo issued when homing finished).
            // The endstop safety supervisor is disabled for this state so we can
            // detect the trigger ourselves. On trigger: record span, place gates,
            // then return to the home datum.
            static unsigned long calStart = 0;
            static uint8_t farHighStreak = 0;
            if (calStart == 0) { calStart = millis(); farHighStreak = 0; }

            // Debounce the far switch: only accept the trigger after it reads HIGH
            // for CAL_FAR_CONFIRM_LOOPS consecutive loops. A single spurious NC-open
            // bounce (carriage vibration) would otherwise latch the sweep at a short
            // farPos and mis-place every gate. Loop runs fast, so the confirmation
            // delay is negligible against the sweep travel.
            static const uint8_t CAL_FAR_CONFIRM_LOOPS = 3;
            if (farSwitchTriggered()) {   // far end = opposite the home datum
                if (farHighStreak < 255) farHighStreak++;
            } else {
                farHighStreak = 0;
            }

            if (farHighStreak >= CAL_FAR_CONFIRM_LOOPS) {   // far endstop confirmed
                farHighStreak = 0;
                long farPos = motor.getPosition();
                motor.stop();
                DEBUG_PRINT(F("[CAL] Far endstop at pos=")); DEBUG_PRINTLN(farPos);
                finishCalibrationSweep(farPos);
                g_calibratePending = false;
                calStart = 0;
                // Restore normal move speed for the return-home and all later moves.
                motor.setMaxSpeed(MAX_SPEED_STEPS_PER_SEC);
                // Return to the home datum (releases the far switch on the way).
                targetStop = 0;
                issueMove(0);
            } else if (!motor.isMoving() || (millis() - calStart > 45000UL)) {
                // Reached the bound or timed out without the far switch — fault.
                motor.stop();
                DEBUG_PRINTLN(F("[CAL] Far endstop not found — calibration aborted."));
                g_calibratePending = false;
                calStart = 0;
                motor.setMaxSpeed(MAX_SPEED_STEPS_PER_SEC); // restore normal speed
                currentState = STATE_ERROR;
            }
            break;
        }

        // ------------------------------------------------------------------
        case STATE_IDLE: {
            bool enabled = control.isEnabled();
            int requested = control.readRequestedStop();

            if (!enabled) {
                if (kRackPresent && currentStop != 0 && currentStop != -1) {
                    targetStop = 0;
                    issueMove(0);
                }
                break;
            }

            // Nothing below this line concerns a board without a rack: it is all
            // the pre-topology outlet→stop path, which moves a carriage. Servo
            // gates and the routing runtime are driven from loop() above and are
            // unaffected.
            if (!kRackPresent) break;

            // Not homed yet — require explicit 'home' command before accepting moves
            if (currentStop == -1) {
                if (!g_notHomedWarnShown) {
                    DEBUG_PRINTLN(F("Not homed. Type 'home' before issuing position commands."));
                    g_notHomedWarnShown = true;
                }
                break;
            }

            // Outlet→stop automation is suppressed once a topology is loaded:
            // the runtime drives the rack from routed mm positions, and letting
            // both command the motor would have them fight over it.
            if (!g_topoRuntime.loaded() && requested != currentStop && requested >= 0) {
                targetStop = requested;
                issueMove(targetStop);
            }
            break;
        }

        // ------------------------------------------------------------------
        case STATE_MOVING:
            if (!control.isEnabled()) {
                motor.stop();
                currentStop = -1;
                currentState = STATE_IDLE;
                DEBUG_PRINTLN(F("Disabled mid-move — stopped."));
                break;
            }

            // Far/home over-travel is enforced directionally by the always-on
            // endstop supervisor at the top of loop() (stops travel *into* a
            // triggered switch, allows travel away). updateMoving() just reports
            // arrival — which is also true once the supervisor has stopped us.
            if (feedback.updateMoving(targetStop)) {
                currentStop = targetStop;
                DEBUG_PRINT(F("Arrived at stop "));
                DEBUG_PRINTLN(currentStop);
                currentState = (currentStop == 0) ? STATE_IDLE : STATE_AT_STOP;
            }
            break;

        // ------------------------------------------------------------------
        case STATE_AT_STOP: {
            bool enabled = control.isEnabled();
            int requested = control.readRequestedStop();

            if (!enabled) {
                if (kRackPresent) { targetStop = 0; issueMove(0); }
                else              { currentState = STATE_IDLE; }
                break;
            }

            if (!kRackPresent) break;   // see STATE_IDLE

            // See STATE_IDLE: routing owns the rack while a topology is loaded.
            if (!g_topoRuntime.loaded() && requested != currentStop && requested >= 0) {
                targetStop = requested;
                issueMove(targetStop);
            }
            break;
        }

        // ------------------------------------------------------------------
        case STATE_DISABLED:
            break;

        // ------------------------------------------------------------------
        case STATE_ERROR:
            motor.stop();
            motor.enable(false);

            // Physical recovery: toggle switch cycled (no hardware e-stop to release).
            //
            // STATE_ERROR means two different things — an e-stop (recoverable
            // here) and a boot-time hardware fault (NOT recoverable; g_hardwareFault
            // is latched until reset). Without the fault check a board that failed
            // begin() silently re-enabled the motor and homed itself on the first
            // tick, then refused every later API move — motion that visibly works
            // paired with "Hardware fault — reset before homing".
#ifndef CONTROL_SERIAL_DEBUG
            if (!g_hardwareFault &&
                !g_eStopTriggered &&
                control.isEnabled()) {
                motor.enable(true);
                currentState = STATE_HOMING;
                startHoming();
                DEBUG_PRINTLN(F("E-stop cleared. Re-homing..."));
            }
#endif
            break;
    }

#ifdef ENABLE_HTTP_API
    {
        const char* stateStr = "UNKNOWN";
        switch (currentState) {
            case STATE_STARTUP:    stateStr = "STARTUP";    break;
            case STATE_HOMING:     stateStr = "HOMING";     break;
            case STATE_IDLE:       stateStr = "IDLE";       break;
            case STATE_MOVING:     stateStr = "MOVING";     break;
            case STATE_AT_STOP:    stateStr = "AT_STOP";    break;
            case STATE_CALIBRATING: stateStr = "CALIBRATING"; break;
            case STATE_DISABLED:   stateStr = "DISABLED";   break;

            case STATE_ERROR:      stateStr = "ERROR";      break;
        }
        ApiStatus s;
        s.stateName      = stateStr;
        s.currentStop    = currentStop;
        s.targetStop     = targetStop;
        s.positionSteps  = motor.getPosition();
        // Same sign convention as consumeSetStopRequest's currentMM below:
        // HOME_DIRECTION inverts the step sign so positive mm is away from home.
        s.positionMM     = (float)s.positionSteps / stepsPerMM() / (-HOME_DIRECTION);
        s.homed          = (currentStop != -1);
        s.enabled        = control.isEnabled();
        s.endstopHome    = readEndstopHome();   // HIGH = triggered (NC switch open)
        s.endstopMax     = readEndstopMax();    // both false on a board with no rack
        s.numActiveStops = g_numActiveStops;
        s.measuredStepsPerMM = g_measuredStepsPerMM;
        s.measuredSpanSteps  = g_measuredSpanSteps;
        s.manifoldModel      = g_manifoldModel;
#ifdef CONTROL_SMART_OUTLET
        apiServer.update(s, &control);
#else
        apiServer.update(s);
#endif

        // Publish the live routing view for GET /api/status. Serialized
        // here, on the task that owns the routing state, so the async handler
        // only ever reads a finished string. Throttled — the routing view only
        // changes on a tool/plan event, not every loop pass.
        if (g_topoRuntime.loaded()) {
            static unsigned long lastPublishMs = 0;
            if (millis() - lastPublishMs >= V2_STATUS_PUBLISH_MS) {
                lastPublishMs = millis();
                DynamicJsonDocument out(4096);
                g_topoRuntime.writeStatus(out.to<JsonObject>());
                String body; serializeJson(out, body);
                apiServer.publishTopologyStatus(body);

            }
        }

        // Per-node link state for GET /api/nodes. Published OUTSIDE the
        // topology gate above, deliberately: pairing no longer depends on a
        // layout, so a paired board must be visible in the boards UI on a primary
        // that has never had a topology. Previously this lived inside
        // `if (g_topoRuntime.loaded())`, which made a freshly-flashed primary
        // report an empty node list — indistinguishable from nothing paired.
        {
            static unsigned long lastNodePublishMs = 0;
            if (millis() - lastNodePublishMs >= V2_STATUS_PUBLISH_MS) {
                lastNodePublishMs = millis();
                DynamicJsonDocument nodes(1024);
                JsonArray arr = nodes.createNestedArray("nodes");
                for (int i = 0; i < g_remoteCount; i++) {
                    topo::RemoteActuatorBus::NodeInfo n = g_remoteBuses[i].info();
                    JsonObject o = arr.createNestedObject();
                    o["id"]        = g_remoteBuses[i].nodeId();
                    o["host"]      = g_remoteBuses[i].host();
                    // From the registry, not the topology: the boards screen has to
                    // render names with no layout loaded at all.
                    o["name"]      = g_nodeRegistry.name(i);
                    o["online"]    = n.connected;
                    o["lastSeen"]  = n.lastSeenMs;
                    o["board"]     = n.board;
                    o["fw"]        = n.fw;
                    JsonObject caps = o.createNestedObject("caps");
                    caps["servos"] = n.capServos;
                    caps["linear"] = n.capLinear;
                    // A node that belongs to ANOTHER primary is offline to us on
                    // purpose. Without naming its owner here, that is
                    // indistinguishable from a dead board — and the difference
                    // decides whether you go looking for a wiring fault or press
                    // "take it over".
                    if (g_remoteBuses[i].wasRefused()) {
                        o["claimedBy"] = g_remoteBuses[i].refusedBy();
                        o["takeable"]  = true;
                    }
                }
                String nodeBody; serializeJson(nodes, nodeBody);
                apiServer.publishNodeStatus(nodeBody);
            }
        }
    }
#endif // ENABLE_HTTP_API

    // Last thing each pass: the indicator reflects the state the loop just
    // finished settling, not the one it started with.
    updateStatusLed();
}

// =============================================================================
// startHoming() — begin the homing sweep toward the near endstop.
// =============================================================================
void startHoming() {
    g_lastActivityMs = millis();
    g_driverAsleep   = false;
    // Guarantee normal move speed as a baseline. The calibration sweep lowers
    // maxSpeed to homing speed and restores it on its own exits — but a global
    // e-stop during the sweep goes straight to STATE_ERROR without restoring it,
    // so a plain re-home here would otherwise leave every later move crawling.
    motor.setMaxSpeed(MAX_SPEED_STEPS_PER_SEC);
    feedback.resetHoming(); // clear _homed so updateHoming() actually runs
    DEBUG_PRINT(F("[HOME] speed=")); Serial.print(HOMING_SPEED_STEPS_PER_SEC, 0);
    Serial.println(F(" steps/sec"));
    motor.startHoming();
}

// =============================================================================
// issueMove() — command motor to a stop position
// =============================================================================
void issueMove(int stop) {
    g_lastActivityMs = millis();
    DEBUG_PRINT(F("Moving to stop "));
    DEBUG_PRINTLN(stop);
    motor.moveTo(feedback.stepsForStop(stop));
    currentState = STATE_MOVING;
}

// Record which side the carriage homed to (the wizard's single "did it home to the
// left?" question). We ALWAYS want the home datum on the user's left:
//   homedLeft = true  → the current datum endstop is already the left one. Keep it.
//   homedLeft = false → it homed to the RIGHT, so make the OTHER endstop the datum
//                       (it's the left one) and flip the motor direction to reach it.
// The next home (the calibration sweep, or a plain home) then parks on the left, and
// gates (1..N from the datum) always read left→right. No re-home is issued here so it
// can't race the calibrate request the wizard sends right after.
void setHomedLeft(bool homedLeft) {
    if (!homedLeft) {
        g_homeIsMaxEndstop = !g_homeIsMaxEndstop;
        g_homeDirection    = -g_homeDirection;
        persistHomeDirection();
        DEBUG_PRINTLN(F("[CFG] Homed on the right — datum switched to the other (left) endstop; next home parks left."));
    } else {
        DEBUG_PRINTLN(F("[CFG] Home confirmed on the left."));
    }
    persistHomeDatum();
}

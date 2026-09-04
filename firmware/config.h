// =============================================================================
// config.h — DustGate Configuration
// Target hardware: Seeed XIAO ESP32C5 — primary or node, decided at flash time
// =============================================================================
// All project settings live here. Change values to match your build.
// Recompile after any change.
// =============================================================================

#pragma once

// -----------------------------------------------------------------------------
// MOTOR AND FEEDBACK TYPE — derived from the pin map. See BOARD CAPABILITIES
// below. No board has either today: the stepper and limit-switch feedback are in
// firmware/attic/linear/, awaiting the ST3215 slider. What is live is the SEAM —
// MotorDriver / FeedbackSystem and their null implementations.

// -----------------------------------------------------------------------------
// CONTROL INPUT — select exactly one
// -----------------------------------------------------------------------------
#define CONTROL_SMART_OUTLET      // Shelly smart outlet polling (auto gate selection)
// #define CONTROL_SERIAL_DEBUG    // Serial Monitor — open at SERIAL_BAUD, type 'help'

// -----------------------------------------------------------------------------
// HTTP API — enable to run the REST + WebSocket server alongside any control mode
// Required for the Angular front-end.
// Requires WiFi (set WIFI_STA_SSID / WIFI_STA_PASS, or use the setup portal).
// -----------------------------------------------------------------------------
#define ENABLE_HTTP_API

// -----------------------------------------------------------------------------
// POSITION CONFIGURATION
// -----------------------------------------------------------------------------
// Compile-time maximum — sets array sizes in CalibrationData and g_stopPositionsMM.
// The runtime count (g_numActiveStops) is set during setup and stored in NVS.
// Bumping this requires clearing calibration (clearcal) because CalibrationData changes size.
#define NUM_STOPS         16      // max selectable positions (position 0 = home)

// Minimum spacing (mm) between two saved gate positions. Authoritative backstop
// against saving two gates on top of each other (e.g. "forgot to jog" — saving
// gate N+1 without having moved off gate N). The Angular UI does a friendlier,
// port-size-aware version of this check before it ever calls /api/setstop; this
// firmware check catches any client that skips the UI (curl, scripts). Kept
// small so it only rejects genuine near-duplicates, never legitimately tight
// real-world gate spacing. Home (stop 0) is excluded from the check.
#define MIN_STOP_SEPARATION_MM   10.0f

// -----------------------------------------------------------------------------
// Manifold profiles + reference-sweep calibration (dual endstop)
// See docs/dual-endstop-calibration.md. A profile gives the mm geometry of a
// known manifold, referenced to the near (home) endstop trigger. The reference
// sweep measures the endstop-to-endstop step span, derives steps/mm, and places
// every gate by PROPORTION of the measured span (immune to steps/mm error).
// Keep these in step with shared/device-model MANIFOLD_PROFILES.
// -----------------------------------------------------------------------------
// Rockler Dust Right 2.5" — MEASURED on the reference build. Symmetric. Two direct
// measurements: trigger-to-trigger span = 84.9mm at 2 gates, gate-to-gate pitch =
// 82.9mm → trigger→gate offset = (84.9−82.9)/2 = 1mm/side. span(N) = 2 + (N−1)·82.9.
// NB: HOME_BACKOFF_STEPS does NOT affect pitch (cancels); it only shifts the
// steps/mm span — the sweep must add HOME_BACKOFF_STEPS back to the home→far step
// count before dividing by span mm.
// firstGateOffset / endMargin are DOCUMENTATION now, not inputs. The firmware
// places gates by centring the array in the span it MEASURED, so the only
// profile number it reads is the pitch. They stay because the JS side keeps its
// own copies (MANIFOLD_PROFILES in device-model.js models a span from them) and
// because they record what the manifold is — but nothing here computes from
// them, and the one function that did (manifoldProfile(), the nominal-offset
// placement this replaced) was deleted on 2026-08-28.
#define MANIFOLD_2_5_FIRST_GATE_OFFSET_MM   1.0f
// Rockler Dust Right 2.5" — 83.57mm, CORRECTED ON HARDWARE 2026-08-28.
//
// It was 82.9 from two direct measurements on the reference build (trigger-to-
// trigger span 84.9mm at 2 gates, gate-to-gate pitch 82.9mm), and 82.9 is what a
// 4-gate rack showed to be wrong. Gates 2 and 3 landed dead on while gates 1 and
// 4 both sat ~1mm TOWARD THE CENTRE — a signature that can only be pitch, since
// placement centres the array in the measured span: the CENTRE comes from span +
// backoff, the SPREAD comes from pitch alone. A pitch error d appears as 1.5d at
// the outer gates and 0.5d at the inner ones, so 1mm outside with the inside
// clean means d ≈ 0.67mm.
//
// 83.57 = 82.9 + (2/3 × 1mm), which is the closed-loop trim rather than a
// measurement: the centre does not move when pitch changes, so the correction is
// two-thirds of the observed outer error. If the outer gates now sit ~1mm OUTSIDE
// instead, subtract two-thirds of that and go again.
//
// CONFIRMED BY THE TRIM PASS, same day: after hand-adjusting all four gates on
// the rack, gate 1 to gate 4 came to 250.90mm, which is 83.63mm per pitch
// against the 83.57 set here. That is the gate-1-to-gate-4 measurement this note
// used to ask for, and it agrees to 0.06mm. LEAVE IT — the individual
// gate-to-gate spans in the same pass scattered 82.61 / 84.53 / 83.76, so a
// ±1mm noise floor swamps that 0.06 and chasing it would be fitting to slop.
//
// THE 1mm/SIDE OFFSET BELOW IS NOW INCONSISTENT and left alone deliberately:
// (84.9 − 83.57)/2 = 0.67mm, so one of the two original measurements is off.
// It does not affect gate placement — the firmware centres in the SPAN it
// measures and never uses the offset — so correcting it on a guess would add a
// second unmeasured number to fix nothing.
//
// PAIR: shared/device-model/device-model.js MANIFOLD_PROFILES holds the same
// number. See the table in CLAUDE.md.
#define MANIFOLD_2_5_GATE_PITCH_MM          83.57f
#define MANIFOLD_2_5_END_MARGIN_MM          1.0f
// Rockler Dust Right 4": pitch derived from Rockler's 10" manifold width ÷ 2 gates
// = 5.000" = 127.0mm center-to-center (their 6.5"/2 = 3.25" ≈ measured 82.9mm on the
// 2.5" build confirms the width-per-gate method). Same rack pitch + endstop margin as
// the 2.5" slider, so offset/end-margin stay 1mm/side → span(N) = 2 + (N−1)·127. Only
// pitch drives sweep placement; the span is measured live. 4" path still disabled in
// the UI until the slider is built and one sweep confirms these numbers.
#define MANIFOLD_4_FIRST_GATE_OFFSET_MM     1.0f
#define MANIFOLD_4_GATE_PITCH_MM            127.0f
#define MANIFOLD_4_END_MARGIN_MM            1.0f

// steps/mm sanity bound: reject a measured sweep whose derived steps/mm deviates
// from the nominal geometric value by more than this — signals a wrong manifold
// profile or a mechanical fault rather than trusting a bad measurement.
#define STEPS_PER_MM_PLAUSIBILITY_PCT       15.0f
// Span re-check tolerance (mm): on re-home, a measured span off by more than this
// from the stored span flags possible lost steps (recalibrate).
#define SPAN_CHECK_TOLERANCE_MM             5.0f

// Names for each stop (used in serial debug output — extend as needed)
#define STOP_NAMES { "Home/Disabled", "Stop 1", "Stop 2", "Stop 3", \
                     "Stop 4", "Stop 5", "Stop 6", "Stop 7",        \
                     "Stop 8", "Stop 9", "Stop 10", "Stop 11",      \
                     "Stop 12", "Stop 13", "Stop 14", "Stop 15", "Stop 16" }

// -----------------------------------------------------------------------------
// MOTION PARAMETERS
//
// The stepper's geometry used to live here — STEPS_PER_REV, MICROSTEPS,
// PINION_TEETH, RACK_PITCH_MM — frozen and unused, kept only because
// stepsPerMM() had an arm that computed from them. All four went with the
// stepper on 2026-08-28. The live rack geometry is the ST3215 block further
// down, next to the capability it depends on.
//
// One of them is worth remembering as a cautionary tale rather than a constant:
// RACK_PITCH_MM 4.145 was NOT A MODULE and not a measurement. It was
// back-derived (gate pitch 82.9mm ÷ an ASSUMED 20 teeth) and it was a LINEAR
// pitch — module is pitch/π — so typing it into a CAD generator's Module field
// made every tooth π× too coarse and produced a 255mm rack where an 82.9mm one
// was wanted. It implied module 1.3193, which is not a standard module and not a
// rack anyone can buy: the tell that it had never been measured. The rack that
// replaced it, and the derivation that keeps this from happening again, is
// §5.0.3 of firmware/wiring/st3215-bench.md.
//
// Speeds and the homing backoff still live here, and the ST3215 block overrides
// them on a board that has a rack.
// -----------------------------------------------------------------------------

// ── Homing: which step direction moves TOWARD the datum ─────────────────────
//
// DERIVED, NOT STORED, AND NOT AUTO-DETECTED (2026-08-28).
//
// It used to be a persisted runtime value that the homing sweep discovered for
// itself: if the FAR endstop tripped while seeking the datum, the motor was
// taken to be wired backwards, the direction was flipped and written to NVS.
// That existed for the STEPPER, where swapping one coil pair reverses rotation
// and is as easy to do as not — the same coin-flip a 3D printer deals with.
//
// A serial bus servo cannot be wired backwards. Its connector is keyed, its
// direction is a protocol bit, and bit 15 turns the shaft the same way every
// time. So the thing the detection protected against no longer exists, and all
// it could do was misfire — which it did, flipping the direction and rewriting
// flash because a far endstop chattered at its release edge.
//
// What DOES vary is which end of the rail the datum is on, and that has one
// answer already: g_homeIsMaxEndstop, settled by the wizard's single "did it
// home to the left?" question. The two were always changed together —
// setHomedLeft() flipped both in lockstep — which is the tell that direction was
// never independent information. So it is computed from the datum instead of
// tracked beside it, and one of the two ways to be wrong is gone.
//
// HOME_DIRECTION_MOUNT is the remaining fixed fact: which step sign drives the
// carriage toward the PIN_ENDSTOP_HOME end. That is a property of the mount and
// the pinion's side of the rack, identical on every unit of a given design.
#define HOME_DIRECTION_MOUNT  (1)
// Kept as an alias: the API and the NVS migration still name the old constant.
#define HOME_DIRECTION_DEFAULT  HOME_DIRECTION_MOUNT
extern bool g_homeIsMaxEndstop;    // defined in firmware.ino / node
inline int homeDirection() {
    return g_homeIsMaxEndstop ? -HOME_DIRECTION_MOUNT : HOME_DIRECTION_MOUNT;
}
#define HOME_DIRECTION           homeDirection()

// Speed & acceleration
// steps/mm ≈ 102.94, so:
//   2000 steps/sec ≈ 19 mm/sec (normal moves)
//   1500 steps/sec ≈ 15 mm/sec (homing — StallGuard needs speed to trigger reliably)
#define MAX_SPEED_STEPS_PER_SEC      2000.0f
#define HOMING_SPEED_STEPS_PER_SEC   500.0f
#define ACCELERATION_STEPS_PER_SEC2  1000.0f

// Maximum travel during homing — safety cutoff if the home switch is never triggered.
// 700 mm covers an 8-gate installation (7 × 82.9 mm ≈ 580 mm) plus generous margin.
// At homing speed (~9.7 mm/sec) this limits runaway to ~72 s before the firmware
// forces the position to home regardless of the switch.
#define HOMING_MAX_TRAVEL_MM  700.0f

// After homing, back off this many steps before zeroing position.
// Endstop margin = 1 tooth = ~427 steps; backoff just needs to clear the switch.
// 50 steps ≈ 0.49mm — conservative, well within the 4.145mm margin.
#define HOME_BACKOFF_STEPS   50  // ~1mm at 51.47 steps/mm — clears backlash without overshooting gate 1

// Idle power-off: if no move/home command is issued for this many seconds,
// the driver is fully disabled (not just dropped to hold current) and the
// position is marked unknown, requiring a rehome before the next move.
// User-configurable at runtime via PUT /api/config/idle-timeout (0 = never
// sleep); this is only the default for a fresh device / after a NVS erase.
#define IDLE_TIMEOUT_SEC_DEFAULT 3600

// -----------------------------------------------------------------------------
// SYSTEM RESILIENCE — unattended-operation recovery
// -----------------------------------------------------------------------------
// How long WiFi may stay disconnected before we actively nudge a reconnect. The
// ESP32 core auto-reconnects on most drops, but a full AP outage can leave the
// radio idle indefinitely; WiFiProvisioner::maintain() retries at this cadence
// so the device rejoins on its own instead of needing a power cycle.
#define WIFI_RECONNECT_INTERVAL_MS  10000

// Task-watchdog timeout (seconds) for the main loop. If a loop iteration ever
// stalls this long, the device reboots itself rather than hanging silently.
// Generous on purpose: normal iterations are sub-millisecond (incremental motor
// steps + async I/O), and blocking network work runs on the poll task, not here.
#define WDT_TIMEOUT_SEC             10

// -----------------------------------------------------------------------------
// PHASE 2 — SERVO (ball-valve gates)
// -----------------------------------------------------------------------------
// Size of the PWM servo bank on one board. Mirrors MAX_SERVOS_PER_HOST in
// shared/device-model/topology.js — the schema refuses to place more servo gates on
// a controller than it has channels.
#define SERVO_COUNT                4

// Servo sweep duration: the driver eases from the current angle to the target
// over this long, rather than slamming the ~90° move in one command — gentler on
// the gate, the coupling, and the ball. ~2s feels deliberate without being slow.
// This is the CEILING, reached by a full-throw move; see SERVO_MS_PER_DEG.
#define SERVO_SWEEP_MS             2000

// Sweep pacing. The sweep duration is proportional to how far the servo actually has
// to travel, clamped to [SERVO_SWEEP_MIN_MS, SERVO_SWEEP_MS]. A fixed duration made a
// 90° throw and a 3° nudge take the same 2s, which is right for the throw and useless
// for the setup jog control — a nudge you can't see land is a nudge you press twice.
// 22ms/° puts a 90° quarter-turn at the full ~2s and a 3° nudge at the 80ms floor.
#define SERVO_MS_PER_DEG             22
#define SERVO_SWEEP_MIN_MS           80

// Post-move hold: keep the servo energized this long AFTER the sweep completes so
// an analog servo can actually catch up to the final commanded angle before we
// de-energize. Then auto-detach (the DEFAULT): analog servos groan/hunt while
// holding, and the ball valve holds position by friction/detent once seated.
#define SERVO_HOLD_MS              1000

// -----------------------------------------------------------------------------
// PIN ASSIGNMENTS — selected per board
// The concrete pin map lives in a boards/*.h header chosen by a -DBOARD_* build
// flag (set per PlatformIO env). Everything downstream uses the PIN_* / SERIAL1_*
// macros unchanged. One board, two roles: the flag is the same either way and
// the ROLE comes from -DDUSTGATE_SECONDARY. All GPIO are 3.3V logic.
// -----------------------------------------------------------------------------
#if defined(BOARD_XIAO_C5)
  #include "boards/xiao_c5.h"
#else
  #error "No board flag set. Build with -DBOARD_XIAO_C5 (see platformio.ini)."
#endif

// -----------------------------------------------------------------------------
// BOARD CAPABILITIES — derived, never hand-set.
//
// A DustGate board is defined by what it can physically drive, and that is
// already implied by its pin map. Deriving the capabilities from the pins means
// a board header can't claim hardware it doesn't wire up, and adding a new
// target is one file rather than a pin map plus a matching set of feature flags.
//
//   HAS_LINEAR  — this board can drive a sliding gate (a carriage on a rack).
//   HAS_SERVO   — the PWM servo bank.
//   HAS_BIN     — this board can watch a dust-bin level sensor.
//
// These replaced the old `#error "No feedback type defined"` / `"No control type
// defined"` walls in the sketch, which made a stepper-less build impossible to
// express at all.
// -----------------------------------------------------------------------------

// A board can drive a sliding gate if it wires the serial-servo bus. Nothing
// defines these pins yet, so HAS_LINEAR is 0 everywhere; the branches it guards
// are kept as the seam an ST3215 driver plugs into. See attic/linear/README.md.
#if defined(PIN_SERVO_BUS_TX)
  #define HAS_LINEAR 1
#else
  #define HAS_LINEAR 0
#endif

// PWM AND SERIAL NEVER SHARE A BOARD. The slider gets dedicated hardware that
// rides along with it, so a bus board drives no ball valves and vice versa. That
// is a decision, not a pin shortage — and it is why the D6/D7 overlap on the
// XIAO C5 (bus RX is also PWM channel 1) costs nothing.
#if HAS_LINEAR && defined(SERVO_PWM_PIN_1)
  #error "A board drives PWM servos or a serial bus, never both — see boards/xiao_c5.h"
#endif

#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)
  #define HAS_SERVO 1
#else
  #define HAS_SERVO 0
#endif

// A board can watch a dust bin if it wires the sensor pin. Note what this is
// NOT: it is not "a bin sensor is fitted", and it is not a board role.
//
// Bin sensing is ONE INPUT PIN, so it collides with nothing and needs no env of
// its own — a primary and a node get it on the same terms, which is the whole
// argument in docs/shop-schema-rfc.md §7.5 (superseding §7.4's "new node type").
// A board is not a "collector node"; it is a board that happens to be near a
// bin.
//
// Whether a given board is actually WATCHING one is a topology fact
// (`bin.sensor.controllerId`) and the primary owns it, because there is no way
// to probe a digital input for whether anything is on the other end — unlike the
// screen, which an I2C ACK at 0x3C settles at boot. HAS_BIN only says the pin
// exists to be read.
#if defined(PIN_BIN_SENSOR)
  #define HAS_BIN 1
#else
  #define HAS_BIN 0
#endif

// The bin pin is D6/GPIO11, which is PIN_SERVO_BUS_TX on a slider build. The
// board header already guards against defining both, and this is the backstop —
// same shape as the PWM-vs-bus #error above, and for the same reason: a pin map
// that quietly claims one pad twice is a bench session nobody enjoys.
#if HAS_BIN && defined(PIN_SERVO_BUS_TX) && (PIN_BIN_SENSOR == PIN_SERVO_BUS_TX)
  #error "PIN_BIN_SENSOR collides with the servo bus — see boards/xiao_c5.h"
#endif

// -----------------------------------------------------------------------------
// THE ST3215 SLIDER — rack, pinion, and the motion numbers that go with them.
//
// This is the LIVE geometry, as against the frozen stepper block near the top of
// this file. It only exists on a board that wires the serial-servo bus, and when
// it does it OVERRIDES the stepper's speeds and backoff: those are in units of
// microsteps at 51.47 steps/mm, and a bus servo counts in encoder counts at
// 24.7 counts/mm. Same macro names on purpose — every call site in the sketch
// keeps working, and "how fast does the carriage move" has one answer per board
// rather than one per driver.
//
// The rack itself: 15 teeth per 82.9mm Rockler gate pitch, module 1.7592, 30T
// pinion. §5.0.3 of wiring/st3215-bench.md carries the derivation, the seam
// rules for printing it in chainable segments, and the warning about the
// artifact pitch the stepper block still holds. DO NOT re-derive it from
// RACK_PITCH_MM up there.
// -----------------------------------------------------------------------------
#if HAS_LINEAR

// The servo's encoder, and the only honest position source on the rail.
#define ST3215_COUNTS_PER_REV     4096.0f

// 30 teeth × 5.5266667mm — one revolution is exactly two gate pitches, which is
// the whole reason 30 and 15 were chosen over 24 and 12. See §5.0.3.
#define ST3215_PINION_TEETH         30
#define ST3215_RACK_PITCH_MM     5.5266667f
#define ST3215_MM_PER_REV        (ST3215_PINION_TEETH * ST3215_RACK_PITCH_MM)   // 165.8

// 4096 / 165.8 = 24.7045 counts/mm. utils/MotionMath.h's stepsPerMM() returns
// this on a bus board, so every mm↔"step" conversion in the sketch is really
// mm↔counts and nothing else had to change.
#define ST3215_COUNTS_PER_MM     (ST3215_COUNTS_PER_REV / ST3215_MM_PER_REV)

// Which servo on the bus. One slider, one servo, and a virgin part answers at 1.
#define ST3215_SERVO_ID              1

// A real baud, not register 6's index. The factory rate, and Seeed's own
// example — see ST3215Bus::begin().
#define ST3215_BAUD            1000000

// -- Motion, in counts/sec, from the bench baseline (st3215-bench.md §5.0.1) --
//
// Top speed measured 1695 counts/s at ~9V, and both speed and supply move
// together, so asking for more than the servo can do is asking to be lied to
// about how fast it went. 1200 tracked to 1046 (87%) and is the fastest number
// this file is willing to claim: ~42mm/s.
#undef  MAX_SPEED_STEPS_PER_SEC
#define MAX_SPEED_STEPS_PER_SEC      1200.0f

// HOMING IS SLOW BECAUSE OVERSHOOT IS AN ABORT FIGURE. A move that runs to its
// own goal lands on it; a move INTERRUPTED — which is exactly what tripping an
// endstop is — coasts 31 counts past the catch point at speed 400. That coast
// is the error in the datum, so 250 counts/s (~10mm/s) is the sweep's accuracy
// budget, not its patience budget.
#undef  HOMING_SPEED_STEPS_PER_SEC
#define HOMING_SPEED_STEPS_PER_SEC    250.0f

// The servo ramps in hardware (register 41); nothing generates steps here, so
// this exists only because the shared motion code names it.
#undef  ACCELERATION_STEPS_PER_SEC2
#define ACCELERATION_STEPS_PER_SEC2  1000.0f

// Back off far enough to release the switch and clear backlash, no further —
// this lands between the datum and gate 1.
//
// RAISED 25 → 60 ON 2026-08-28. 25 counts (1.0mm) was the millimetre the stepper
// used, and on the real rail it never released the switch first time: every home
// logged "still on the datum switch — backing off further" twice before clearing
// at ~51 counts. Each of those extensions is a torque cycle and a second of
// travel for nothing. 60 counts ≈ 2.4mm clears it in one move, and still lands
// well short of gate 1.
//
// Homing extends this until the switch actually opens and reports the distance
// it used, so this is a starting guess, not a limit — see
// LimitSwitchDistance::backoffSteps().
#undef  HOME_BACKOFF_STEPS
#define HOME_BACKOFF_STEPS            60

#endif // HAS_LINEAR

// -----------------------------------------------------------------------------
// SECONDARY ROLE (-DDUSTGATE_SECONDARY)
//
// A "dumb" actuator bank in the star: it accepts already-resolved NodeLink SET
// frames and moves a channel to an angle. It owns no topology, computes no
// routing, polls no Shelly plugs and serves no web UI — the primary does all of
// that. Building one is mostly SUBTRACTION from the normal firmware, which is
// what keeps the cheap variant from being a second codebase.
//
// Kept as one flag rather than a pile of per-feature switches so "is this board
// a secondary?" has exactly one answer.
// -----------------------------------------------------------------------------
// The secondary is built from its own tiny source root (firmware/node/),
// NOT by #ifdef-ing the primary sketch: the stepper, endstops, homing, the
// reference sweep, Shelly polling and the routing brain are woven through the
// main .ino in ~100 separate places, and splitting that with the preprocessor
// would leave a sketch nobody can read. See node/dustgate_node.cpp.
//
// This flag therefore only switches off the primary-only subsystems that the
// SHARED headers would otherwise pull in.
#ifdef DUSTGATE_SECONDARY
  // No plug polling: the primary owns tool sensing and tells us what to do.
  #undef CONTROL_SMART_OUTLET
  // No REST API / Angular bundle — a node's entire interface is /nodelink.
  #undef ENABLE_HTTP_API
#endif

// -----------------------------------------------------------------------------
// SMART OUTLET CONTROL (CONTROL_SMART_OUTLET)
// Requires WIFI_STA_SSID / WIFI_STA_PASS to be set below.
// Outlet-to-stop mappings are stored in NVS during setup.
// -----------------------------------------------------------------------------

// Maximum number of outlet slots (one per blast gate)
#define SMART_OUTLET_COUNT            7

// Maximum number of switchable COLLECTOR plugs — one per airflow system.
// A shop owns N systems (docs/shop-schema-rfc.md), each with its own blower:
// a 4" cyclone for the big machines and a 2.5" wall unit for the bench is the
// case that motivated this. Slot 0 is the one persisted in NVS and the one the
// pre-topology stop-index automation drives; the rest are RAM-only and rebuilt
// from the layout on every adopt, exactly like the tool slots.
// Three, not two: two is the realistic ceiling for a home shop and the third
// costs one pointer, so a third system doesn't silently lose its blower.
#define COLLECTOR_COUNT               3

// How often the poll task queries each outlet (ms)
#define OUTLET_POLL_INTERVAL_MS     500

// HTTP request timeout per outlet — must be shorter than OUTLET_POLL_INTERVAL_MS
// to avoid stalling the poll loop when a device is offline
#define OUTLET_HTTP_TIMEOUT_MS      400

// Timeout for RPC config writes (Ws.SetConfig / Switch.SetConfig / Sys.SetConfig).
// These persist to the plug's flash and can take far longer than a status read,
// so they get a generous window. Only runs at provisioning time (device add /
// boot), never on the fast poll path, so a long value is safe here.
#define OUTLET_RPC_WRITE_TIMEOUT_MS 3000

// Reachability probe timeout for the provisioning path. Unlike the 400ms poll
// probe (which must fit inside the poll interval), provisioning runs rarely and
// off the motor loop, so it can afford to wait for a marginal plug to answer its
// first request instead of failing it and deferring for a whole retry cycle.
#define OUTLET_PROVISION_PROBE_TIMEOUT_MS 2000

// How often to retry push-provisioning plugs that aren't yet configured (e.g. a
// plug that was briefly unreachable at boot). Each retry only fast-probes the
// unprovisioned plugs; already-pushing plugs are skipped, so this is cheap.
#define OUTLET_PROVISION_RETRY_MS   15000

// mDNS discovery (setup wizard's "Scan for outlets" / serial 'discover').
//
// ONE LONG QUERY, NOT SEVERAL SHORT ONES (2026-08-22). This was 3 attempts of
// 400ms, on the theory that mDNS is lossy UDP and a device only has to answer
// once across the attempts — with a comment asserting that anything that will
// answer does so "within tens of milliseconds". That is true of a plain LAN and
// was flatly untrue of the shop's: a C5 primary found ZERO of four Shelly plugs
// at 400ms and all four, first try from cold, at 3000ms.
//
// Short-and-repeated is the wrong shape for that anyway. ESP-IDF retransmits
// the query itself over the life of one search, so a single 3000ms window
// already gives a slow responder several chances — while three separate 400ms
// searches give it three chances that all close before it speaks. And a second
// search reads back the mDNS cache the first one filled, which makes repeat
// attempts look far more effective in testing than they are in the field.
//
// The window is affordable now because the query no longer blocks: MdnsQuery.h
// polls an async search and pets the watchdog. Raising it is cheap; the cost is
// that gate control doesn't run while a scan is in flight. See MdnsQuery.h.
#define DISCOVER_MDNS_ATTEMPTS       1
#define DISCOVER_MDNS_TIMEOUT_MS     3000
#define DISCOVER_MDNS_RETRY_DELAY_MS 250
#define DISCOVER_MAX_RESULTS         16

// How long a tool must be drawing above threshold before the gate moves (ms).
// Prevents false triggers from motor startup inrush.
#define OUTLET_ON_DEBOUNCE_MS      1000

// How long all tools must be idle before returning to home (ms).
// Extra slack for tools with mechanical coast-down (router, bandsaw, etc.)
#define OUTLET_OFF_DEBOUNCE_MS     3000

// Default watts threshold for "tool is on". Overridden per-outlet in NVS.
// Set low enough to catch light tools (shop vac ≈ 1000W, soldering iron ≈ 60W).
// Set above standby draw of power strips / outlet transformers (typically < 2W).
#define OUTLET_DEFAULT_THRESHOLD_W  5.0f

// -----------------------------------------------------------------------------
// WIFI CREDENTIALS
// Station mode credentials — uncomment to hardcode (developer / known network).
// Leave commented for end-user deployments: credentials are stored via the setup portal.
// #define WIFI_STA_SSID    "your-network-name"
// #define WIFI_STA_PASS    "your-password"

// Setup portal SSID: shown when no WiFi credentials are stored.
// Connect to this hotspot and visit http://192.168.4.1 to enter your network credentials.
#define WIFI_PORTAL_SSID    "DustGate-Setup"

// -----------------------------------------------------------------------------
// HTTP API (ENABLE_HTTP_API)
// -----------------------------------------------------------------------------

// API key length (bytes) for auto-generated keys. 8 bytes = 16 hex chars.
#define API_KEY_BYTES   8

// Port for the HTTP API server (also serves WebSocket at /ws)
#define API_PORT        80

// How often the main loop re-serializes the routing view for
// GET /api/status. The view only changes on a tool on/off or a plan step, so
// rebuilding it every loop pass would burn heap churn for nothing; 250ms is
// well inside what the Live view needs to feel immediate.
#define V2_STATUS_PUBLISH_MS   250

// -----------------------------------------------------------------------------
// SERIAL COMMANDS
// Enables serial command processing (status, home, jog, help, etc.) alongside
// any control mode. Independent of CONTROL_SERIAL_DEBUG — you can type commands
// in the serial monitor even when CONTROL_SMART_OUTLET is the active mode.
// Disable to save flash if you never use the serial monitor in production.
// -----------------------------------------------------------------------------
#define ENABLE_SERIAL_COMMANDS

// -----------------------------------------------------------------------------
// SERIAL / DEBUG
// -----------------------------------------------------------------------------
#define SERIAL_BAUD       115200
#define DEBUG_ENABLED       true   // Set false to suppress all Serial output

#if DEBUG_ENABLED
  #define DEBUG_PRINT(x)    Serial.print(x)
  #define DEBUG_PRINTLN(x)  Serial.println(x)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
#endif

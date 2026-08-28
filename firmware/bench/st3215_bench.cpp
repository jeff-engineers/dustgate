// =============================================================================
// st3215_bench.cpp — a serial console for ONE bus servo on a bench.
//
// This is not part of DustGate. It is the smallest program that can answer the
// only questions worth asking on the day a bus servo first arrives:
//
//     is anything answering?     scan
//     at what id, at what baud?  scan / baud
//     does it move?              move 2048
//     does it stop?              stop
//     can I turn it by hand?     torque off
//     what does it think?        read / watch
//
// WHY A SEPARATE PROGRAM. There is no gate, no topology, no WiFi and no node
// here — nothing that has to be configured before a shaft turns. A first-contact
// bug should be a bug in ONE screenful of code, not somewhere in a program that
// also joins a network and serves an Angular bundle.
//
// NOTHING MOVES UNLESS YOU TYPE IT. Boot enables no torque and writes no
// register: a servo bolted to a half-built slider must not lurch because a board
// reset. `torque on` is a command like any other.
//
// Build and flash (this is the only env that defines the bus pins):
//     PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e xiao_c5_bus_bench -t upload
//     PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio device monitor -e xiao_c5_bus_bench
//
// ⚠️ Read firmware/wiring/st3215-bench.md before connecting the signal wire.
// The bus logic level is unconfirmed and this part is not 5V tolerant.
// =============================================================================

#include <Arduino.h>
#include <stdarg.h>          // the suite's check()/meas() formatting
#include "../config.h"
#include "../motor/st3215/ST3215Bus.h"

static ST3215Bus bus;

// The id every command uses when it isn't given one. 1 is the factory default
// on every Feetech servo I have read about, which is also why a bus with two
// virgin servos on it cannot work until one of them is renumbered.
static uint8_t  target   = 1;
static uint32_t busBaud  = ST_DEFAULT_BAUD;

// `watch` streams until a key is pressed; loop() owns the timer so the watchdog
// keeps getting petted.
static bool     watching = false;
static uint32_t nextTick = 0;

// -----------------------------------------------------------------------------
// Printing
// -----------------------------------------------------------------------------

static void printState();   // defined below, next to the other readouts

static void banner() {
    Serial.println();
    Serial.println(F("ST3215 bench — one servo, one wire, no gate."));
    Serial.printf("  bus: TX D6/GPIO%d  RX D7/GPIO%d  @ %lu baud, %s-endian\n",
                  PIN_SERVO_BUS_TX, PIN_SERVO_BUS_RX,
                  (unsigned long)busBaud, bus.isLittleEndian() ? "little" : "big");
    Serial.printf("  target id: %u\n", target);
    // Ask the servo rather than claiming anything: mode and angle limits live in
    // its EEPROM and outlive any number of board resets, so "fresh boot" says
    // nothing about what state it is in.
    printState();
    Serial.println(F("  type `help`"));
}

static void help() {
    Serial.println(F(
        "\n"
        "  scan [hi]        ping every id from 0 to hi (default 20)\n"
        "  sweep [hi]       scan at every plausible baud, both pin orders, and stay on what works\n"
        "  pins             which pad is TX and which is RX right now\n"
        "  selftest         loop TX to RX inside the chip — proves the UART with no servo at all\n"
        "  selftest wire    the same, through a jumper from D6 to D7 (XIAO out of the board)\n"
        "  blast [s]        transmit continuously so a meter or scope can see the pin\n"
        "  continuity       DC test of the same jumper, no UART — is the wire even on?\n"
        "  swap             flip them, and reopen\n"
        "  baud <n>         reopen the bus at another rate (factory is 1000000)\n"
        "  id <n>           which servo the commands below talk to\n"
        "  ping             is it there, and what is its status byte\n"
        "  read             position, speed, load, volts, temp, moving\n"
        "  watch            stream `read` until you press a key\n"
        "  dump <addr> <n>  raw registers as hex — the map is unverified, so start here\n"
        "\n"
        "  torque on|off    off frees the shaft; nothing is held\n"
        "  move <pos> [spd] 0..4095 over one turn in mode 0; speed is steps/s, 0 = flat out\n"
        "  stop             goal := where it is right now, and it stays powered\n"
        "  centre           move 2048\n"
        "  mode 0|3         0 = position (one turn), 3 = multi-turn step\n"
        "  limits off|turn  off = both angle limits to 0, which is what UNLOCKS multi-turn\n"
        "  mturn <n> [spd]  goal in counts past one turn, signed. 4096 counts = one turn\n"
        "  stepmode on|off  mode 3 + limits 0, the documented way to step indefinitely\n"
        "  step <n>         in stepmode: a NUMBER OF STEPS, signed. Not a position\n"
        "  zero             reset the tracked across-turns position to 0 (it wraps; we count)\n"
        "  timeit <pos> [spd]  move there and time it, in counts/s and rpm — measures what 46 means\n"
        "\n"
        "  suite            run every check, print PASS/FAIL and a baseline. THE SHAFT TURNS.\n"
        "\n"
        "  w8 <addr> <v>    poke a byte    (careful: EEPROM writes below 40 persist)\n"
        "  w16 <addr> <v>   poke a word\n"
        "  endian lo|hi     16-bit byte order, if the numbers come back nonsense\n"
        "  trace on|off     every frame in both directions, as hex\n"));
}

static void fail(const char* what) {
    const char* why = bus.lastError();
    // An empty reason is itself a bug — something failed and then a later
    // successful transaction cleared the record before we printed it. Say so
    // rather than printing "failed: " and leaving the reader to guess.
    Serial.printf("  %s failed: %s\n", what, (why && *why) ? why : "(reason already overwritten — retry)");
}

/** The servo's status byte, spelled out. 0 is a healthy reply. */
static void printErrBits(uint8_t err) {
    if (!err) { Serial.println(F("  status: ok")); return; }
    Serial.printf("  status: 0x%02X —", err);
    if (err & 0x01) Serial.print(F(" voltage"));
    if (err & 0x02) Serial.print(F(" angle"));
    if (err & 0x04) Serial.print(F(" overheat"));
    if (err & 0x08) Serial.print(F(" range"));
    if (err & 0x10) Serial.print(F(" checksum"));
    if (err & 0x20) Serial.print(F(" overload"));
    if (err & 0x40) Serial.print(F(" instruction"));
    Serial.println();
}

/** Feetech puts the sign of speed in bit 15, not in two's complement. */
static int16_t signedField(uint16_t raw) {
    int16_t mag = (int16_t)(raw & 0x7FFF);
    return (raw & 0x8000) ? (int16_t)-mag : mag;
}

/**
 * LOAD is not the same shape as speed: 10-bit magnitude, direction in bit 10.
 *
 * Read as a bit-15 field it looks like a servo straining — a bare shaft idling
 * reported "1056", which is 0x420: the direction bit and a magnitude of 32, or
 * about 3%. That misreading had us hunting a supply sag that was never there
 * (2026-08-26).
 */
static int16_t signedLoad(uint16_t raw) {
    int16_t mag = (int16_t)(raw & 0x03FF);
    return (raw & 0x0400) ? (int16_t)-mag : mag;
}

// -----------------------------------------------------------------------------
// Where the shaft actually is, across turns
//
// PRESENT_POSITION IS ONE TURN, SIGNED, AND IT WRAPS. Measured 2026-08-26:
// 4096 steps forward from 0 lands back at 4, and stepping backwards from there
// reads 0x8B51 — bit 15 set, sign-magnitude, so -2897 rather than 35665. So the
// value is a position on a 4096-count circle, reported as -4095..+4095 depending
// on which side of zero the shaft sits.
//
// Nothing in the register map counts turns, so absolute position is ours to
// keep: decode the sign first, then unwrap.
//
// THE SAMPLING RULE THIS DEPENDS ON: a jump of more than half a turn between two
// samples is indistinguishable from a smaller jump the other way, so this must
// be fed faster than the shaft can travel 2048 counts. At the measured ceiling
// of ~1700 counts/s that is 1.2s, and everything here samples at 15-200ms. A
// slider node polling slower than that would silently lose turns.
//
// It cannot survive a power cycle, which is why the endstops stay on the rail
// and the homing sweep is still the calibration path.
//
// AND IT HAS TO RUN ON ITS OWN. Sampling only when someone types `read` counts
// nothing: two `step 4096` commands between two reads look like no movement at
// all, because the wraps happened unobserved. loop() polls it, which is exactly
// what the slider node will have to do — the turn count is only as good as the
// polling behind it.
// -----------------------------------------------------------------------------
static long    absPos     = 0;
static int32_t lastSigned = 0;
static bool    haveLast   = false;

/** The signed position on the circle: -4095..+4095, sign in bit 15. */
static int32_t circlePos(uint16_t raw) { return signedField(raw); }

static void trackPosition(uint16_t raw) {
    int32_t now = circlePos(raw);
    if (haveLast) {
        int32_t delta = now - lastSigned;
        if (delta >  2048) delta -= 4096;      // wrapped backwards past zero
        if (delta < -2048) delta += 4096;      // wrapped forwards past the top
        absPos += delta;
    }
    lastSigned = now;
    haveLast   = true;
}

/** Mode and torque, in words. Every state-reporting line prints these. */
static void printState() {
    uint8_t mode = 255, torque = 255;
    bool haveMode   = bus.read8(target, ST_REG_MODE, &mode);
    bool haveTorque = bus.read8(target, ST_REG_TORQUE_ENABLE, &torque);
    if (!haveMode && !haveTorque) { Serial.println(F("  (couldn't read mode/torque)")); return; }

    Serial.printf("  mode %s   torque %s\n",
        !haveMode ? "?" : mode == 0 ? "0 (position, one turn)"
                        : mode == 1 ? "1 (wheel)"
                        : mode == 2 ? "2 (PWM open loop)"
                        : mode == 3 ? "3 (step / multi-turn)" : "?",
        !haveTorque ? "?" : torque ? "on" : "OFF — nothing will move");
}

// -----------------------------------------------------------------------------
// Commands
// -----------------------------------------------------------------------------

/** Ping every id up to `hi`, printing only what answers. Returns how many did. */
static uint8_t scanQuiet(uint8_t hi) {
    uint8_t found = 0;
    for (uint8_t id = 0; id <= hi; id++) {
        uint8_t err = 0;
        if (bus.ping(id, &err)) {
            Serial.printf("  id %-3u answered (status 0x%02X)\n", id, err);
            found++;
        }
        // Also the yield that keeps the idle task fed through a long hunt.
        delay(2);
    }
    return found;
}

/**
 * The whole search space, when nothing answers and you don't know why.
 *
 * A silent bus has three ordinary causes and they are indistinguishable from the
 * console: the baud is wrong, the pins are the other way round, or nothing is
 * listening. This walks the first two — every plausible rate, both pin orders —
 * and leaves the bus configured on whatever worked. Then the remaining silence
 * means the third, and that is a meter question, not a firmware one.
 */
static void doSweep(uint8_t hi) {
    static const uint32_t kBauds[] = { 1000000, 500000, 250000, 128000, 115200, 76800, 57600, 38400, 19200, 9600 };

    for (uint8_t order = 0; order < 2; order++) {
        for (uint8_t b = 0; b < sizeof(kBauds) / sizeof(kBauds[0]); b++) {
            bus.begin(kBauds[b], order == 1);
            Serial.printf("  %lu baud, %s pins ... ", (unsigned long)kBauds[b],
                          order ? "swapped" : "normal");
            uint8_t found = scanQuiet(hi);
            if (!found) { Serial.println(F("nothing")); continue; }

            busBaud = kBauds[b];
            Serial.printf("  ^ that one. Bus left at %lu baud, %s pins (TX GPIO%d, RX GPIO%d).\n",
                          (unsigned long)busBaud, bus.swapped() ? "swapped" : "normal",
                          bus.txPin(), bus.rxPin());
            return;
        }
    }

    // Back to where we started, so a failed sweep leaves nothing behind.
    bus.begin(busBaud, false);
    Serial.println(F("  nothing answered at any rate, either way round.\n"
                     "  That is now a wiring question, not a settings one — see\n"
                     "  firmware/wiring/st3215-bench.md section 5."));
}

/**
 * Prove the UART before blaming the bus.
 *
 * Three layers, and each one only means something if the one before it passed:
 *
 *   1. INSIDE THE CHIP — TX tied to RX in silicon. Passing means the peripheral,
 *      the baud and this program's framing are all sound, and every remaining
 *      suspect is outside the board.
 *   2. AT THE PADS — the same test with a wire from D6 to D7 (XIAO out of its
 *      socket). Passing means both pads are really the UART, and the numbers in
 *      the board header are right.
 *   3. THE BUS ITSELF — that is `scan`, and it is only worth running once 1 and
 *      2 pass.
 *
 * A servo that answers nothing tells you nothing about which of the three is
 * broken, which is the whole reason this exists.
 */
static void doSelfTest(bool internal) {
    Serial.println(internal
        ? F("  looping TX back to RX inside the chip...")
        : F("  expecting a wire from D6 to D7 (XIAO out of the servo board)..."));

    if (internal && !bus.loopback(true)) {
        Serial.println(F("  the chip refused internal loopback — that itself is a finding"));
        return;
    }

    bus.clearEchoSeen();
    uint8_t err = 0;
    bus.ping(target, &err);          // the reply is beside the point; the ECHO is the test
    bool heard = bus.echoSeen();

    if (internal) bus.loopback(false);

    if (heard) {
        Serial.printf("  PASS — what we sent came back%s.\n", internal ? " inside the chip" : " through the pads");
        if (internal) Serial.println(F("  So the UART, the baud and the framing are fine. Try `selftest wire` next."));
        else          Serial.println(F("  So both pads are live and the header's pin numbers are right."));
    } else {
        Serial.printf("  FAIL — nothing came back%s.\n", internal ? " inside the chip" : "");
        if (internal) Serial.println(F("  Nothing past this point is worth debugging: the UART is not doing its job."));
        else          Serial.println(F("  Either the wire is not on D6/D7, or those pads are not this UART.\n"
                                       "  `swap` and try again; if that also fails, the pin numbers are wrong."));
    }
}

/**
 * Continuity between the two pads, with no UART involved at all.
 *
 * `selftest wire` failing tells you the pads did not carry a 1 Mbps frame, and
 * that has two very different causes: the jumper is not making contact, or the
 * UART is not really on those pads. This separates them by driving one pad from
 * plain GPIO and reading the other — DC levels, no timing, nothing to get wrong.
 *
 * Both pads go back to the UART afterwards.
 */
static void doContinuity() {
    const int tx = bus.txPin(), rx = bus.rxPin();
    Serial.printf("  driving GPIO%d, reading GPIO%d — wire them together first\n", tx, rx);

    pinMode(tx, OUTPUT);
    pinMode(rx, INPUT_PULLDOWN);
    digitalWrite(tx, HIGH); delay(5);
    bool high = digitalRead(rx);
    digitalWrite(tx, LOW);  delay(5);
    bool low  = digitalRead(rx);

    // Whatever happens, leave the bus usable.
    bus.begin(busBaud, bus.swapped());

    if (high && !low) {
        Serial.println(F("  PASS — the two pads are connected.\n"
                         "  So the wire is fine and the UART is not reaching them: that is a firmware\n"
                         "  or core problem, not a bench one."));
    } else if (!high && !low) {
        Serial.println(F("  FAIL — the reading pad never went high. The jumper is not making contact\n"
                         "  (castellated pads need a header pin or real pressure), or it is on the\n"
                         "  wrong two pads. D6 and D7 are the two nearest the USB connector's far end."));
    } else {
        Serial.printf("  ODD — high:%d low:%d. Something else is driving GPIO%d.\n", high, low, rx);
    }
}

/**
 * Talk continuously so a meter or a scope can see it.
 *
 * A DMM on the TX pad reads ~3.3 V idle and visibly lower while this runs; a
 * scope sees the frames. It is the only way to answer "is anything coming out
 * of that pin at all" without another board to listen with.
 */
static void doBlast(uint32_t seconds) {
    Serial.printf("  streaming on GPIO%d for %lus.\n"
                  "  Meter DC, black on GND, red on the pad: ~3.3 V idle, and clearly\n"
                  "  lower (roughly 1.5-2.5 V) while this runs. Then meter the same signal\n"
                  "  where it ARRIVES — the servo connector — and see if the board passed it on.\n",
                  bus.txPin(), (unsigned long)seconds);
    bus.stream(target, seconds * 1000);
    Serial.println(F("  done"));
}

static void doScan(uint8_t hi) {
    Serial.printf("  scanning 0..%u at %lu baud\n", hi, (unsigned long)busBaud);
    uint8_t found = 0;
    for (uint8_t id = 0; id <= hi; id++) {
        uint8_t err = 0;
        if (bus.ping(id, &err)) {
            Serial.printf("  id %-3u answered (status 0x%02X)\n", id, err);
            found++;
        }
        delay(2);   // the bus is shared; give a slow part time to let go of it
    }
    if (!found) {
        Serial.println(F("  nothing answered.\n"
                         "  `sweep` tries every baud both pin orders — do that before reaching\n"
                         "  for the meter. A servo that has been configured before keeps whatever\n"
                         "  rate it was given, and a silent bus cannot say which of the two it is."));
    }
}

static void doRead() {
    uint16_t pos = 0, spd = 0, load = 0, cur = 0;
    uint8_t  volt = 0, temp = 0, moving = 0;

    if (!bus.read16(target, ST_REG_PRESENT_POS, &pos))        { fail("read position"); return; }
    bus.read16(target, ST_REG_PRESENT_SPEED, &spd);
    bus.read16(target, ST_REG_PRESENT_LOAD, &load);
    bus.read8 (target, ST_REG_PRESENT_VOLTAGE, &volt);
    bus.read8 (target, ST_REG_PRESENT_TEMP, &temp);
    bus.read8 (target, ST_REG_MOVING, &moving);
    bus.read16(target, ST_REG_PRESENT_CURRENT, &cur);

    trackPosition(pos);
    // Decode the sign before doing arithmetic on it: raw 35665 is 0x8B51, which
    // is -2897, and printing it as 35665 "degrees 3134" is how that got missed.
    int32_t signedPos = circlePos(pos);
    int32_t deg = ((signedPos % 4096) + 4096) % 4096 * 360 / 4096;
    Serial.printf("  id %u  pos %+5ld (%3ld deg)  speed %5d  load %5d  %4.1fV  %u C  %s\n",
                  target, (long)signedPos, (long)deg,
                  signedField(spd), signedLoad(load),
                  volt / 10.0f, temp, moving ? "MOVING" : "still");
    // The state that silently decides whether a goal does anything at all. It
    // is on this line because a mode left over from an experiment ten minutes
    // ago looks exactly like a servo that has stopped working.
    printState();

    // The register wraps, so this is the only running total of where the shaft
    // is. `zero` resets it; a power cycle of the servo invalidates it.
    Serial.printf("  tracked: %+ld counts = %+.2f turns since `zero`\n", absPos, absPos / 4096.0f);
    if (cur) Serial.printf("  current %u (units per the register map, unconfirmed)\n", cur);
}

/**
 * Multi-turn goals, and the angle limit that has to come off first.
 *
 * Mode 3 alone is not enough: the servo still honours MIN_ANGLE/MAX_ANGLE, which
 * ship as 0..4095 — one turn — so a goal beyond that is clamped and the shaft
 * stops at the end of its first revolution looking like mode 3 does not work.
 * Both limits at zero is what means "no limit" on this family.
 *
 * ⚠️ EEPROM, and it persists. `limits turn` puts them back.
 */
static void doLimits(bool off) {
    if (!bus.writeEeprom16(target, ST_REG_MIN_ANGLE, 0))                  { fail("min angle"); return; }
    if (!bus.writeEeprom16(target, ST_REG_MAX_ANGLE, off ? 0 : ST_POS_MAX)) { fail("max angle"); return; }

    uint16_t lo = 0, hi = 0;
    bus.read16(target, ST_REG_MIN_ANGLE, &lo);
    bus.read16(target, ST_REG_MAX_ANGLE, &hi);
    Serial.printf("  angle limits now %u..%u — %s\n", lo, hi,
                  (lo == 0 && hi == 0) ? "multi-turn travel is unlocked"
                                       : "one turn, the factory setting");
}

/**
 * A goal beyond one turn. 4096 counts per revolution, sign in bit 15.
 *
 * Feetech puts the sign of a 16-bit field in its top bit rather than using two's
 * complement, so -4096 is 0x9000 and not 0xF000. Whether PRESENT_POSITION reads
 * back the same way is not documented anywhere I can reach — `read` prints both
 * interpretations past one turn so the bench can settle it.
 */
static void doMTurn(long counts, long speed) {
    if (counts > 32767)  counts = 32767;
    if (counts < -32767) counts = -32767;
    uint16_t raw = (uint16_t)labs(counts);
    if (counts < 0) raw |= 0x8000;

    if (!bus.moveTo(target, raw, speed >= 0 ? (uint16_t)speed : 0)) { fail("mturn"); return; }
    Serial.printf("  goal %ld counts (%.2f turns) sent as 0x%04X\n",
                  counts, counts / 4096.0f, raw);
}

/**
 * Move, and time it — because "counts per second" is a claim, not a measurement.
 *
 * GOAL_SPEED is documented as encoder counts per second, 4096 to the turn. This
 * drives a known distance, times the arrival and divides. If the number that
 * comes back is the number that was asked for, the unit is settled for this part
 * and the slider's feed rates can be worked out on paper instead of by feel.
 *
 * It measures the WHOLE move, ramps included, so a short hop reads slower than
 * the setting — that is the acceleration register (41) doing its job, not a
 * disagreement. Give it a long travel for a fair figure.
 */
/** What a settle attempt found out. `ms` excludes the stillness we waited for. */
struct Settle {
    bool     arrived;      // within the deadband of the goal, and stopped there
    bool     stalled;      // stopped somewhere else and stayed
    uint32_t ms;
    uint16_t from;
    uint16_t pos;
};

/** The deadband this part actually settles inside. Measured, not assumed. */
static const int16_t kCloseEnough = 8;

/**
 * Drive to `goal` and wait for it to mean something.
 *
 * ARRIVAL IS A POSITION QUESTION, NOT A FLAG QUESTION. This polled MOVING
 * (register 66) once; that flag read "still" on a servo demonstrably mid-travel
 * (2026-08-26), and a flag that lies about motion turns a timing measurement
 * into a random number. Watch the position instead.
 *
 * TWO MOMENTS LOOK EXACTLY LIKE A STALL AND ARE NOT, both of which produced a
 * false "stopped short" the same day: the instant after a goal is written and
 * before the servo accelerates, and the turnaround of a reversed move, where
 * velocity passes through zero. Hence a start grace and a stillness threshold
 * long enough that only a real stop clears them. The eventual MotorDriver needs
 * this same logic, for these same reasons.
 */
static Settle settleAt(uint16_t goal, long speed, uint32_t timeoutMs = 15000) {
    Settle r = { false, false, 0, 0, 0 };
    if (!bus.read16(target, ST_REG_PRESENT_POS, &r.from)) return r;

    const uint32_t kStartGraceMs = 1000;
    const uint32_t kQuietMs      = 300;

    // ONE BLOCK WRITE, not a speed write followed by a position write. See
    // ST3215Bus::moveTo — a standalone GOAL_SPEED is ignored and every move runs
    // at the servo's maximum.
    uint32_t started = millis();
    if (!bus.moveTo(target, goal, speed >= 0 ? (uint16_t)speed : 0)) return r;

    uint32_t deadline   = started + timeoutMs;
    uint16_t last       = r.from;
    uint32_t lastChange = started;
    bool     everMoved  = false;

    while ((int32_t)(millis() - deadline) < 0) {
        delay(15);
        uint16_t now = 0;
        if (!bus.read16(target, ST_REG_PRESENT_POS, &now)) continue;
        trackPosition(now);
        if (now != last) { last = now; lastChange = millis(); everMoved = true; }

        bool stillEnough = (millis() - lastChange) >= kQuietMs;
        if (!stillEnough) continue;
        if (abs((int32_t)now - (int32_t)goal) <= kCloseEnough) { r.arrived = true; break; }
        if (everMoved || (millis() - started) > kStartGraceMs)  { r.stalled = true; break; }
    }

    r.ms = millis() - started - ((r.arrived || r.stalled) ? kQuietMs : 0);
    bus.read16(target, ST_REG_PRESENT_POS, &r.pos);
    return r;
}

/** counts/s over a settle, or 0 if it was too short to mean anything. */
static float measuredCps(const Settle& s) {
    long travelled = (long)s.pos - (long)s.from;
    if (s.ms < 50 || travelled == 0) return 0;
    return fabsf(travelled) * 1000.0f / s.ms;
}

static void doTimeIt(uint16_t goal, long speed) {
    Settle s = settleAt(goal, speed);
    Serial.printf("  %u -> %u = %ld counts in %.2fs\n",
                  s.from, s.pos, (long)s.pos - (long)s.from, s.ms / 1000.0f);

    float cps = measuredCps(s);
    if (cps > 0) {
        Serial.printf("  measured %.0f counts/s = %.2f rev/s = %.1f rpm", cps, cps / 4096.0f, cps * 60.0f / 4096.0f);
        if (speed > 0) Serial.printf("  (asked for %ld)", speed);
        Serial.println();
    }
    if (s.stalled) {
        uint16_t load = 0; uint8_t volt = 0;
        bus.read16(target, ST_REG_PRESENT_LOAD, &load);
        bus.read8(target, ST_REG_PRESENT_VOLTAGE, &volt);
        Serial.printf("  STOPPED SHORT of %u and stayed there. load %d, %.1fV\n",
                      goal, signedLoad(load), volt / 10.0f);
        printState();
    } else if (!s.arrived) {
        Serial.println(F("  ...still moving at the timeout — very slow, or creeping"));
    }
}

/**
 * Stepping mode, set up the way the vendor documentation actually describes it.
 *
 * Three things have to be true at once, and we had never had all three: mode 3,
 * both angle limits at zero, and — the one nothing in firmware can do — the
 * servo restarted since. EEPROM settings on these parts are documented as taking
 * effect at startup, and every attempt so far changed the mode on a servo that
 * was already running.
 */
static void doStepMode(bool on) {
    // All three of these are EEPROM, so all three go through the lock. Writing
    // them plainly is what made every previous attempt evaporate on restart.
    // Short-circuit and report WHICH one, keeping the reason: the non-short-
    // circuit version ran all three, and the last one's success wiped the error
    // string, printing "stepmode failed:" with nothing after it.
    if (!bus.writeEeprom8(target, ST_REG_MODE, on ? 3 : 0))                    { fail("mode write"); return; }
    if (!bus.writeEeprom16(target, ST_REG_MIN_ANGLE, 0))                       { fail("min angle write"); return; }
    if (!bus.writeEeprom16(target, ST_REG_MAX_ANGLE, on ? 0 : ST_POS_MAX))     { fail("max angle write"); return; }

    uint8_t mode = 255; uint16_t lo = 1, hi = 1;
    bus.read8(target, ST_REG_MODE, &mode);
    bus.read16(target, ST_REG_MIN_ANGLE, &lo);
    bus.read16(target, ST_REG_MAX_ANGLE, &hi);
    Serial.printf("  mode %u, limits %u..%u — written through the EEPROM lock and verified\n", mode, lo, hi);
    if (on) Serial.println(F("  Power-cycle the servo, then `read` to confirm it comes back in MODE 3.\n"
                             "  It comes back with TORQUE OFF — `torque on` first, or nothing moves.\n"
                             "  Then `step 4096` — a number of steps, not a position."));
    else    Serial.println(F("  back to position mode, one turn"));
}

/**
 * A number of STEPS, not a position — that is what register 42 means in mode 3.
 * Bit 15 carries the direction, so -4096 is 0x9000, not 0xF000.
 */
static void doStep(long steps) {
    uint16_t before = 0;
    bus.read16(target, ST_REG_PRESENT_POS, &before);

    uint16_t raw = (uint16_t)labs(steps);
    if (raw > 32767) raw = 32767;
    if (steps < 0) raw |= 0x8000;
    if (!bus.moveTo(target, raw, 1500)) { fail("step"); return; }

    Serial.printf("  %ld steps sent as 0x%04X, from %+ld (tracked %+ld) — the tracker follows it from here\n",
                  steps, raw, (long)circlePos(before), absPos);
}

static void doDump(uint8_t addr, uint8_t len) {
    uint8_t buf[32];
    if (len > sizeof(buf)) len = sizeof(buf);
    if (!bus.readRegs(target, addr, len, buf)) { fail("dump"); return; }
    Serial.printf("  %u..%u:", addr, addr + len - 1);
    for (uint8_t i = 0; i < len; i++) Serial.printf(" %02X", buf[i]);
    Serial.println();
}

static void doMove(uint16_t pos, long speed) {
    if (pos > ST_POS_MAX) pos = ST_POS_MAX;
    // Speed rides with the position in one write, or it is ignored entirely.
    if (!bus.moveTo(target, pos, speed >= 0 ? (uint16_t)speed : 0)) { fail("move"); return; }
    Serial.printf("  goal %u at speed %ld sent\n", pos, speed >= 0 ? speed : 0);
}

/**
 * Stop where it is, still holding.
 *
 * There is no stop instruction on this protocol — a move is a goal, not a
 * command you can cancel — so stopping means making the goal the present
 * position. It still holds afterwards: `torque off` is the other kind of stop,
 * and on a gate they mean very different things.
 */
static void doStop() {
    uint16_t pos = 0;
    if (!bus.read16(target, ST_REG_PRESENT_POS, &pos)) { fail("read position"); return; }
    // Aim at where it is, at the speed it can decelerate with. It will overshoot
    // and come back — a full-speed run overshot ~1000 counts on the bench — so
    // "stopped" means "settled near here", not "froze on the spot".
    if (!bus.moveTo(target, pos, 0)) { fail("stop"); return; }
    Serial.printf("  aiming at %u — it overshoots and returns; `read` to see where it settled\n", pos);
}

static void doTorque(bool on) {
    if (!bus.write8(target, ST_REG_TORQUE_ENABLE, on ? 1 : 0)) { fail("torque"); return; }
    Serial.printf("  torque %s\n", on ? "ON — it will hold, and it will move when told"
                                      : "off — the shaft is free");
}

static void doMode(uint8_t mode) {
    // Mode lives in EEPROM behind the register-55 lock, so this goes through the
    // unlock/write/verify path. A plain write reads back correct and evaporates
    // on the next power cycle — see ST3215Bus::eepromUnlocked.
    if (!bus.writeEeprom8(target, ST_REG_MODE, mode)) { fail("mode"); return; }
    Serial.printf("  mode %u written and verified — this one survives a power cycle\n", mode);
}

// -----------------------------------------------------------------------------
// The suite
//
// One command that exercises everything this bench has established, prints a
// PASS/FAIL line per check and a compact baseline at the end. It exists so a
// build's behaviour is a REPORT rather than a transcript of hand-typed commands
// interpreted twice — half the wrong conclusions in this file's history came
// from a command landing in a state left by the one before it.
//
// Rules for anything added here:
//   - set the state you depend on, never inherit it;
//   - put the servo back the way you found it before returning;
//   - a check that cannot fail is not a check, it is a measurement — label it
//     MEAS and leave it out of the pass count.
// -----------------------------------------------------------------------------

static uint8_t sPass, sFail, sStep;

static void check(const char* name, bool ok, const char* fmt, ...) {
    char detail[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    ok ? sPass++ : sFail++;
    Serial.printf("  [%2u] %-22s %s  %s\n", ++sStep, name, ok ? "PASS" : "FAIL", detail);
}

static void meas(const char* name, const char* fmt, ...) {
    char detail[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    Serial.printf("  [%2u] %-22s MEAS  %s\n", ++sStep, name, detail);
}

void doSuite() {
    sPass = sFail = sStep = 0;
    Serial.println(F("\n  ST3215 suite — THE SHAFT WILL TURN. Nothing should be bolted to it.\n"));

    // ---- it is there, and we know what state it is in -----------------------
    uint8_t err = 0;
    if (!bus.ping(target, &err)) {
        Serial.printf("  no servo at id %u — `sweep` first. Suite aborted.\n", target);
        return;
    }
    check("ping", err == 0, "id %u, status 0x%02X", target, err);

    // Through the lock, or these are RAM-only and the next power cycle undoes
    // them without saying so.
    bus.writeEeprom8(target, ST_REG_MODE, 0);
    bus.writeEeprom16(target, ST_REG_MIN_ANGLE, 0);
    bus.writeEeprom16(target, ST_REG_MAX_ANGLE, ST_POS_MAX);
    bus.write8(target, ST_REG_TORQUE_ENABLE, 1);          // RAM, no lock involved

    uint8_t mode = 255, torque = 255;
    uint16_t lo = 1, hi = 0;
    // A failed READ is not a failed STATE, and conflating them cost a run: a
    // transient miss right after a servo power cycle printed "torque 255" and
    // read as a fault in the servo rather than a dropped reply.
    bool got = bus.read8(target, ST_REG_MODE, &mode)
             & bus.read8(target, ST_REG_TORQUE_ENABLE, &torque)
             & bus.read16(target, ST_REG_MIN_ANGLE, &lo)
             & bus.read16(target, ST_REG_MAX_ANGLE, &hi);
    if (!got) {
        check("state readback", false, "a register read failed: %s — bus, not state", bus.lastError());
    } else {
        check("state readback", mode == 0 && torque == 1 && lo == 0 && hi == ST_POS_MAX,
              "mode %u torque %u limits %u..%u", mode, torque, lo, hi);
    }

    uint8_t volt = 0, temp = 0;
    bus.read8(target, ST_REG_PRESENT_VOLTAGE, &volt);
    bus.read8(target, ST_REG_PRESENT_TEMP, &temp);
    // Wide bounds on purpose: this rig has run at 9V and at 12V, and the check
    // is "plausible supply", not "the supply I expected".
    check("supply + temp", volt >= 60 && volt <= 145 && temp < 60,
          "%.1fV, %u C", volt / 10.0f, temp);

    // ---- it goes where it is told -------------------------------------------
    // Park first: "travel to 3000" from 2997 passed in 0.00s once, which proved
    // nothing at all.
    settleAt(500, 0);
    Settle a = settleAt(3000, 0);
    check("travel to 3000", a.arrived && a.from != a.pos, "%u -> %u in %.2fs", a.from, a.pos, a.ms / 1000.0f);
    Settle b = settleAt(500, 0);
    check("travel to 500", b.arrived && b.from != b.pos, "%u -> %u in %.2fs", b.from, b.pos, b.ms / 1000.0f);

    meas("top speed", "%.0f counts/s = %.1f rpm at speed 0",
         measuredCps(b), measuredCps(b) * 60.0f / 4096.0f);

    // ---- speed means counts per second --------------------------------------
    // Loose tolerance by design: a settle includes both acceleration ramps, so a
    // measured figure is always under the setting. The question is whether the
    // number tracks, not whether it matches.
    //
    // These failed flat (asked 300, got 1599) until the move became a single
    // block write of position+time+speed. A standalone GOAL_SPEED write is
    // accepted, reads back, and does nothing.
    struct { uint16_t goal; long speed; } legs[] = { { 3000, 300 }, { 500, 1200 } };
    for (auto& leg : legs) {
        Settle s = settleAt(leg.goal, leg.speed);
        float cps = measuredCps(s);
        float ratio = leg.speed ? cps / leg.speed : 0;
        check("speed tracks", s.arrived && ratio > 0.6f && ratio < 1.15f,
              "asked %ld, measured %.0f counts/s (%.0f%%)", leg.speed, cps, ratio * 100);
    }

    // Does the servo even keep what we wrote? Worth its own line, because
    // "written, readable, and ignored" is a state we have now seen.
    uint16_t spdBack = 0;
    bus.read16(target, ST_REG_GOAL_SPEED, &spdBack);
    meas("goal speed readback", "register 46 holds %u after the last move", spdBack);

    // ---- a goal written mid-move is honoured --------------------------------
    // This is what `stop` is built on, and what a make-before-break gate change
    // will lean on: the servo must abandon a move in flight when re-aimed.
    bus.write16(target, ST_REG_GOAL_SPEED, 400);
    bus.write16(target, ST_REG_GOAL_POSITION, 3500);
    delay(600);                                   // let it get properly under way
    Settle r = settleAt(1000, 400);
    check("retarget mid-move", r.arrived, "diverted to %u", r.pos);

    // ---- stop, and hold ------------------------------------------------------
    // A stop is not a freeze. Caught at full speed the shaft overshot ~1000
    // counts and then came back, and sampling 500ms in caught it mid-return and
    // called it a failure. So: let it settle, then ask the two questions that
    // actually matter — did it end up near where it was told, and did it STAY.
    settleAt(500, 0);
    bus.moveTo(target, 3500, 400);
    delay(900);
    uint16_t caught = 0;
    bus.read16(target, ST_REG_PRESENT_POS, &caught);
    bus.moveTo(target, caught, 0);                      // this is what `stop` does

    uint16_t peak = caught, settled = caught;
    uint32_t until = millis() + 2500;
    while ((int32_t)(millis() - until) < 0) {
        delay(20);
        bus.read16(target, ST_REG_PRESENT_POS, &settled);
        if (settled > peak) peak = settled;
    }
    uint16_t later = 0;
    delay(600);
    bus.read16(target, ST_REG_PRESENT_POS, &later);

    meas("stopping distance", "overshot %ld counts past the catch at speed 400",
         (long)peak - (long)caught);
    check("stop and hold", labs((long)later - (long)settled) <= kCloseEnough
                           && labs((long)settled - (long)caught) < 200,
          "caught %u, settled %+ld, then moved %+ld",
          caught, (long)settled - (long)caught, (long)later - (long)settled);

    // ---- does it land in the same place twice? -------------------------------
    uint16_t lowest = 0xFFFF, highest = 0;
    for (uint8_t i = 0; i < 3; i++) {
        settleAt(1000, 0);
        Settle s = settleAt(3000, 800);
        if (s.pos < lowest)  lowest = s.pos;
        if (s.pos > highest) highest = s.pos;
    }
    check("repeatability", (highest - lowest) <= 12, "3 approaches spread %u counts (%u..%u)",
          highest - lowest, lowest, highest);

    // ---- the EEPROM lock -----------------------------------------------------
    // Register 55 guards every setting below address 40. Written past it, a
    // value reads back correctly and vanishes on the next restart, which is how
    // "mode 3, confirmed" survived a dozen readbacks and no power cycles.
    uint8_t lockState = 255;
    bus.read8(target, ST_REG_LOCK, &lockState);
    check("eeprom re-locked", lockState == 1, "register 55 reads %u after the writes above", lockState);

    // ---- travel past one turn ------------------------------------------------
    //
    // WHAT THE VENDOR DOCS SAY, found after three bench attempts failed:
    //   - Mode 3 is STEPPING mode, and register 42 is not a position there. It
    //     is a NUMBER OF STEPS, with bit 15 as the direction. So "goal 8192"
    //     was never a request to go to two turns, and the 3 counts it moved
    //     were the servo doing something else entirely.
    //   - Mode 3 also requires BOTH angle limits at 0, "otherwise it is
    //     impossible to step indefinitely".
    //   - Mode 0 stops at one turn by design. That is not a bug to chase.
    //
    // The combination never yet tried is mode 3 + limits 0 + a POWER CYCLE of
    // the servo, and nothing in firmware can power-cycle it. So this is a
    // measurement with instructions, not a check: a suite should not fail on a
    // step a human has to perform. `stepmode on` sets it up.
    bus.writeEeprom16(target, ST_REG_MIN_ANGLE, 0);
    bus.writeEeprom16(target, ST_REG_MAX_ANGLE, 0);
    delay(50);
    uint16_t mlo = 1, mhi = 1;
    bus.read16(target, ST_REG_MIN_ANGLE, &mlo);
    bus.read16(target, ST_REG_MAX_ANGLE, &mhi);
    check("limits cleared", mlo == 0 && mhi == 0, "read back %u..%u", mlo, mhi);

    bus.writeEeprom8(target, ST_REG_MODE, 3);
    delay(50);
    uint16_t before = 0;
    bus.read16(target, ST_REG_PRESENT_POS, &before);
    bus.moveTo(target, 4096, 1500);              // 4096 STEPS, one turn's worth
    delay(6000);
    uint16_t after = 0;
    bus.read16(target, ST_REG_PRESENT_POS, &after);
    meas("step mode 4096 steps", "%u -> %u, moved %ld counts (no power cycle since mode 3 was set)",
         before, after, labs((long)after - (long)before));

    // ---- put it back ---------------------------------------------------------
    bus.writeEeprom8(target, ST_REG_MODE, 0);
    bus.writeEeprom16(target, ST_REG_MIN_ANGLE, 0);
    bus.writeEeprom16(target, ST_REG_MAX_ANGLE, ST_POS_MAX);
    delay(50);
    settleAt(2048, 800);
    bus.read8(target, ST_REG_MODE, &mode);
    bus.read16(target, ST_REG_MAX_ANGLE, &hi);
    check("restored", mode == 0 && hi == ST_POS_MAX, "mode %u, limits 0..%u, parked at centre", mode, hi);

    Serial.printf("\n  %u passed, %u failed, %u checks total.\n", sPass, sFail, sStep);
    Serial.println(F("  Paste this block back; it is the baseline for the next build.\n"));
}

// -----------------------------------------------------------------------------
// The console
// -----------------------------------------------------------------------------

/** Word `n` of the line, or "" — a tiny split so the parser below reads flat. */
static String arg(const String& line, int n) {
    int start = 0;
    for (int word = 0; ; word++) {
        while (start < (int)line.length() && line[start] == ' ') start++;
        int end = line.indexOf(' ', start);
        if (end < 0) end = line.length();
        if (word == n) return line.substring(start, end);
        if (end >= (int)line.length()) return String();
        start = end + 1;
    }
}

static void handle(const String& line) {
    String cmd = arg(line, 0);
    if (!cmd.length()) return;

    if (cmd == "help" || cmd == "?")      { help(); return; }
    if (cmd == "scan")                    { doScan(arg(line,1).length() ? arg(line,1).toInt() : 20); return; }
    if (cmd == "sweep")                   { doSweep(arg(line,1).length() ? arg(line,1).toInt() : 20); return; }
    if (cmd == "selftest") { doSelfTest(arg(line,1) != "wire"); return; }
    if (cmd == "continuity") { doContinuity(); return; }
    if (cmd == "blast")    { doBlast(arg(line,1).length() ? (uint32_t)arg(line,1).toInt() : 5); return; }
    if (cmd == "pins") {
        Serial.printf("  TX GPIO%d, RX GPIO%d (%s)\n", bus.txPin(), bus.rxPin(),
                      bus.swapped() ? "swapped" : "as the board header has them");
        return;
    }
    if (cmd == "swap") {
        bus.begin(busBaud, !bus.swapped());
        Serial.printf("  reopened: TX GPIO%d, RX GPIO%d\n", bus.txPin(), bus.rxPin());
        return;
    }

    if (cmd == "baud") {
        long b = arg(line, 1).toInt();
        if (b <= 0) { Serial.println(F("  baud <n>")); return; }
        busBaud = (uint32_t)b;
        bus.begin(busBaud);
        Serial.printf("  bus reopened at %lu\n", (unsigned long)busBaud);
        return;
    }
    if (cmd == "id") {
        long v = arg(line, 1).toInt();
        if (v < 0 || v > 253) { Serial.println(F("  id 0..253")); return; }
        target = (uint8_t)v;
        Serial.printf("  talking to id %u\n", target);
        return;
    }

    if (cmd == "ping") {
        uint8_t err = 0;
        if (!bus.ping(target, &err)) { fail("ping"); return; }
        Serial.printf("  id %u is there\n", target);
        printErrBits(err);
        // Which of the two wirings this is, answered by the bus rather than by
        // looking at the bench. Both work; knowing which one you are on is what
        // makes the next odd symptom readable.
        Serial.printf("  wiring: %s\n", bus.echoSeen()
            ? "our own frames come back — TX and RX share the wire"
            : "no echo — something is turning the line around (buffered adapter)");
        return;
    }
    if (cmd == "read")   { doRead(); return; }
    if (cmd == "watch")  { watching = true; nextTick = 0; Serial.println(F("  watching — press any key")); return; }
    if (cmd == "dump")   { doDump((uint8_t)arg(line,1).toInt(), (uint8_t)max(1L, arg(line,2).toInt())); return; }

    if (cmd == "torque") { doTorque(arg(line,1) == "on"); return; }
    if (cmd == "move") {
        if (!arg(line,1).length()) { Serial.println(F("  move <0..4095> [speed]")); return; }
        doMove((uint16_t)arg(line,1).toInt(), arg(line,2).length() ? arg(line,2).toInt() : -1);
        return;
    }
    if (cmd == "stop")   { doStop(); return; }
    if (cmd == "centre" || cmd == "center") { doMove(2048, -1); return; }
    if (cmd == "mode")   { doMode((uint8_t)arg(line,1).toInt()); return; }
    if (cmd == "limits")   { doLimits(arg(line,1) != "turn"); return; }
    if (cmd == "stepmode") { doStepMode(arg(line,1) != "off"); return; }
    if (cmd == "zero") {
        absPos = 0; haveLast = false;
        Serial.println(F("  tracked position zeroed here"));
        return;
    }
    if (cmd == "step") {
        if (!arg(line,1).length()) { Serial.println(F("  step <steps> — signed; 4096 is a turn's worth")); return; }
        doStep(arg(line,1).toInt());
        return;
    }
    if (cmd == "suite")  { doSuite(); return; }
    if (cmd == "timeit") {
        if (!arg(line,1).length()) { Serial.println(F("  timeit <pos> [speed]")); return; }
        doTimeIt((uint16_t)arg(line,1).toInt(), arg(line,2).length() ? arg(line,2).toInt() : -1);
        return;
    }
    if (cmd == "mturn")  {
        if (!arg(line,1).length()) { Serial.println(F("  mturn <counts> [speed] — 4096 counts is one turn")); return; }
        doMTurn(arg(line,1).toInt(), arg(line,2).length() ? arg(line,2).toInt() : -1);
        return;
    }

    if (cmd == "w8")     {
        if (!bus.write8(target, (uint8_t)arg(line,1).toInt(), (uint8_t)arg(line,2).toInt())) fail("w8");
        else Serial.println(F("  written"));
        return;
    }
    if (cmd == "w16")    {
        if (!bus.write16(target, (uint8_t)arg(line,1).toInt(), (uint16_t)arg(line,2).toInt())) fail("w16");
        else Serial.println(F("  written"));
        return;
    }
    if (cmd == "endian") {
        bus.littleEndian(arg(line,1) != "hi");
        Serial.printf("  16-bit registers read %s-endian\n", bus.isLittleEndian() ? "little" : "big");
        return;
    }
    if (cmd == "trace")  {
        bus.trace(arg(line,1) == "on");
        Serial.printf("  trace %s\n", bus.tracing() ? "on" : "off");
        return;
    }

    Serial.printf("  ? %s — try `help`\n", cmd.c_str());
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    // Native USB: the port only exists once a host opens it, and printing into a
    // port nobody has opened is how a banner gets lost. Wait, but not forever —
    // this board is often powered from a bench supply with no host at all.
    uint32_t until = millis() + 2000;
    while (!Serial && (int32_t)(millis() - until) < 0) delay(10);

    bus.begin(busBaud);
    banner();
}

/** The background poll that makes the turn count mean anything. */
static void trackerTick() {
    static uint32_t next = 0;
    if ((int32_t)(millis() - next) < 0) return;
    next = millis() + 100;                 // well inside the half-turn rule

    uint16_t pos = 0;
    if (bus.read16(target, ST_REG_PRESENT_POS, &pos)) trackPosition(pos);
}

void loop() {
    static String line;

    // Before the console, so a move commanded and left running is still being
    // counted while nobody is typing.
    trackerTick();

    while (Serial.available()) {
        char c = Serial.read();
        if (watching) { watching = false; Serial.println(F("  (stopped)")); line = ""; continue; }
        if (c == '\r') continue;
        if (c == '\n') { Serial.printf("> %s\n", line.c_str()); handle(line); line = ""; continue; }
        if (line.length() < 64) line += c;
    }

    if (watching && (int32_t)(millis() - nextTick) >= 0) {
        nextTick = millis() + 200;
        doRead();
    }
}

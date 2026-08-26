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

static void banner() {
    Serial.println();
    Serial.println(F("ST3215 bench — one servo, one wire, no gate."));
    Serial.printf("  bus: TX D6/GPIO%d  RX D7/GPIO%d  @ %lu baud, %s-endian\n",
                  PIN_SERVO_BUS_TX, PIN_SERVO_BUS_RX,
                  (unsigned long)busBaud, bus.isLittleEndian() ? "little" : "big");
    Serial.printf("  target id: %u   torque: not touched since boot\n", target);
    Serial.println(F("  type `help`"));
}

static void help() {
    Serial.println(F(
        "\n"
        "  scan [hi]        ping every id from 0 to hi (default 20)\n"
        "  sweep [hi]       scan at every plausible baud, both pin orders, and stay on what works\n"
        "  pins             which pad is TX and which is RX right now\n"
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
        "\n"
        "  w8 <addr> <v>    poke a byte    (careful: EEPROM writes below 40 persist)\n"
        "  w16 <addr> <v>   poke a word\n"
        "  endian lo|hi     16-bit byte order, if the numbers come back nonsense\n"
        "  trace on|off     every frame in both directions, as hex\n"));
}

static void fail(const char* what) {
    Serial.printf("  %s failed: %s\n", what, bus.lastError());
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

/** Feetech puts the sign of speed and load in bit 15, not in two's complement. */
static int16_t signedField(uint16_t raw) {
    int16_t mag = (int16_t)(raw & 0x7FFF);
    return (raw & 0x8000) ? (int16_t)-mag : mag;
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

    Serial.printf("  id %u  pos %4u (%3u deg)  speed %5d  load %5d  %4.1fV  %u C  %s\n",
                  target, pos, (unsigned)((uint32_t)pos * 360 / (ST_POS_MAX + 1)),
                  signedField(spd), signedField(load),
                  volt / 10.0f, temp, moving ? "MOVING" : "still");
    if (cur) Serial.printf("  current %u (units per the register map, unconfirmed)\n", cur);
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
    if (speed >= 0 && !bus.write16(target, ST_REG_GOAL_SPEED, (uint16_t)speed)) {
        fail("set speed"); return;
    }
    if (!bus.write16(target, ST_REG_GOAL_POSITION, pos)) { fail("move"); return; }
    Serial.printf("  goal %u sent%s\n", pos,
                  speed >= 0 ? "" : " (speed left as it was)");
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
    if (!bus.write16(target, ST_REG_GOAL_POSITION, pos)) { fail("stop"); return; }
    Serial.printf("  stopped at %u, still holding (`torque off` to let go)\n", pos);
}

static void doTorque(bool on) {
    if (!bus.write8(target, ST_REG_TORQUE_ENABLE, on ? 1 : 0)) { fail("torque"); return; }
    Serial.printf("  torque %s\n", on ? "ON — it will hold, and it will move when told"
                                      : "off — the shaft is free");
}

static void doMode(uint8_t mode) {
    // Mode lives in EEPROM. It survives a power cycle, which is the point, and
    // it also means this is not a register to poke in a loop.
    if (!bus.write8(target, ST_REG_MODE, mode)) { fail("mode"); return; }
    Serial.printf("  mode %u written (EEPROM — it persists)\n", mode);
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

void loop() {
    static String line;

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

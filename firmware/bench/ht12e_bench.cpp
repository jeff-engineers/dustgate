// =============================================================================
// ht12e_bench.cpp — a serial console for injecting HT12E codes at a Rockler
// dust collector remote switch.
//
// This is not part of DustGate. It is the smallest program that can answer the
// only questions worth asking the day the parts arrive:
//
//     is the oscillator right?      osc
//     does the receiver hear me?    vt        (needs the HT12D stage, below)
//     WHICH data pin is the button? scan
//     does the collector switch?    tx
//
// WHY A SEPARATE PROGRAM. There is no WiFi, no topology and no node here.
// A first-contact RF bug should be a bug in one screenful of code, not somewhere
// in a program that also joins a network and serves an Angular bundle. Once the
// numbers below are known, they move into the real driver as constants and this
// file stops mattering.
//
// NOTHING TRANSMITS AT BOOT. Same rule as the servo bench: a board that resets
// must not key a dust collector. Every burst is a command you typed.
//
// WHAT IS ALREADY KNOWN (docs/tool-sensing-rfc.md §4.2, read off the hardware):
//     band      315 MHz          FCC ID VFWPD5T, Part 15.231
//     encoder   HT12E            12 bits: 8 address + 4 data
//     address   DIP 1, 6, 8 on   → A0=0 A1=1 A2=1 A3=1 A4=1 A5=0 A6=1 A7=0
//     buttons   one              → exactly one of AD8..AD11 is asserted; which
//                                  one is the ONLY unknown, and `scan` finds it
//
// ⚠️ Read firmware/wiring/ht12e-bench.md before wiring anything.
//
// Build and flash:
//     PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e xiao_c5_ht12e_bench -t upload
//     PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio device monitor -e xiao_c5_ht12e_bench
// =============================================================================

#include <Arduino.h>
#include "../config.h"

// -- Pins ---------------------------------------------------------------------
//
// A bench board has nothing else on it, so this map is chosen for clarity rather
// than by elimination. D3 is deliberately unused: it is GPIO7, a strapping pin
// (see boards/xiao_c5.h), and the HT12E's OSC2 idles in a state nobody has
// measured — not a line to hold a strap with at reset.
static const int PIN_TE   = 0;    // D1  → HT12E pin 14, TE (active low)
static const int PIN_OSC  = 1;    // D0  ← HT12E pin 15, OSC2 (frequency check)
static const int PIN_VT   = 11;   // D6  ← HT12D pin 17, VT  (optional stage 2)
// D4 → the TX module's DATA — the encoder-less path, where the ESP32 keys the
// transmitter directly and the HT12E is out of the circuit.
//
// ⚠️ ONE WIRE, MOVED. Do NOT tie this and HT12E pin 17 to DATA together. DOUT is
// a CMOS output and is never high-impedance — in standby it is still driving —
// so both connected is two outputs fighting over the same net, not a harmless
// wired-OR. (The AD pins tolerate that because nothing ever drives them HIGH;
// DOUT is the opposite case.) To A/B, move the DATA jumper between HT12E 17 and
// D4. Same board, same module, same antenna, same receiver — which is the whole
// reason to do it on one prototype rather than two.
static const int PIN_TXDATA = 23; // D4  → TX module DATA (encoder bypassed)
static const int PIN_AD[4] = {
    12,   // D7  → HT12E pin 10, AD8
    8,    // D8  → HT12E pin 11, AD9
    9,    // D9  → HT12E pin 12, AD10
    10,   // D10 → HT12E pin 13, AD11
};

// -- The three-state trick, and why it is not optional ------------------------
//
// HT12E address and data pins are transmission gates with NO internal pull-up
// (that is the HT12A; datasheet p3). Holtek's documented usage is "set to VSS or
// left open", so a logic 1 is genuinely OPEN — not driven high.
//
// Driving one high would probably work, and "probably" is not what you want
// underneath a result you are about to trust. So: OUTPUT LOW to assert a 0, and
// INPUT (high-Z) to leave it open for a 1. Same for TE, which does have a
// pull-high (~1.5 MΩ) and so idles high on its own.
//
// A USEFUL CONSEQUENCE: because nothing here ever drives a pin HIGH, a GPIO and
// a DIP switch on the same AD pin wired-OR correctly — whichever pulls it down
// wins, and a released GPIO leaves the DIP in charge. So the data pins can be on
// a DIP instead if you prefer; wire both, leave the console at `data 15`, and
// flip switches. The GPIOs are here because finding WHICH pin the button asserts
// is the one open question, and `scan` answers it hands-free while you watch the
// collector. The ADDRESS pins are the opposite case — set once to match the fob,
// never swept — so those are a DIP switch and are not wired to the ESP32 at all.
//
// This is a BENCH affordance. On a real primary the data pin is known by then
// and gets strapped, leaving TE as the only GPIO the transmitter needs — which
// matters, because a four-gate primary has exactly two pads free.
static inline void assertLow(int pin) { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
static inline void release(int pin)   { pinMode(pin, INPUT); }

// The 4-bit data word as the HT12E will TRANSMIT it: bit 0 = AD8 … bit 3 = AD11,
// a 1 meaning "left open". 0b1111 is the resting state.
//
// DEFAULTS TO THE ANSWER (2026-09-06). A full sweep keys the Rockler receiver on
// every EVEN value and nothing odd — so the rule is AD8 low, and AD9/AD10/AD11
// are don't-cares. 0b1110 asserts AD8 alone and leaves the rest open, which is
// what the fob's single button does. `tx` on a fresh boot now does something
// useful instead of sending the resting word.
static uint8_t g_data = 0b1110;

// How long TE is held low, which is how long we transmit — the HT12E repeats
// its 4-word group for as long as TE stays down.
//
// 500 ms, and that number is MEASURED, not derived. 120 ms keyed the receiver
// only intermittently while the fob was rock solid; 500 ms is reliable
// (2026-09-06). The 120 was arithmetic off a block diagram — one 4-word group
// "plus a bit" — and that arithmetic was never trustworthy: the word rate comes
// off fOSC through the ÷3 divider and the datasheet gives no word duration
// directly. It briefly sat at 400 as a guess between the two; 500 replaced it
// the moment there was a real result to use instead.
//
// What IS in the datasheet is that four words is the MINIMUM transmission for an
// HT12E, and that number exists because the decoder validates by seeing the same
// frame more than once. Cut a group short and you do not get a wrong output, you
// get no output — which looks exactly like a range problem.
//
// So: send like a person. Nobody stabs a remote for 120 ms; a press is several
// hundred milliseconds and the fob transmits continuously throughout. `hold` is
// still a command precisely because this number is empirical.
static uint32_t g_holdMs = 500;

static void applyData(uint8_t d) {
    for (int i = 0; i < 4; i++) {
        if (d & (1 << i)) release(PIN_AD[i]);
        else              assertLow(PIN_AD[i]);
    }
}

// One transmission. Returns whether the HT12D (if wired) saw a valid frame.
static bool transmit(uint8_t d, uint32_t holdMs) {
    applyData(d);
    delay(2);                       // let the gates settle before TE
    bool sawVt = false;
    assertLow(PIN_TE);
    const uint32_t until = millis() + holdMs;
    while ((int32_t)(millis() - until) < 0) {
        if (digitalRead(PIN_VT) == HIGH) sawVt = true;
        delay(1);
    }
    release(PIN_TE);
    applyData(0b1111);              // rest with nothing asserted
    return sawVt;
}

// -- fOSC ---------------------------------------------------------------------
//
// COUNT EDGES OVER A WINDOW. Do not time individual pulses.
//
// The first version paired pulseIn(HIGH) with pulseIn(LOW) and summed them as
// one period. They are measurements of two DIFFERENT cycles, an edge can slip
// between the two calls, and pulseIn is a busy-wait that any interrupt stretches.
// On a steady oscillator it read 3470-3749 Hz — +/-4% of pure method, and only
// 16-23 of its 32 attempts even landed.
//
// Counting is immune to both: an interrupt can delay NOTICING an edge, but the
// edge is still counted. At ~3.5 kHz a 250 ms window is ~875 edges, so one edge
// either way is about 0.1%.
static volatile uint32_t g_edges = 0;
static void IRAM_ATTR oscIsr() { g_edges++; }

// assertTe=false measures whatever is on D0 WITHOUT keying our own encoder —
// that is how you measure the FOB. Clip D0 to the fob's OSC2 (pin 15) and hold
// its button; its oscillator only runs while transmitting, same as ours.
static void measureOsc(bool assertTe) {
    if (assertTe) { assertLow(PIN_TE); delay(5); }

    g_edges = 0;
    attachInterrupt(digitalPinToInterrupt(PIN_OSC), oscIsr, RISING);
    const uint32_t t0 = millis();
    while (millis() - t0 < 250) { delay(1); }
    detachInterrupt(digitalPinToInterrupt(PIN_OSC));
    const uint32_t edges = g_edges, took = millis() - t0;

    if (assertTe) { release(PIN_TE); applyData(0b1111); }

    if (edges < 10) {
        Serial.printf("  %lu edges in %lums — not oscillating.\n",
                      (unsigned long)edges, (unsigned long)took);
        Serial.println(assertTe
            ? F("  Rosc missing or open, HT12E unpowered, or OSC2 not on D0.")
            : F("  Nothing driving D0. Is the fob's button held?"));
        return;
    }
    Serial.printf("  fOSC %.0f Hz  (%lu edges in %lums)\n",
                  (float)edges * 1000.0f / (float)took,
                  (unsigned long)edges, (unsigned long)took);

    // DELIBERATELY NOT A PASS/FAIL. The number that matters is what the FOB runs
    // at: the HT12D in the receiver was built around the fob, and its own
    // oscillator is fixed. Holtek's ~3 kHz is a reference point, not a
    // requirement, and cheap CMOS RC oscillators spread widely part to part at
    // both ends — which is exactly why the capture window has to be generous.
    if (assertTe)
        Serial.println(F("  compare with `oscf` — the fob's rate is the one to match."));
}

// =============================================================================
// The encoder-less path: generate the HT12E waveform ourselves, in RMT hardware.
//
// WHY THIS EXISTS. The HT12E is a guarantee — its waveform is right by
// construction — but it is also three parts (encoder, address DIP, Rosc) and a
// hardware-fixed address. Generating the frame instead drops the BOM to the
// ESP32 and the transmitter, and makes the address a SOFTWARE value, which is
// what lets one board work with any receiver without jumpers.
//
// WHY RMT AND NOT delayMicroseconds(). This is option B from
// docs/tool-sensing-rfc.md §4.2, which was rejected for timing: the primary runs
// WiFi, and FreeRTOS will preempt a bit-banging loop and stretch a pulse. RMT
// clocks the pulse train out in HARDWARE from a list of (duration, level) pairs,
// so nothing the CPU does afterwards can disturb it. That single difference is
// what turns "reimplementing the protocol badly" into "reimplementing it".
//
// THE SHAPE comes from RCSwitch protocol 11 — { 270, {36,1}, {1,2}, {2,1}, true }
// — a base tick, a long sync, and two three-tick symbols. But the tick is NOT
// taken on faith: our encoder measured ~3.5 kHz where Holtek's example says 3.0,
// so this chip's symbols are around 17% shorter than a nominal one's. `tick` and
// `inv` are runtime settings precisely because the published numbers are a
// starting point and the receiver is the authority.
// =============================================================================
static uint32_t g_tickUs  = 270;    // RCSwitch protocol 11's base tick
static bool     g_inv     = true;   // protocol 11 is an inverted protocol
static uint16_t g_repeats = 24;     // ~500 ms at 270 us, matching the proven hold

// Address as EIGHT BITS, bit i = A_i, 1 = "left open" = logic 1. The Rockler's
// DIP has rockers 1, 6, 8 closed — A0, A5, A7 grounded — which is 0b01011110.
// THE POINT OF THE WHOLE EXERCISE: this is a variable, not a switch.
static uint8_t  g_addr = 0b01011110;

static bool     g_rmtReady = false;

// One HT12E symbol as an RMT entry: a low run then a high run, or the reverse
// when g_inv is false. RCSwitch counts {high, low} in ticks and then inverts the
// levels, so a "1" is {2,1} → two ticks low, one tick high.
static void sym(rmt_data_t& e, uint8_t firstTicks, uint8_t secondTicks) {
    e.level0    = g_inv ? 0 : 1;
    e.duration0 = firstTicks  * g_tickUs;
    e.level1    = g_inv ? 1 : 0;
    e.duration1 = secondTicks * g_tickUs;
}

// 12 bits (A0..A7 then AD8..AD11, transmission order per the datasheet) plus the
// sync, repeated. A repeating frame is cyclic, so sync-last matches sync-first.
static void rmtSend() {
    if (!g_rmtReady) {
        if (!rmtInit(PIN_TXDATA, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 1000000)) {
            Serial.println(F("  rmtInit failed"));
            return;
        }
        g_rmtReady = true;
    }
    rmt_data_t frame[13];
    for (int i = 0; i < 8; i++) {                       // address, A0 first
        const bool one = (g_addr >> i) & 1;
        if (one) sym(frame[i], 2, 1); else sym(frame[i], 1, 2);
    }
    for (int i = 0; i < 4; i++) {                       // data, AD8 first
        const bool one = (g_data >> i) & 1;
        if (one) sym(frame[8 + i], 2, 1); else sym(frame[8 + i], 1, 2);
    }
    sym(frame[12], 36, 1);                              // sync / pilot

    for (uint16_t r = 0; r < g_repeats; r++) {
        rmtWrite(PIN_TXDATA, frame, 13, RMT_WAIT_FOR_EVER);
    }
}

// -- Console ------------------------------------------------------------------
static void banner() {
    Serial.println(F("\nHT12E bench — 315 MHz injection for the Rockler DC switch"));
    Serial.println(F("  tx            send one burst with the current data word"));
    Serial.println(F("  data <0-15>   set AD8..AD11 (bit0=AD8). 15 = all open = idle"));
    Serial.println(F("  scan          try the four one-button patterns, 2s apart"));
    Serial.println(F("  sweep         all 16 data words, 2s apart"));
    Serial.println(F("  hold <ms>     TE low duration (now 500, measured) — raise if flaky"));
    Serial.println(F("  osc           measure OUR fOSC on OSC2"));
    Serial.println(F("  oscf          measure the FOB's fOSC — the rate to match"));
    Serial.println(F("  vt            report whether the HT12D saw the last burst"));
    Serial.println(F("\n  -- encoder-less path: move the DATA jumper to D4 first --"));
    Serial.println(F("  rmt           send the frame from RMT hardware, no HT12E"));
    Serial.println(F("  tick <us>     base tick (now 270, from RCSwitch protocol 11)"));
    Serial.println(F("  rsweep [a b s] sweep the tick to find the WINDOW (default 100-400/10)"));
    Serial.println(F("  inv <0|1>     invert the waveform (now 1)"));
    Serial.println(F("  addr <0-255>  address in SOFTWARE. bit i = A_i, 1 = open"));
    Serial.println(F("  reps <n>      frame repeats (now 24, ~= the 500ms hold)"));
    Serial.println(F("\nNOTHING TRANSMITS UNTIL YOU TYPE IT."));
    // A lamp, not the collector: nobody has a dust collector at their bench, and
    // a lamp is the better indicator anyway — instant, unambiguous, no spin-up to
    // misread. `sweep` especially: sixteen motor starts two seconds apart is
    // locked-rotor inrush sixteen times for nothing, AND unreadable, because a
    // blower cannot spin up and coast down inside a two-second window so the
    // results smear across patterns.
    Serial.println(F("Put a LAMP in the Rockler receiver's outlet and watch that."));
    Serial.println(F("(Listening for the relay click works too, with nothing plugged in.)\n"));
}

// Anything typed aborts a run — but only something typed AFTER it starts.
//
// This used to be a bare Serial.available(), and it stopped every scan dead
// after exactly one pattern: handle() is called from inside loop()'s read loop,
// on the '\r', so with a CRLF terminal the '\n' is still unread in the buffer
// and the very first check saw it. Drain, then watch.
static void drainInput() { while (Serial.available()) Serial.read(); }
static bool aborted() {
    if (!Serial.available()) return false;
    drainInput();
    Serial.println(F("  stopped."));
    return true;
}

// One candidate. Prints the WHOLE line at once: a partial line followed by a
// blocking call is how a stall becomes invisible, and Serial.flush() on USB
// Serial/JTAG blocks until the host drains the FIFO, which is its own hazard.
static void tryPattern(const char* label, uint8_t d) {
    const bool vt = transmit(d, g_holdMs);
    Serial.printf("  %s  data %2u (AD11..AD8 = %u%u%u%u)  %s\n",
                  label, d, (d>>3)&1, (d>>2)&1, (d>>1)&1, d&1,
                  vt ? "sent, HT12D decoded it" : "sent");
}

static void scan(uint8_t from, uint8_t to) {
    drainInput();
    Serial.println(F("  watch the lamp — the pattern that switches it is the answer"));
    Serial.println(F("  the receiver TOGGLES, so watch for CHANGES, not for 'on'"));
    Serial.println(F("  (press any key to stop)"));
    for (uint16_t d = from; d <= to; d++) {
        tryPattern("     ", (uint8_t)d);
        delay(2000);
        if (aborted()) return;
    }
    Serial.println(F("  sweep complete."));
}

static void handle(String line) {
    line.trim();
    if (line.isEmpty()) return;
    const int sp = line.indexOf(' ');
    const String cmd = (sp < 0) ? line : line.substring(0, sp);
    const String arg = (sp < 0) ? ""   : line.substring(sp + 1);

    if (cmd == "help")  { banner(); }
    else if (cmd == "tx") {
        const bool vt = transmit(g_data, g_holdMs);
        Serial.printf("  data %u, TE low %ums ... %s\n", g_data, g_holdMs,
                      vt ? "sent, HT12D decoded it" : "sent");
    }
    else if (cmd == "data") {
        const int v = arg.toInt();
        if (v < 0 || v > 15) { Serial.println(F("  0-15")); return; }
        g_data = (uint8_t)v;
        Serial.printf("  data = %u (AD11..AD8 = %u%u%u%u)\n",
                      g_data, (g_data>>3)&1, (g_data>>2)&1, (g_data>>1)&1, g_data&1);
    }
    else if (cmd == "hold") {
        const int v = arg.toInt();
        if (v < 10 || v > 5000) { Serial.println(F("  10-5000 ms")); return; }
        g_holdMs = (uint32_t)v;
        Serial.printf("  hold = %ums\n", g_holdMs);
    }
    // The four one-button candidates: exactly one AD pin pulled low. The fob has
    // one button, so the answer is one of these unless the board ORs two.
    else if (cmd == "scan") {
        drainInput();
        const uint8_t cand[4] = { 0b1110, 0b1101, 0b1011, 0b0111 };
        Serial.println(F("  watch the lamp — the pattern that switches it is the answer"));
        Serial.println(F("  the receiver TOGGLES, so watch for CHANGES, not for 'on'"));
        Serial.println(F("  (press any key to stop)"));
        for (int i = 0; i < 4; i++) {
            char label[8]; snprintf(label, sizeof(label), "AD%-2d ", 8 + i);
            tryPattern(label, cand[i]);
            delay(2000);
            if (aborted()) return;
        }
        Serial.println(F("  scan complete. Nothing switched? `sweep` tries all 16,"));
        Serial.println(F("  then re-check `osc` against `oscf` and the address DIP."));
    }
    else if (cmd == "sweep") { scan(0, 15); }
    // ── the encoder-less path ────────────────────────────────────────────
    else if (cmd == "rmt") {
        Serial.printf("  RMT: addr 0x%02X data %u tick %luus inv %d x%u ... ",
                      g_addr, g_data, (unsigned long)g_tickUs, (int)g_inv, g_repeats);
        rmtSend();
        Serial.println(F("sent"));
    }
    else if (cmd == "tick") {
        const int v = arg.toInt();
        if (v < 50 || v > 1000) { Serial.println(F("  50-1000 us")); return; }
        g_tickUs = (uint32_t)v;
        Serial.printf("  tick = %luus\n", (unsigned long)g_tickUs);
    }
    else if (cmd == "inv") {
        g_inv = (arg.toInt() != 0);
        Serial.printf("  inverted = %d\n", (int)g_inv);
    }
    else if (cmd == "addr") {
        const int v = arg.toInt();
        if (v < 0 || v > 255) { Serial.println(F("  0-255")); return; }
        g_addr = (uint8_t)v;
        Serial.printf("  addr = 0x%02X — A7..A0 = ", g_addr);
        for (int i = 7; i >= 0; i--) Serial.print((g_addr >> i) & 1);
        Serial.println();
    }
    else if (cmd == "reps") {
        const int v = arg.toInt();
        if (v < 1 || v > 200) { Serial.println(F("  1-200")); return; }
        g_repeats = (uint16_t)v;
        Serial.printf("  repeats = %u\n", g_repeats);
    }
    // The tick is the one number RCSwitch's protocol 11 gives us that our own
    // chip contradicts (~3.5 kHz measured against Holtek's nominal 3.0), so sweep
    // it rather than trust it. This also measures how WIDE the receiver's window
    // is, which no single working value would tell you.
    else if (cmd == "rsweep") {
        // Range is settable because the first run found its answer at 180, the
        // very bottom of the original 180-360 span — which is an EDGE, not a
        // result. A working value at the end of a sweep means the window
        // probably continues past it and you have no idea how far.
        uint32_t from = 100, to = 400, step = 10;
        if (arg.length()) sscanf(arg.c_str(), "%lu %lu %lu",
                                 (unsigned long*)&from, (unsigned long*)&to,
                                 (unsigned long*)&step);
        if (from < 40 || to > 1200 || from >= to || step < 1) {
            Serial.println(F("  rsweep [from] [to] [step]   (40-1200us)"));
            return;
        }
        const uint32_t saved = g_tickUs;
        drainInput();
        Serial.printf("  sweeping %lu-%luus step %lu — watch the lamp for CHANGES\n",
                      (unsigned long)from, (unsigned long)to, (unsigned long)step);
        Serial.println(F("  NOTE EVERY TICK THAT TOGGLES IT, not just the first."));
        Serial.println(F("  You want the MIDDLE of the working range, not an edge."));
        Serial.println(F("  (press any key to stop)"));
        for (uint32_t t = from; t <= to; t += step) {
            g_tickUs = t;
            Serial.printf("  tick %4luus ... ", (unsigned long)t);
            rmtSend();
            Serial.println(F("sent"));
            delay(2000);
            if (aborted()) { g_tickUs = saved; return; }
        }
        g_tickUs = saved;
        Serial.printf("  rsweep complete, tick restored to %luus.\n",
                      (unsigned long)saved);
        Serial.println(F("  If the working values run to either END of the sweep,"));
        Serial.println(F("  widen it — you found a boundary, not a window."));
    }
    else if (cmd == "osc")   { measureOsc(true); }
    else if (cmd == "oscf")  {
        Serial.println(F("  clip D0 to the FOB's OSC2 (pin 15) and hold its button now..."));
        delay(2000);
        measureOsc(false);
    }
    else if (cmd == "vt") {
        Serial.println(digitalRead(PIN_VT) == HIGH
            ? F("  VT is HIGH right now — the HT12D is holding a valid decode")
            : F("  VT low. Only meaningful during a burst; `tx` reports it inline."));
    }
    else Serial.println(F("  ? try help"));
}

void setup() {
    Serial.begin(115200);
    delay(400);
    // Everything to its RESTING state before anything else. An HT12E whose TE is
    // driven low by a booting GPIO transmits, and this program's one hard rule is
    // that it does not do that.
    release(PIN_TE);
    applyData(0b1111);
    pinMode(PIN_OSC, INPUT);
    pinMode(PIN_VT,  INPUT);        // no pull-up: floats low with no HT12D wired
    banner();
}

void loop() {
    static String line;
    while (Serial.available()) {
        const char c = (char)Serial.read();
        if (c == '\n' || c == '\r') { if (line.length()) { handle(line); line = ""; } }
        else if (line.length() < 64) line += c;
    }
    delay(5);
}

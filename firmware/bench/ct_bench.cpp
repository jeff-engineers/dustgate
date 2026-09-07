// =============================================================================
// ct_bench.cpp — a walk-around current meter for a split-core CT.
//
// THE QUESTION THIS EXISTS TO ANSWER: can a 30 A clamp tell a running tool from
// an idle one, well enough to drive DustGate's routing? Not "what does the
// jointer draw" — what is the NOISE FLOOR, and how far above it does a real tool
// sit. Everything here is arranged around that comparison.
//
// It runs with no laptop attached. Board + OLED + a USB power bank, clamp the
// lead, read the glass. Serial says the same things when it is plugged in.
//
// ⚠️ A CT MUST GO AROUND ONE CONDUCTOR. Clamped around a whole appliance cord,
// hot and neutral cancel and it reads ~zero. Use a line splitter's 1X loop, or
// clamp inside the tool's own wiring compartment. (Whether an intact cord leaks
// enough field to detect on/off anyway is an open question — shop-schema-rfc.md
// §5.4 — and this meter is how you would answer it: read the splitter's 1X loop,
// then the intact cord, back to back.)
//
// SENSOR: SCT-013-030, 30 A → 1 V RMS, burden resistor built in. Cut the 3.5 mm
// plug off and use the bare leads. Wiring: firmware/wiring/ct-bench.md.
//
// Build and flash:
//     PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e xiao_c5_ct_bench -t upload
//     PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio device monitor -e xiao_c5_ct_bench
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include "../config.h"

static const int  PIN_CT    = 1;      // D0, the only analog pad on the edge
static const int  OLED_ADDR = 0x3C;

// SCT-013-030: 30 A RMS through the jaw gives 1 V RMS out. Linear, so amps are
// just volts × 30. No calibration constant, because there is nothing here that
// a calibration would improve — see the note on resolution below.
static const float AMPS_PER_VOLT = 30.0f;

// Nominal, NOT measured. This board has no idea what the mains voltage is, and
// 120 V is only here so the numbers can be compared against DEFAULT_THRESHOLD_W
// (shared/device-model), which is in watts. Treat every watt figure as ±5%
// before you treat it as anything else.
static const float NOMINAL_VOLTS = 120.0f;

static Adafruit_SSD1306 oled(128, 64, &Wire, -1);
static bool  g_haveOled = false;
static bool  g_screenOn = true;

// ⚠️ THE SCREEN IS A NOISE SOURCE, and it is not subtle. Measured 2026-09-06:
// the reading fell from ~0.4 A to ~0 the moment the panel was unplugged.
//
// SSD1306_SWITCHCAPVCC means the panel makes its own ~7-9 V from 3.3 V with a
// switching charge pump, on the same rail as the ADC's bias divider — and a
// 10k/10k divider presents 5 kOhm to the pin, which is high enough to pick that
// up handsomely. DISPLAYOFF stops the charge pump, not just the pixels, which is
// why this toggle is worth having rather than reaching for the connector.
//
// THIS IS A PRODUCT PROBLEM, not a bench one. Every DustGate env assumes a
// screen, and a CT input on a real board sits on D0 beside that same panel. The
// fix is a lower-impedance divider (1k/1k) and a 100 nF ceramic at the pin, and
// it has to be validated with the screen ON — see docs/shop-schema-rfc.md §7.5.
// Track the REQUESTED state whether or not a panel answered. The first version
// returned early with no panel, leaving g_screenOn stuck at its `true` default —
// so `screen 0` reported "on", and worse, `zero` kept warning that the screen was
// costing 0.4 A of noise while the panel sat unplugged on the bench. A debugging
// aid that lies about the thing being debugged is worse than not having it.
static void screenPower(bool on) {
    g_screenOn = on;
    if (!g_haveOled) return;
    oled.ssd1306_command(on ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
}

static float g_amps   = 0;    // last reading
// Max since the last clear. TREAT WITH SUSPICION: it latches noise as readily as
// signal, and it did — a 4.368 A "peak" recorded during the screen-noise period
// got quoted back as a real tool measurement on 2026-09-06 and a whole margin
// argument was built on it. It is a convenience for walking up to a tool and
// flipping its switch, not evidence.
static float g_hold   = 0;
static float g_floor  = 0;    // measured noise floor (`zero`)
static float g_rateKs = 0;    // achieved sample rate, kSPS
// The DC operating point, printed because its ABSENCE cost a debugging session.
// A railed input — 0 V or 3V3 instead of mid-rail — returns the same count every
// sample, so the variance is exactly zero and the meter reports a beautiful
// 0.000 A floor while measuring nothing at all. Mid-rail on a 3V3 divider is
// ~1650 mV; anything near either rail means the bias network is not working and
// every reading above is fiction.
static uint32_t g_dcMv = 0;
static float    g_dcCounts = 0;

// -- One measurement ----------------------------------------------------------
//
// Sample flat out for a whole number of line cycles, subtract the mean, take the
// RMS of what is left. Subtracting the MEASURED mean is what makes the bias
// network's exact midpoint irrelevant — a lazy divider and a drifting reference
// both come out in the wash, which is why there is no trim here.
//
// 200 ms is 12 cycles at 60 Hz. Long enough that a half-cycle error is noise,
// short enough to feel live on the screen.
// RAW COUNTS, NOT MILLIVOLTS — and that is a correction, not a preference.
//
// This used analogReadMilliVolts(), which returns WHOLE MILLIVOLTS. With the
// screen off the input is quieter than that, so every sample came back the same
// integer, the variance was exactly zero, and the meter printed 0.000 A for
// minutes on end (2026-09-06). That is not a measurement, it is a floor made of
// rounding: it hid everything below 1 mV RMS, which is 0.03 A on this CT.
//
// analogRead() is ~0.61 mV/LSB at 11 dB — 1.6x finer — and faster, so more
// samples per window as well. The mV-per-count scale is derived once per window
// from a single analogReadMilliVolts() of the same input, which keeps the
// per-chip ADC calibration without paying for it 5000 times.
static float readAmps() {
    static const uint32_t windowMs = 200;
    const uint32_t t0 = millis();
    uint32_t n = 0;
    double sum = 0, sumSq = 0;

    // Two passes would be tidier but the signal moves; accumulate both moments
    // in one pass and do the algebra afterwards.
    while (millis() - t0 < windowMs) {
        const uint32_t c = analogRead(PIN_CT);
        sum   += c;
        sumSq += (double)c * (double)c;
        n++;
    }
    if (n < 100) return 0;
    const uint32_t took = millis() - t0;
    g_rateKs = (float)n / (float)took;                 // samples/ms == kSPS

    const double mean = sum / n;
    const double var  = (sumSq / n) - (mean * mean);   // RMS of the AC part
    const double rmsCounts = (var > 0 ? sqrt(var) : 0);

    // counts -> volts, using the chip's own calibration at the operating point.
    const uint32_t meanMv = analogReadMilliVolts(PIN_CT);
    g_dcMv = meanMv; g_dcCounts = (float)mean;
    const double mvPerCount = (mean > 1) ? ((double)meanMv / mean) : 0.61;
    return (float)(rmsCounts * mvPerCount / 1000.0 * AMPS_PER_VOLT);
}

// -- Output -------------------------------------------------------------------
static void drawScreen() {
    if (!g_haveOled || !g_screenOn) return;
    oled.clearDisplay();

    oled.setTextSize(3);
    oled.setCursor(0, 0);
    oled.print(g_amps, g_amps < 10 ? 2 : 1);
    oled.setTextSize(1);
    oled.print(" A");

    oled.setTextSize(2);
    oled.setCursor(0, 26);
    oled.print((int)(g_amps * NOMINAL_VOLTS));
    oled.setTextSize(1);
    oled.print(" W");

    oled.setCursor(0, 46);
    oled.printf("peak %.2fA", g_hold);
    oled.setCursor(0, 56);
    // The floor is the whole point: anything not comfortably above it is not
    // detectable, however good the number above looks.
    oled.printf("floor %.2fA %dW", g_floor, (int)(g_floor * NOMINAL_VOLTS));

    oled.display();
}

static void report() {
    Serial.printf("%7.3f A  %6d W   peak %6.3f A   floor %5.3f A   "
                  "DC %4lumV/%.0f   %.1f kSPS%s\n",
                  g_amps, (int)(g_amps * NOMINAL_VOLTS), g_hold, g_floor,
                  (unsigned long)g_dcMv, g_dcCounts, g_rateKs,
                  (g_dcMv < 300 || g_dcMv > 3000) ? "  <-- RAILED" : "");
}

// -- Phases, and a suite built out of them ------------------------------------
//
// `q` was not enough, and the gap was obvious in hindsight: it silences the
// serial stream, and the serial stream is how you SEE an excursion. Watching
// with the screen fitted is no better, because the screen is the loudest noise
// source on the board.
//
// So a phase samples for N seconds and says nothing until it is done, then
// reports WHEN each excursion happened and the gaps between them. "Every ~2
// seconds" is a claim about intervals, and nothing else here measures intervals.
struct Phase {
    uint32_t windows;
    float    mn, mean, mx;
    int      marks;
    float    meanGapS;
};

static Phase runPhase(uint32_t secs, bool chatty) {
    const float trip = (g_floor > 0) ? g_floor * 3.0f : 0.05f;
    uint32_t markMs[96]; float markA[96]; int nMarks = 0;
    float mn = 1e9f, mx = 0; double acc = 0; uint32_t n = 0;

    delay(50);                                   // let the prompt drain
    while (Serial.available()) Serial.read();

    const uint32_t t0 = millis();
    while ((millis() - t0) < secs * 1000UL) {
        const float a = readAmps();
        n++; acc += a;
        if (a < mn) mn = a;
        if (a > mx) mx = a;
        if (a > trip && nMarks < 96) { markMs[nMarks] = millis() - t0;
                                       markA[nMarks] = a; nMarks++; }
        // CHATTY IS THE POINT of the first phase, not a convenience: it puts USB
        // traffic on the bus during sampling so the silent phase has something
        // to be compared against.
        if (chatty) Serial.printf("    %.3f A\n", a);
        if (Serial.available()) { while (Serial.available()) Serial.read(); break; }
    }

    Phase p;
    p.windows = n;
    p.mn   = (mn > 1e8f) ? 0 : mn;
    p.mean = n ? (float)(acc / n) : 0;
    p.mx   = mx;
    p.marks = nMarks;
    p.meanGapS = 0;
    if (nMarks > 1)
        p.meanGapS = ((markMs[nMarks - 1] - markMs[0]) / (float)(nMarks - 1)) / 1000.0f;

    Serial.printf("    %lu windows  min %.3f  mean %.3f  max %.3f A\n",
                  (unsigned long)p.windows, p.mn, p.mean, p.mx);
    if (!nMarks) {
        Serial.printf("    no excursions above %.3f A\n", trip);
    } else {
        Serial.printf("    %d excursions above %.3f A", nMarks, trip);
        if (p.meanGapS > 0) Serial.printf(", mean gap %.2fs", p.meanGapS);
        Serial.println();
        for (int i = 0; i < nMarks && i < 12; i++) {
            Serial.printf("      %6.2fs  %.3f A", markMs[i] / 1000.0f, markA[i]);
            if (i) Serial.printf("   (+%.2fs)", (markMs[i] - markMs[i-1]) / 1000.0f);
            Serial.println();
        }
        if (nMarks > 12) Serial.printf("      ... %d more\n", nMarks - 12);
    }
    return p;
}

static void waitForEnter(const char* what) {
    while (Serial.available()) Serial.read();
    Serial.printf("\n  >>> %s, then press Enter.\n", what);
    while (!Serial.available()) delay(20);
    while (Serial.available()) Serial.read();
}

// The whole diagnostic, in the order that eliminates the most per step.
static void suite(uint32_t secs) {
    Serial.printf("\n=== CT bench suite, %lus per phase ===\n", (unsigned long)secs);
    if (g_floor <= 0) {
        Serial.println(F("  No floor yet — measuring one first, clamp as it is."));
        float a = 0; for (int i = 0; i < 8; i++) a += readAmps();
        g_floor = a / 8;
        Serial.printf("  floor %.3f A\n", g_floor);
    }

    Serial.println(F("\n[A] baseline, SERIAL CHATTERING during sampling"));
    Phase a = runPhase(secs, true);

    Serial.println(F("\n[B] baseline, SILENT"));
    Phase b = runPhase(secs, false);

    waitForEnter("UNPLUG THE CT and short the ADC pin to the bias midpoint");
    Serial.println(F("\n[C] no sensor at all, silent"));
    Phase c = runPhase(secs, false);

    waitForEnter("RECONNECT THE CT, clamp around a de-energised conductor");
    Serial.println(F("\n[D] sensor back, silent"));
    Phase d = runPhase(secs, false);

    Serial.println(F("\n=== verdict ==="));
    Serial.printf("  A chatty   %2d excursions, max %.3f A\n", a.marks, a.mx);
    Serial.printf("  B silent   %2d excursions, max %.3f A\n", b.marks, b.mx);
    Serial.printf("  C no CT    %2d excursions, max %.3f A\n", c.marks, c.mx);
    Serial.printf("  D CT back  %2d excursions, max %.3f A\n", d.marks, d.mx);
    Serial.println();

    if (a.marks > b.marks * 2 + 1)
        Serial.println(F("  * USB TRAFFIC is a noise source here — A is much worse"));
    else
        Serial.println(F("  * serial traffic is NOT the cause; A and B agree"));

    if (c.marks > (b.marks + d.marks) / 2 / 2)
        Serial.println(F("  * it happens with NO SENSOR ATTACHED, so it is the board"));
    else if (d.marks > c.marks * 2 + 1)
        Serial.println(F("  * it arrives through the CT LEADS — C was clean, D is not"));
    else
        Serial.println(F("  * inconclusive: too few excursions to attribute"));

    if (b.meanGapS > 0)
        Serial.printf("  * silent gaps average %.2fs — regular means CLOCKED, "
                      "ragged means environmental\n", b.meanGapS);

    // [E] and [F] are the only phases that answer DESIGN questions rather than
    // debugging ones, so they go last.
    //
    // E is the whole-cord case, and it is not a warm-up — it is the open question
    // from shop-schema-rfc.md §5.4. Hot and neutral cancel in an intact cord, so
    // in theory it reads nothing; iVAC ship a product that says otherwise, and
    // if they are right then every 240V and hardwired install stops needing
    // anything opened up. It does not have to be accurate. It has to be
    // repeatable and distinguishable from off.
    waitForEnter("Clamp the WHOLE CORD (both conductors) and switch the LOAD ON");
    Serial.println(F("\n[E] load on, clamp around BOTH conductors"));
    Phase e = runPhase(secs, false);

    waitForEnter("Separate the zip cord and clamp ONE conductor, load still ON");
    Serial.println(F("\n[F] load on, clamp around ONE conductor"));
    Phase f = runPhase(secs, false);

    Serial.println(F("\n=== the design questions ==="));
    Serial.printf("  floor        %.3f A  (%.1f W at %dV nominal)\n",
                  g_floor, g_floor * NOMINAL_VOLTS, (int)NOMINAL_VOLTS);
    Serial.printf("  whole cord   %.3f A mean, %.3f A max\n", e.mean, e.mx);
    Serial.printf("  one conductor %.3f A mean, %.3f A max\n", f.mean, f.mx);

    if (g_floor > 0 && f.mean > g_floor)
        Serial.printf("  one-conductor margin %.1fx over the floor\n", f.mean / g_floor);

    // The answer that changes the install story.
    if (g_floor > 0 && e.mean > g_floor * 3) {
        Serial.printf("\n  * THE INTACT CORD READS %.1fx THE FLOOR. If that repeats,\n",
                      e.mean / g_floor);
        Serial.println(F("    240V and hardwired tools stop needing anything opened"));
        Serial.println(F("    up — clip on and go. Re-run it a few times before"));
        Serial.println(F("    believing it; repeatability is the whole claim."));
    } else {
        Serial.println(F("\n  * the intact cord reads at or near the floor — the"));
        Serial.println(F("    fields cancel as theory says, and every install needs"));
        Serial.println(F("    one conductor separated. shop-schema-rfc.md §5.4."));
    }
    if (f.mean > 0 && e.mean > 0)
        Serial.printf("  * whole cord is %.0f%% of one conductor\n",
                      100.0f * e.mean / f.mean);
    Serial.println(F("  A CT MEASURES CURRENT, NOT POWER. A small motor's power"));
    Serial.println(F("  factor is poor, so amps x 120 will read HIGHER than a"));
    Serial.println(F("  wattmeter's real power on the same load. thresholdW in the"));
    Serial.println(F("  model is watts — that gap is a modelling problem, not a"));
    Serial.println(F("  calibration one."));
}

// -- Console ------------------------------------------------------------------
static void banner() {
    Serial.println(F("\nCT bench — SCT-013-030 on D0"));
    Serial.println(F("  zero      measure the NOISE FLOOR. Do this with the clamp"));
    Serial.println(F("            around a DE-ENERGISED conductor, not in mid-air —"));
    Serial.println(F("            a dangling clamp picks up less than a clamped one"));
    Serial.println(F("  clear     reset the peak hold"));
    Serial.println(F("  log <n>   n one-second samples as CSV, for pasting"));
    Serial.println(F("  screen <0|1>  the panel's charge pump is a NOISE SOURCE."));
  Serial.println(F("            Turn it off to measure, on to read. `zero` with"));
  Serial.println(F("            it off is the sensor's real floor"));
  Serial.println(F("  suite [s]   THE WHOLE DIAGNOSTIC. Four phases, prompts you"));
  Serial.println(F("            for the two it cannot do itself, prints a verdict"));
  Serial.println(F("  silent <s>  sample with NO serial output, then report the"));
  Serial.println(F("            excursions and the GAPS between them. This is how"));
  Serial.println(F("            you test whether USB traffic is the noise source,"));
  Serial.println(F("            since `q` would also hide the answer"));
  Serial.println(F("  q         quiet: no serial spam (for walking with the screen)"));
    Serial.println(F("\nCLAMP ONE CONDUCTOR. A whole cord reads ~zero: the fields cancel.\n"));
}

static bool g_quiet = false;

static void handle(String line) {
    line.trim();
    if (line.isEmpty()) return;
    const int sp = line.indexOf(' ');
    const String cmd = (sp < 0) ? line : line.substring(0, sp);
    const String arg = (sp < 0) ? ""   : line.substring(sp + 1);

    if (cmd == "help") banner();
    else if (cmd == "zero") {
        // Average several windows: the floor is what we are about to judge every
        // other reading against, so it is worth more than 200 ms of attention.
        float acc = 0;
        for (int i = 0; i < 8; i++) acc += readAmps();
        g_floor = acc / 8;
        Serial.printf("  floor %.3f A  (%.1f W at %dV nominal)\n",
                      g_floor, g_floor * NOMINAL_VOLTS, (int)NOMINAL_VOLTS);
        Serial.printf("  DC operating point %lumV (%.0f counts)\n",
                      (unsigned long)g_dcMv, g_dcCounts);
        if (g_dcMv < 300 || g_dcMv > 3000) {
            Serial.println(F("  ⚠️  THAT IS RAILED, NOT BIASED. Expect ~1650mV on a"));
            Serial.println(F("  3V3 divider. A stuck input returns the same count"));
            Serial.println(F("  every sample, so the variance is zero and this"));
            Serial.println(F("  'floor' is measuring nothing. Check the 10k pair and"));
            Serial.println(F("  that the CT bridges the midpoint and D0."));
        } else if (g_floor == 0.0f) {
            Serial.println(F("  Bias looks right, and the floor is BELOW ONE LSB —"));
            Serial.println(F("  quieter than this ADC can resolve. That is a real"));
            Serial.println(F("  result, not a broken one."));
        }
        // The comparison that actually answers the design question.
        //
        // Say what is actually true. The first version of this printed "BELOW
        // the floor" whenever the threshold was inside 3x the floor, which is a
        // different claim and was wrong the first time it mattered: 0.042 A
        // against a 0.020 A floor is ABOVE it, just without much room.
        const float thrA = 5.0f / NOMINAL_VOLTS;
        // Guarded: a railed input gives a zero floor, and dividing by it printed
        // "infx the floor. Comfortable." — the most confident possible way to say
        // nothing. Refuse to draw a conclusion from a broken measurement.
        if (g_dcMv < 300 || g_dcMv > 3000) {
            Serial.println(F("  No verdict — the input is railed, so there is no"));
            Serial.println(F("  floor to compare against. Fix the bias first."));
            return;
        }
        Serial.printf("  DEFAULT_THRESHOLD_W is 5 W = %.3f A — ", thrA);
        if (g_floor <= 0)
            Serial.println(F("floor is below one LSB, so the margin is at least "
                             "the ADC's resolution. Good, but unquantified."));
        else if (thrA < g_floor)
            Serial.println(F("BELOW the floor. Not detectable on this CT at all: "
                             "raise the threshold, or use a smaller-range CT for "
                             "small tools."));
        else if (thrA < g_floor * 3)
            Serial.printf("above the floor, but only %.1fx it. Usable in principle, "
                          "marginal in practice — a shop with motors running is "
                          "noisier than this bench.\n", thrA / g_floor);
        else
            Serial.printf("%.0fx the floor. Comfortable.\n", thrA / g_floor);

        // Only when a panel actually answered AND it is lit — see screenPower().
        if (g_haveOled && g_screenOn)
            Serial.println(F("  ...and the SCREEN IS ON, which is worth ~0.4 A of "
                             "noise. `screen 0` and measure again."));
    }
    else if (cmd == "clear") { g_hold = 0; Serial.println(F("  peak cleared")); }
    else if (cmd == "silent") {
        int n = arg.toInt(); if (n < 1 || n > 600) n = 30;
        runPhase((uint32_t)n, false);
    }
    else if (cmd == "suite") {
        int n = arg.toInt(); if (n < 5 || n > 120) n = 20;
        suite((uint32_t)n);
    }
    else if (cmd == "screen") {
        screenPower(arg.toInt() != 0);
        if (!g_haveOled) {
            Serial.println(F("  no panel detected at boot — nothing to switch."));
            Serial.println(F("  (if one is plugged in now, reset to probe again)"));
        } else {
            Serial.printf("  screen %s%s\n", g_screenOn ? "on" : "off",
                          g_screenOn ? " — expect the floor to rise" : "");
        }
    }
    else if (cmd == "q")     { g_quiet = !g_quiet;
                               Serial.println(g_quiet ? F("  quiet") : F("  talking")); }
    else if (cmd == "log") {
        int n = arg.toInt(); if (n < 1 || n > 600) n = 30;
        Serial.println(F("  sec,amps,watts"));
        for (int i = 0; i < n; i++) {
            float a = 0;
            for (int k = 0; k < 5; k++) a += readAmps();   // ~1 s
            a /= 5;
            Serial.printf("  %d,%.3f,%d\n", i, a, (int)(a * NOMINAL_VOLTS));
            if (Serial.available()) { while (Serial.available()) Serial.read();
                                      Serial.println(F("  stopped.")); return; }
        }
    }
    else Serial.println(F("  ? try help"));
}

void setup() {
    Serial.begin(115200);
    delay(400);

    analogSetAttenuation(ADC_11db);          // full ~0-2.5V span; the CT swings
    analogReadResolution(12);                // around a mid-rail bias

    // Same probe StatusScreen.h uses: a board with the pins but no panel is the
    // ordinary case, not a mistake. Serial still works either way.
    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
    Wire.beginTransmission(OLED_ADDR);
    if (Wire.endTransmission() == 0 && oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        g_haveOled = true;
        oled.setTextColor(SSD1306_WHITE);
        oled.clearDisplay(); oled.display();
    }
    banner();
    // State it at boot. Which of these two lines you get decides how to read
    // every `screen` and `zero` message afterwards.
    if (g_haveOled) {
        Serial.println(F("  OLED found — walk with it. It is also a NOISE SOURCE:"));
        Serial.println(F("  `screen 0` before `zero`, or the floor is the panel's."));
    } else {
        g_screenOn = false;              // nothing to be on
        Serial.println(F("  No OLED detected. Serial only, and the analog input"));
        Serial.println(F("  is as quiet as this board gets."));
    }
}

void loop() {
    static String line;
    while (Serial.available()) {
        const char c = (char)Serial.read();
        if (c == '\n' || c == '\r') { if (line.length()) { handle(line); line = ""; } }
        else if (line.length() < 64) line += c;
    }

    g_amps = readAmps();
    if (g_amps > g_hold) g_hold = g_amps;
    drawScreen();

    static uint32_t nextReport = 0;
    if (!g_quiet && millis() > nextReport) { report(); nextReport = millis() + 1000; }
}

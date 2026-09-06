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

static float g_amps   = 0;    // last reading
static float g_hold   = 0;    // max since the last clear
static float g_floor  = 0;    // measured noise floor (`zero`)
static float g_rateKs = 0;    // achieved sample rate, kSPS

// -- One measurement ----------------------------------------------------------
//
// Sample flat out for a whole number of line cycles, subtract the mean, take the
// RMS of what is left. Subtracting the MEASURED mean is what makes the bias
// network's exact midpoint irrelevant — a lazy divider and a drifting reference
// both come out in the wash, which is why there is no trim here.
//
// 200 ms is 12 cycles at 60 Hz. Long enough that a half-cycle error is noise,
// short enough to feel live on the screen.
static float readAmps() {
    static const uint32_t windowMs = 200;
    const uint32_t t0 = millis();
    uint32_t n = 0;
    double sum = 0, sumSq = 0;

    // Two passes would be tidier but the signal moves; accumulate both moments
    // in one pass and do the algebra afterwards.
    while (millis() - t0 < windowMs) {
        const uint32_t mv = analogReadMilliVolts(PIN_CT);
        sum   += mv;
        sumSq += (double)mv * (double)mv;
        n++;
    }
    if (n < 100) return 0;
    const uint32_t took = millis() - t0;
    g_rateKs = (float)n / (float)took;                 // samples/ms == kSPS

    const double mean = sum / n;
    const double var  = (sumSq / n) - (mean * mean);   // RMS of the AC part
    const double rmsV = (var > 0 ? sqrt(var) : 0) / 1000.0;
    return (float)(rmsV * AMPS_PER_VOLT);
}

// -- Output -------------------------------------------------------------------
static void drawScreen() {
    if (!g_haveOled) return;
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
    Serial.printf("%7.3f A  %6d W   peak %6.3f A   floor %5.3f A (%d W)   %.1f kSPS\n",
                  g_amps, (int)(g_amps * NOMINAL_VOLTS), g_hold,
                  g_floor, (int)(g_floor * NOMINAL_VOLTS), g_rateKs);
}

// -- Console ------------------------------------------------------------------
static void banner() {
    Serial.println(F("\nCT bench — SCT-013-030 on D0"));
    Serial.println(F("  zero      measure the NOISE FLOOR. Do this with the clamp"));
    Serial.println(F("            around a DE-ENERGISED conductor, not in mid-air —"));
    Serial.println(F("            a dangling clamp picks up less than a clamped one"));
    Serial.println(F("  clear     reset the peak hold"));
    Serial.println(F("  log <n>   n one-second samples as CSV, for pasting"));
    Serial.println(F("  q         quiet: screen only, no serial spam"));
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
        // The comparison that actually answers the design question.
        Serial.printf("  DEFAULT_THRESHOLD_W is 5 W = %.3f A — %s\n",
                      5.0f / NOMINAL_VOLTS,
                      (5.0f / NOMINAL_VOLTS > g_floor * 3)
                          ? "above the floor, usable"
                          : "BELOW the floor. A 30A clamp cannot see it; a tool "
                            "threshold on this sensor has to be much higher, or "
                            "a smaller-range CT is needed for small tools");
    }
    else if (cmd == "clear") { g_hold = 0; Serial.println(F("  peak cleared")); }
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
    Serial.println(g_haveOled ? F("  OLED found — walk with it.")
                              : F("  No OLED. Serial only."));
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

#pragma once
// =============================================================================
// StatusScreenModel.h — what the optional OLED status screen SAYS.
//
// This header is the layout half of the status screen: given a set of facts about
// the board, it produces the exact characters of an 8-row screen. It was written
// before any hardware existed, because it is the half that can be tested without
// a panel — nothing here touches I²C, a driver library, or a pin. A screen has
// since been driven for real (2026-08-21, DevKitC), which changes nothing about
// this file and is the point.
//
// PURE. No Arduino.h, no globals, no I/O — same rule as FaultPolicy.h, for the
// same reason: the sketch cannot be tested and this can. test_statusscreen.cpp
// asserts every line against the 21-column budget, which is the only question a
// 128×64 panel really asks.
//
// THE BUDGET, and where it comes from: Adafruit_GFX's built-in font is 5×7 in a
// 6×8 cell, so a 128×64 panel at size 1 is 21 columns × 8 rows. A size-2 glyph
// costs two columns AND two rows. The header band is two rows tall so the signal
// glyph gets a 3×2-cell box — at 6×8 it would be ~1.5mm on a 0.96" panel, which
// is decoration rather than information. That leaves:
//
//     rows 0-1   header band (inverse): hostname + signal glyph
//     rows 2-3   the state word, size 2 — 10 characters, hence "NODE DARK"
//     rows 4-7   four detail lines at size 1, 21 characters each
//
// Those layouts, and the reasoning behind each screen, are in
// docs/mockups/oled-status.html. This file is that page made executable; when
// they disagree, one of them is wrong and it is worth finding out which.
//
// IT DOES NOT INVENT A STATE VOCABULARY. The state word is statusled::Status
// (StatusVocab.h), spelled out — the same enum the pixel colours. Motion
// outranks Status here exactly as it does on the pixel, because during a move
// the move is the news.
//
// Truncation, not wrapping: a name too long for its line is cut, never folded
// onto the next one. Eight rows is a fixed budget and a wrapped hostname would
// silently push a detail line off the bottom.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>   // atoi, for the build stamp

#include "StatusVocab.h"

namespace statusscreen {

// ---------------------------------------------------------------------------
// The geometry. These are the panel's, not a preference — see the header note.
// docs/mockups/oled-status.html draws its screens on exactly this grid.
// ---------------------------------------------------------------------------
static const int kCols     = 21;   // characters per row at size 1
static const int kRows     = 8;    // rows at size 1
static const int kBarRows  = 2;    // the header band
static const int kMaxLines = 6;    // headline (2 rows) + 4 details, worst case

enum class Role { PRIMARY, NODE };

// ---------------------------------------------------------------------------
// Everything the screen is allowed to know. Filled at the call site from live
// state; the model reads it and nothing else, so a test can hand it a shop that
// doesn't exist.
//
// Every pointer may be null — an absent fact prints as an absent line rather
// than as "(null)". Counts of -1 mean "not known yet" and suppress their line
// for the same reason.
// ---------------------------------------------------------------------------
struct Facts {
    Role role = Role::PRIMARY;

    statusled::Status status = statusled::BOOTING;
    statusled::Motion motion = statusled::STILL;

    const char* hostname = nullptr;
    int  wifiBars = -1;          // 0-4 from RSSI; -1 draws no glyph at all

    // -- primary ------------------------------------------------------------
    int  gatesReady  = -1;       // gates reachable / gates configured
    int  gatesTotal  = -1;
    int  nodesLinked = -1;
    int  nodesTotal  = -1;
    bool collectorOn = false;
    const char* toolName = nullptr;   // the tool drawing power, by ITS name —
                                      // never a port id; that is what the
                                      // woodworker recognises
    // Gate LABELS, not numbers: a real topology names its selectors ("Manifold
    // A"), and a number the woodworker never sees written on anything is worse
    // than the name they do. Short ones only — the line is 21 columns and the
    // state on the right is the news.
    const char* openGate    = nullptr;   // the gate that tool routes to
    const char* openingGate = nullptr;   // make-before-break puts two gates in
    const char* closingGate = nullptr;   // flight, so a move screen names both

    // -- node ---------------------------------------------------------------
    const char* primaryHost = nullptr;   // the brain it is linked to
    int  servoCount  = -1;
    int  lastCmdSec  = -1;

    // -- faults ---------------------------------------------------------------
    const char* darkNode  = nullptr;  // a board that stopped answering
    int  darkForSec = -1;
    const char* ssid      = nullptr;  // the network being looked for. Half of
                                      // all WiFi faults are the wrong SSID, so
                                      // the screen says which one out loud.
    int  retrySec   = -1;

    // -- portal ---------------------------------------------------------------
    const char* apName    = nullptr;  // the AP a stranger has to join
    const char* portalIp  = nullptr;

    // -- provenance -----------------------------------------------------------
    // When this firmware was BUILT (not flashed). Printed on the bottom row and
    // only where a row is actually spare, so it can never displace a fact — see
    // render(). "Which build is on this board?" is otherwise a question you
    // answer by reflashing to be sure, which is how you lose the state you were
    // trying to debug.
    const char* buildStamp = nullptr;
};

struct Line {
    char    text[kCols + 1] = {0};
    uint8_t size = 1;            // 1 or 2; a size-2 line consumes two rows
};

struct Screen {
    char bar[kCols + 1] = {0};   // header band text (inverse video)
    bool barBlink = false;       // the band blinks when something is wrong
    int  wifiBars = -1;          // -1 = draw no glyph

    Line lines[kMaxLines];
    int  lineCount = 0;

    /** Rows consumed, header band included. Must never exceed kRows. */
    int rows() const {
        int r = kBarRows;
        for (int i = 0; i < lineCount; i++) r += lines[i].size;
        return r;
    }
};

// ---------------------------------------------------------------------------
// Small formatting helpers. Every one of them clamps to kCols; that guarantee
// is what the fit test checks, and it is why they exist rather than sprintf at
// the call sites.
// ---------------------------------------------------------------------------

/** Copy at most kCols characters, always NUL-terminated. */
inline void _fit(char* dst, const char* src) {
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n > (size_t)kCols) n = (size_t)kCols;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/**
 * label left, value hard against the right edge, spaces between — the shape
 * every detail line on the mockup has ("gates      4/4"). The LABEL is what
 * gets truncated when they collide, because the value is the news.
 */
inline void _pair(char* dst, const char* label, const char* value) {
    char l[kCols + 1], v[kCols + 1];
    _fit(l, label);
    _fit(v, value);
    int lv = (int)strlen(v);
    int ll = (int)strlen(l);
    if (ll + 1 + lv > kCols) { ll = kCols - 1 - lv; if (ll < 0) ll = 0; l[ll] = '\0'; }
    int gap = kCols - ll - lv;
    memcpy(dst, l, (size_t)ll);
    for (int i = 0; i < gap; i++) dst[ll + i] = ' ';
    memcpy(dst + ll + gap, v, (size_t)lv);
    dst[kCols] = '\0';
}

inline void _pairNum(char* dst, const char* label, int value, const char* suffix = "") {
    char v[kCols + 1];
    snprintf(v, sizeof(v), "%d%s", value, suffix);
    _pair(dst, label, v);
}

inline void _pairFrac(char* dst, const char* label, int a, int b) {
    char v[kCols + 1];
    snprintf(v, sizeof(v), "%d/%d", a, b);
    _pair(dst, label, v);
}

/** Append a line, silently refusing one that would overrun the panel. */
inline void _add(Screen& s, const char* text, uint8_t size = 1) {
    if (s.lineCount >= kMaxLines) return;
    if (s.rows() + size > kRows) return;
    Line& l = s.lines[s.lineCount];
    l.size = size;
    if (size == 2) {
        // Half the columns at double width, so the headline is clipped to 10
        // rather than spilling off the right edge.
        char t[kCols + 1];
        _fit(t, text);
        if (strlen(t) > (size_t)(kCols / 2)) t[kCols / 2] = '\0';
        strcpy(l.text, t);
    } else {
        _fit(l.text, text);
    }
    s.lineCount++;
}

/**
 * Compiler __DATE__ ("Aug 20 2026") + __TIME__ ("07:25:00") into the short form
 * a person reads at a glance: "8/20/26 7:25:00". 15 characters at the longest,
 * which is what lets it share the 21-column bottom row.
 *
 * Pure and host-tested, because date parsing is exactly the kind of code that
 * looks obviously right and is off by one somewhere. Anything it cannot parse
 * comes back empty rather than half-formatted — a wrong date on a status screen
 * is worse than no date, since it sends you to the wrong binary.
 */
inline void formatBuild(const char* date, const char* time, char* out, size_t n) {
    if (!out || n == 0) return;
    out[0] = '\0';
    if (!date || !time || strlen(date) < 11 || strlen(time) < 8) return;

    static const char* kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int month = 0;
    for (int i = 0; i < 12; i++) {
        if (strncmp(date, kMonths + i * 3, 3) == 0) { month = i + 1; break; }
    }
    if (!month) return;

    // __DATE__ pads a single-digit day with a SPACE ("Aug  5 2026"), which is
    // the detail that breaks a naive atoi-at-a-fixed-offset.
    const int day  = atoi(date + 4);
    const int year = atoi(date + 7);
    if (day < 1 || day > 31 || year < 2000) return;

    const int hour = atoi(time);
    snprintf(out, n, "%d/%d/%02d %d:%c%c:%c%c",
             month, day, year % 100, hour,
             time[3], time[4], time[6], time[7]);
}

/**
 * The state word, size 2 — 10 characters at most, which is why these are the
 * spellings they are. Motion outranks Status, same rule as the pixel.
 */
inline const char* stateWord(const Facts& f) {
    switch (f.motion) {
        case statusled::MOVING:      return "MOVING";
        case statusled::HOMING:      return "HOMING";
        case statusled::CALIBRATING: return "CALIB";      // "CALIBRATING" is 11
        case statusled::STILL:       break;
    }
    switch (f.status) {
        case statusled::FAULT:   return f.darkNode ? "NODE DARK" : "FAULT";
        case statusled::BOOTING: return "STARTING";
        case statusled::PORTAL:  return "JOIN WIFI";
        case statusled::NO_WIFI: return "NO WIFI";
        case statusled::ONLINE:
            // A node on WiFi with no brain is UNLINKED. A primary shows blue for
            // two different reasons and they are not the same news: no topology
            // stored at all, or a paired board gone dark. Saying "NO SHOP" about
            // a shop that is laid out and running would send someone to the
            // wrong problem.
            if (f.role == Role::NODE) return "UNLINKED";
            return f.darkNode ? "NODE DARK" : "NO SHOP";
        case statusled::READY:
            // A running tool is the thing worth reading from the doorway.
            if (f.role == Role::NODE) return "LINKED";
            return f.toolName ? "RUNNING" : "READY";
    }
    return "";
}

/**
 * True when the board is in a state a human has to do something about — which
 * is what makes the header band blink, and what holds the screen awake.
 *
 * A dark node is on this list even though the PIXEL shows it blue rather than
 * red. That is not the two indicators disagreeing: blue is the honest colour
 * (nothing is broken here — part of the shop is unreachable), and blinking is
 * how a screen says "and this is the part you walked over to read". The pixel
 * has no equivalent of the band.
 */
inline bool isAlarm(const Facts& f) {
    return f.status == statusled::FAULT ||
           f.status == statusled::NO_WIFI ||
           f.darkNode != nullptr ||
           (f.status == statusled::ONLINE && f.role == Role::NODE);
}

// ---------------------------------------------------------------------------
// The whole model: facts in, characters out.
// ---------------------------------------------------------------------------
inline Screen _renderBody(const Facts& f) {
    Screen s;

    // -- header band --------------------------------------------------------
    // Normally the hostname, because "which board am I looking at?" is the
    // first question in a shop with several. A fault replaces it: at that point
    // the identity matters less than the fact, and the band blinks.
    if (f.status == statusled::PORTAL) {
        _fit(s.bar, "setup dustgate");   // no hostname is meaningful yet
    } else if (f.status == statusled::FAULT) {
        _fit(s.bar, "FAULT");
    } else {
        _fit(s.bar, f.hostname ? f.hostname : "dustgate");
    }
    s.barBlink = isAlarm(f);
    s.wifiBars = f.status == statusled::PORTAL ? -1 : f.wifiBars;

    // -- the state word -----------------------------------------------------
    _add(s, stateWord(f), 2);

    char buf[kCols + 1];

    // -- the detail lines ---------------------------------------------------
    // Four rows, and what earns them differs per state. The rule throughout:
    // name the specific thing (which tool, which node, which network), because
    // the general fact is already on the pixel.
    if (f.status == statusled::PORTAL) {
        _add(s, "");                       // breathing room: a stranger reads this
        _add(s, f.apName ? f.apName : "DustGate-Setup");
        if (f.portalIp) {
            snprintf(buf, sizeof(buf), "then open %s", f.portalIp);
            _add(s, buf);
        }
        return s;
    }

    if (f.status == statusled::NO_WIFI) {
        if (f.ssid)         { _pair(buf, "ssid", f.ssid);                 _add(s, buf); }
        if (f.retrySec >= 0){ _pairNum(buf, "retrying in", f.retrySec, "s"); _add(s, buf); }
        if (f.role == Role::PRIMARY && f.gatesTotal >= 0) _add(s, "gates local only");
        return s;
    }

    if (f.darkNode) {
        snprintf(buf, sizeof(buf), "%s lost link", f.darkNode);
        _add(s, buf);
        if (f.darkForSec >= 0) { _pairNum(buf, "last seen", f.darkForSec, "s ago"); _add(s, buf); }
    }

    if (f.role == Role::NODE) {
        if (f.primaryHost) { _pair(buf, "to", f.primaryHost); _add(s, buf); }
        else if (f.status == statusled::ONLINE) _add(s, "wifi ok, no brain");
        if (f.retrySec >= 0)  { _pairNum(buf, "retrying in", f.retrySec, "s"); _add(s, buf); }
        if (f.servoCount >= 0) {
            if (f.status == statusled::ONLINE) _add(s, "servos held");
            else { _pairNum(buf, "servos", f.servoCount); _add(s, buf); }
        }
        if (f.lastCmdSec >= 0) { _pairNum(buf, "last cmd", f.lastCmdSec, "s ago"); _add(s, buf); }
        return s;
    }

    // -- primary ------------------------------------------------------------
    if (f.motion != statusled::STILL && (f.openingGate || f.closingGate)) {
        // Make-before-break means two gates are in flight at once, and which is
        // which is exactly what you walked over to see.
        if (f.toolName) _add(s, f.toolName);
        if (f.openingGate) { _pair(buf, f.openingGate, "opening"); _add(s, buf); }
        if (f.closingGate) { _pair(buf, f.closingGate, "closing"); _add(s, buf); }
        return s;
    }

    if (f.toolName) {
        _add(s, f.toolName);
        if (f.openGate) { _pair(buf, f.openGate, "open"); _add(s, buf); }
        _pair(buf, "collector", f.collectorOn ? "on" : "off");
        _add(s, buf);
        return s;
    }

    if (f.gatesTotal  >= 0) { _pairFrac(buf, "gates", f.gatesReady,  f.gatesTotal);  _add(s, buf); }
    if (f.nodesTotal  >= 0) { _pairFrac(buf, "nodes", f.nodesLinked, f.nodesTotal);  _add(s, buf); }
    if (!f.darkNode)        { _pair(buf, "collector", f.collectorOn ? "on" : "off"); _add(s, buf); }
    return s;
}

/**
 * The whole screen: the body above, plus the build stamp IF a row is left over.
 *
 * Most screens use seven of the eight rows — band (2), state word (2), three
 * details — so there is exactly one row spare, and this is what it is for. The
 * rule is strictly "only what is left": _add() refuses a line that would not
 * fit, so a screen carrying four detail lines simply doesn't get a stamp. It
 * can never push a fact off the panel.
 *
 * Right-aligned, so it reads as a footer rather than as another data line.
 */
inline Screen render(const Facts& f) {
    Screen s = _renderBody(f);
    if (f.buildStamp && *f.buildStamp && s.rows() < kRows) {
        char buf[kCols + 1];
        _pair(buf, "", f.buildStamp);
        _add(s, buf);
    }
    return s;
}

// ---------------------------------------------------------------------------
// Sleep policy — pure, and separate from the drawing for the same reason
// FaultPolicy is separate from the sketch.
//
// An OLED with fixed labels lit 24/7 burns them in: the ghost of `gates` and
// `nodes` etched into every later screen. So the panel blanks after a couple of
// minutes and wakes on events — a gate moving, a tool drawing power, a node
// dropping. A lit screen then MEANS something happened, instead of becoming
// wallpaper nobody reads.
//
// A state a human has to act on holds it awake, because nobody should have to
// press a button to find out what broke. That includes the captive portal: a
// blank screen is useless to a stranger being asked to join an AP.
// ---------------------------------------------------------------------------
// uint32_t, not unsigned long, everywhere below: millis() is 32-bit on the
// device and 64-bit on a test host, and the rollover arithmetic is only correct
// at the device's width. Getting that wrong would pass every host test and then
// blank the screen for 49 days on a real board.
static const uint32_t kIdleBlankMs = 120000;   // two minutes

inline bool awake(const Facts& f, uint32_t lastEventMs, uint32_t nowMs,
                  uint32_t idleMs = kIdleBlankMs) {
    if (isAlarm(f) || f.status == statusled::PORTAL) return true;
    if (f.motion != statusled::STILL) return true;
    // Unsigned subtraction, so this stays correct across the millis() rollover
    // at 49.7 days — a shop controller is expected to run past one.
    return (nowMs - lastEventMs) < idleMs;
}

} // namespace statusscreen

// =============================================================================
// test_statusscreen.cpp — host test for StatusScreenModel.h.
//
// The layout half went in before any hardware existed, and this is why: the only
// real question a 128×64 panel asks is what fits, and that is answerable on a
// laptop. (A panel has since been driven for real — 2026-08-21, DevKitC — and
// none of these assertions had to change.) Every case below is one of the screens drawn in
// docs/mockups/oled-status.html — the mockup and this test are the same design
// stated twice, so when they disagree one of them is wrong.
//
// The invariant that matters, checked on EVERY case rather than case by case:
// no line exceeds 21 characters and no screen exceeds 8 rows. A line that
// overruns doesn't fail loudly on a real panel, it just draws off the right
// edge and takes the end of a hostname with it.
//
// Build + run:
//   c++ -std=c++17 firmware/test/test_statusscreen.cpp -o /tmp/screentest && /tmp/screentest
// (or the tools/ script `firmware:screen:test`)
// =============================================================================
#include <cstdio>
#include <cstring>
#include "../utils/StatusScreenModel.h"

using namespace statusscreen;

static int passed = 0, failed = 0;

static void ok(const char* what, bool cond) {
    if (cond) { printf("  ✓ %s\n", what); passed++; }
    else      { printf("  ✗ %s\n", what); failed++; }
}

/** Every screen pays this toll: within the panel, both ways. */
static void fits(const char* what, const Screen& s) {
    bool good = s.rows() <= kRows && (int)strlen(s.bar) <= kCols;
    for (int i = 0; i < s.lineCount; i++) {
        const int budget = s.lines[i].size == 2 ? kCols / 2 : kCols;
        if ((int)strlen(s.lines[i].text) > budget) good = false;
    }
    if (!good) {
        printf("    ...%s: %d rows, bar \"%s\"\n", what, s.rows(), s.bar);
        for (int i = 0; i < s.lineCount; i++)
            printf("       [%2d] size %d \"%s\"\n",
                   (int)strlen(s.lines[i].text), s.lines[i].size, s.lines[i].text);
    }
    ok(what, good);
}

static bool hasLine(const Screen& s, const char* text) {
    for (int i = 0; i < s.lineCount; i++)
        if (strcmp(s.lines[i].text, text) == 0) return true;
    return false;
}

static const char* headline(const Screen& s) {
    return s.lineCount > 0 ? s.lines[0].text : "";
}

int main() {
    printf("\n== StatusScreenModel ==\n");

    // ── the budget itself ────────────────────────────────────────────────
    // Stated first because everything below depends on it, and because these
    // two numbers come from the panel (6x8 GFX cells in 128x64), not taste.
    ok("21 columns", kCols == 21);
    ok("8 rows",     kRows == 8);
    ok("the header band costs two of them", kBarRows == 2);

    // ── primary: idle, everything linked ─────────────────────────────────
    {
        Facts f;
        f.hostname = "dustgate-a1"; f.wifiBars = 3;
        f.status = statusled::READY;
        f.gatesReady = 4; f.gatesTotal = 4;
        f.nodesLinked = 2; f.nodesTotal = 2;
        Screen s = render(f);
        fits("idle primary fits", s);
        ok("...says READY",            strcmp(headline(s), "READY") == 0);
        ok("...names the board",       strcmp(s.bar, "dustgate-a1") == 0);
        ok("...counts gates",          hasLine(s, "gates             4/4"));
        ok("...counts nodes",          hasLine(s, "nodes             2/2"));
        ok("...and the collector",     hasLine(s, "collector         off"));
        ok("...does not blink",        !s.barBlink);
    }

    // ── primary: a tool is running ───────────────────────────────────────
    {
        Facts f;
        f.hostname = "dustgate-a1"; f.wifiBars = 3;
        f.status = statusled::READY;
        f.toolName = "Table Saw"; f.openGate = "gate 3"; f.collectorOn = true;
        Screen s = render(f);
        fits("running primary fits", s);
        ok("...says RUNNING",          strcmp(headline(s), "RUNNING") == 0);
        // The TOOL's name, never a port id — that is what the woodworker
        // recognises, and it is the same rule the canvas follows.
        ok("...names the tool",        hasLine(s, "Table Saw"));
        ok("...names its gate",        hasLine(s, "gate 3           open"));
        ok("...collector on",          hasLine(s, "collector          on"));
    }

    // ── primary: mid-transition ──────────────────────────────────────────
    {
        // Make-before-break: two gates in flight, and both get named. A screen
        // showing only the opening one would look identical to a shop that
        // dead-headed the collector.
        Facts f;
        f.hostname = "dustgate-a1"; f.wifiBars = 3;
        f.status = statusled::READY; f.motion = statusled::MOVING;
        f.toolName = "Jointer"; f.openingGate = "gate 1"; f.closingGate = "gate 3";
        Screen s = render(f);
        fits("transition fits", s);
        ok("motion outranks status",   strcmp(headline(s), "MOVING") == 0);
        ok("...names the opening gate", hasLine(s, "gate 1        opening"));
        ok("...names the closing gate", hasLine(s, "gate 3        closing"));
    }

    // ── primary: a node went dark ────────────────────────────────────────
    {
        Facts f;
        f.hostname = "dustgate-a1"; f.wifiBars = 3;
        f.status = statusled::FAULT;
        f.darkNode = "qtpy-2"; f.darkForSec = 42;
        f.gatesReady = 2; f.gatesTotal = 4;
        Screen s = render(f);
        fits("node-dark fits", s);
        ok("...says NODE DARK",        strcmp(headline(s), "NODE DARK") == 0);
        ok("...names the board",       hasLine(s, "qtpy-2 lost link"));
        ok("...says how long",         hasLine(s, "last seen     42s ago"));
        // Still shows what works. "Something is broken" is the pixel's job;
        // this screen exists to say how much of the shop is left.
        ok("...still counts gates",    hasLine(s, "gates             2/4"));
        ok("...the band blinks",       s.barBlink);
        ok("...the band says FAULT",   strcmp(s.bar, "FAULT") == 0);
    }

    // ── primary: blue for two different reasons ──────────────────────────
    // The pixel shows ONLINE for "no topology stored" and for "a paired board is
    // dark". Same colour, different problem — and the screen exists to say which.
    {
        Facts f;
        f.hostname = "dustgate-a1"; f.wifiBars = 3;
        f.status = statusled::ONLINE;
        Screen bare = render(f);
        ok("blue with no topology says NO SHOP", strcmp(headline(bare), "NO SHOP") == 0);

        f.darkNode = "qtpy-2"; f.darkForSec = 8;
        f.gatesReady = 2; f.gatesTotal = 4;
        Screen dark = render(f);
        fits("blue-with-a-dark-node fits", dark);
        ok("blue with a dark node names it instead",
           strcmp(headline(dark), "NODE DARK") == 0 && hasLine(dark, "qtpy-2 lost link"));
        // Blue on the pixel, blinking on the screen. Not a disagreement: blue is
        // honest (nothing here is broken), the blink is the band saying "this is
        // the bit you came to read".
        ok("...and blinks even though the pixel is blue", dark.barBlink);
        ok("...keeping the board's own name in the band",
           strcmp(dark.bar, "dustgate-a1") == 0);
    }

    // ── primary: off the network ─────────────────────────────────────────
    {
        Facts f;
        f.hostname = "dustgate-a1"; f.wifiBars = 0;
        f.status = statusled::NO_WIFI;
        f.ssid = "ShopNet-5G"; f.retrySec = 12; f.gatesTotal = 4;
        Screen s = render(f);
        fits("no-wifi fits", s);
        ok("...says NO WIFI",          strcmp(headline(s), "NO WIFI") == 0);
        // Half of all WiFi faults are the wrong SSID, so it says which.
        ok("...names the network",     hasLine(s, "ssid       ShopNet-5G"));
        ok("...counts down the retry", hasLine(s, "retrying in       12s"));
    }

    // ── primary: first boot, no credentials ──────────────────────────────
    {
        // The one screen a stranger has to follow, so it is instructions and
        // nothing else — no hostname, no counts, no signal glyph.
        Facts f;
        f.status = statusled::PORTAL;
        f.apName = "DustGate-Setup"; f.portalIp = "192.168.4.1";
        Screen s = render(f);
        fits("portal fits", s);
        ok("...says JOIN WIFI",        strcmp(headline(s), "JOIN WIFI") == 0);
        ok("...names the AP",          hasLine(s, "DustGate-Setup"));
        ok("...gives the address",     hasLine(s, "then open 192.168.4.1"));
        ok("...no signal glyph",       s.wifiBars < 0);
        ok("...no hostname",           strcmp(s.bar, "setup dustgate") == 0);
    }

    // ── node: linked ─────────────────────────────────────────────────────
    {
        Facts f;
        f.role = Role::NODE;
        f.hostname = "qtpy-2"; f.wifiBars = 3;
        f.status = statusled::READY;
        f.primaryHost = "dustgate-a1"; f.servoCount = 4; f.lastCmdSec = 3;
        Screen s = render(f);
        fits("linked node fits", s);
        ok("...says LINKED",           strcmp(headline(s), "LINKED") == 0);
        ok("...names its brain",       hasLine(s, "to        dustgate-a1"));
        ok("...counts servos",         hasLine(s, "servos              4"));
        ok("...ages the last command", hasLine(s, "last cmd       3s ago"));
    }

    // ── node: on wifi, no primary ────────────────────────────────────────
    {
        // The state the pairing bug in TODO actually produces, and the one that
        // currently needs a serial monitor to see at all.
        Facts f;
        f.role = Role::NODE;
        f.hostname = "qtpy-2"; f.wifiBars = 2;
        f.status = statusled::ONLINE;
        f.servoCount = 4; f.retrySec = 4;
        Screen s = render(f);
        fits("unlinked node fits", s);
        ok("...says UNLINKED",         strcmp(headline(s), "UNLINKED") == 0);
        ok("...says why",              hasLine(s, "wifi ok, no brain"));
        ok("...counts down the retry", hasLine(s, "retrying in        4s"));
        ok("...says the servos held",  hasLine(s, "servos held"));
        ok("...and blinks about it",   s.barBlink);
    }

    // ── the vocabulary is not re-invented ────────────────────────────────
    // Every statusled::Status has a word, and no word is empty. A state the
    // pixel can show but the screen can't spell would be exactly the drift
    // StatusVocab.h exists to prevent.
    {
        const statusled::Status all[] = {
            statusled::FAULT, statusled::BOOTING, statusled::PORTAL,
            statusled::NO_WIFI, statusled::ONLINE, statusled::READY };
        bool everyState = true, everyRole = true;
        for (statusled::Status st : all) {
            Facts p; p.status = st;
            Facts n; n.status = st; n.role = Role::NODE;
            if (strlen(stateWord(p)) == 0) everyState = false;
            if (strlen(stateWord(n)) == 0) everyRole  = false;
            // 10 columns is the size-2 ceiling, and a clipped state word is the
            // one truncation that would actually mislead.
            if ((int)strlen(stateWord(p)) > kCols / 2) everyState = false;
            if ((int)strlen(stateWord(n)) > kCols / 2) everyRole  = false;
        }
        ok("every Status has a word (primary)", everyState);
        ok("every Status has a word (node)",    everyRole);
    }

    // ── truncation never overruns ────────────────────────────────────────
    {
        Facts f;
        f.hostname = "a-really-long-mdns-hostname.local";
        f.status = statusled::READY;
        f.toolName = "The Big Sawstop In The Corner";
        f.openGate = "a selector with a very long name indeed";
        Screen s = render(f);
        fits("absurd names still fit", s);
        ok("...the hostname is cut, not wrapped", (int)strlen(s.bar) == kCols);
    }
    {
        // The label loses, not the value: "4/4" is the news and a clipped
        // count would be a lie rather than an abbreviation.
        char buf[kCols + 1];
        _pair(buf, "an extremely long label here", "4/4");
        ok("a colliding pair keeps its value", strlen(buf) == (size_t)kCols &&
                                               strcmp(buf + kCols - 3, "4/4") == 0);
    }

    // ── the build stamp ──────────────────────────────────────────────────
    // "Which build is on this board?" is otherwise answered by reflashing to be
    // sure, which destroys the state you were debugging.
    {
        char b[32];
        formatBuild("Aug 20 2026", "07:25:00", b, sizeof(b));
        ok("__DATE__/__TIME__ reads as a date", strcmp(b, "8/20/26 7:25:00") == 0);
        // __DATE__ pads a single-digit day with a SPACE, which is the detail
        // that breaks parsing at a fixed offset.
        formatBuild("Aug  5 2026", "23:04:09", b, sizeof(b));
        ok("a space-padded day parses",         strcmp(b, "8/5/26 23:04:09") == 0);
        formatBuild("Dec 31 2026", "00:00:00", b, sizeof(b));
        ok("december, midnight",                strcmp(b, "12/31/26 0:00:00") == 0);
        // Empty, never half-formatted: a wrong date sends you to the wrong binary.
        formatBuild("Xxx 20 2026", "07:25:00", b, sizeof(b));
        ok("an unparseable month yields nothing", b[0] == '\0');
        formatBuild(nullptr, nullptr, b, sizeof(b));
        ok("nulls yield nothing",                 b[0] == '\0');
        ok("it fits the bottom row", strlen("12/31/26 23:04:09") <= (size_t)kCols);
    }
    {
        // It rides the row nothing else wanted — and only that row.
        Facts f;
        f.hostname = "dustgate-a1"; f.wifiBars = 3;
        f.status = statusled::READY;
        f.gatesReady = 4; f.gatesTotal = 4;
        f.nodesLinked = 2; f.nodesTotal = 2;
        Screen without = render(f);
        f.buildStamp = "8/20/26 7:25:00";
        Screen with = render(f);
        fits("the stamp still fits", with);
        ok("the idle screen had a spare row",  without.rows() == kRows - 1);
        ok("...and the stamp takes it",        with.rows() == kRows);
        ok("...right-aligned on the bottom",
           strcmp(with.lines[with.lineCount - 1].text, "      8/20/26 7:25:00") == 0);
        ok("...displacing nothing above it",   with.lineCount == without.lineCount + 1);
    }
    {
        // A screen that genuinely fills its eight rows — a dark node names the
        // board, ages it, and still counts what is left — drops the stamp rather
        // than a fact. _add() refuses the line; it never truncates one.
        Facts f;
        f.hostname = "dustgate-a1"; f.wifiBars = 3;
        f.status = statusled::FAULT;
        f.darkNode = "qtpy-2"; f.darkForSec = 42;
        f.gatesReady = 2; f.gatesTotal = 4;
        f.nodesLinked = 1; f.nodesTotal = 2;
        Screen full = render(f);
        ok("the fault screen fills the panel", full.rows() == kRows);
        f.buildStamp = "8/20/26 7:25:00";
        Screen stamped = render(f);
        fits("...and offering it a stamp changes nothing", stamped);
        ok("a full screen drops the stamp, not a fact",
           !hasLine(stamped, "      8/20/26 7:25:00") &&
           hasLine(stamped, "last seen     42s ago") &&
           hasLine(stamped, "gates             2/4"));
    }
    {
        // The portal screen has a spare row (its deliberate blank line is row 4
        // of 8), so a board fresh out of the box shows which build it is running
        // while it waits to be told a network. That is the moment it is most
        // likely to be the wrong one.
        Facts f;
        f.status = statusled::PORTAL;
        f.apName = "DustGate-Setup"; f.portalIp = "192.168.4.1";
        f.buildStamp = "8/20/26 7:25:00";
        Screen s = render(f);
        fits("stamped portal fits", s);
        ok("the portal keeps its instructions", hasLine(s, "then open 192.168.4.1"));
        ok("...and still shows the build",      hasLine(s, "      8/20/26 7:25:00"));
    }

    // ── sleep policy ─────────────────────────────────────────────────────
    // The timer has no exceptions as of 2026-08-22: a fault that lasts a
    // weekend would otherwise burn itself into the glass. What used to hold the
    // screen lit now only WAKES it, via the state hash in StatusScreen.h.
    {
        ok("awake right after an event",  awake(1000, 1000));
        ok("awake a minute later",        awake(1000, 61000));
        ok("asleep after two minutes",   !awake(1000, 1000 + kIdleBlankMs));
        ok("a fault does not hold it awake",  !awake(1000, 1000 + kIdleBlankMs * 10));
        // A shop controller is expected to run past 49.7 days.
        ok("survives the millis rollover",
           awake(0xFFFFFF00u, 0x00000100u));
    }

    // ── the servo self-test screen ───────────────────────────────────────
    // The bench tool on the wake button's long press. It owns the panel while it
    // runs: a person is standing at the board comparing this angle to a valve.
    {
        Facts f;
        f.role = Role::NODE;              // both roles show it — a node has servos too
        f.status = statusled::READY;
        f.hostname = "dustgate-node-1";
        f.selfTestCh = 3; f.selfTestOf = 4; f.selfTestAngle = 120;
        Screen s = render(f);
        ok("the self-test names itself",   !strcmp(stateWord(f), "SERVOTEST"));
        ok("...says which channel",        hasLine(s, "servo             3/4"));
        ok("...and the commanded angle",   hasLine(s, "commanded      120deg"));
        ok("...and the board, in the band", !strcmp(s.bar, "dustgate-node-1"));
        fits("the self-test screen fits", s);
        // It outranks a fault on purpose: the finger on the button wins over a
        // standing condition, because the question being asked is "did that do
        // anything?" and the answer has to be on the glass.
        Facts g = f; g.status = statusled::FAULT;
        ok("...and it outranks a fault",   !strcmp(stateWord(g), "SERVOTEST"));
    }
    {
        Facts f;
        f.status = statusled::READY;
        f.selfTestRefused = "collector on";
        Screen s = render(f);
        ok("a refusal says so",          !strcmp(stateWord(f), "REFUSED"));
        ok("...and says why",            hasLine(s, "collector on"));
        fits("the refusal screen fits", s);
    }

    // ── the button's off half ────────────────────────────────────────────
    // A press on a lit panel blanks it (2026-08-22), by backdating the event
    // clock rather than carrying a second "user turned it off" concept. These
    // assert the property that choice buys: off now, but not off through the
    // next thing worth seeing.
    {
        const uint32_t now = 500000;
        ok("a deliberate blank reads as asleep",
           !awake(blankedAt(now), now));
        ok("...and stays asleep as time passes",
           !awake(blankedAt(now), now + 60000));
        ok("...but the next event lights it again",
           awake(now + 1, now + 1));
        // Pressed inside the first two minutes after boot, blankedAt() wraps.
        // Same-width subtraction in awake() makes that a non-event; get the
        // width wrong and the panel refuses to blank until the board has been
        // up for two minutes.
        const uint32_t early = 5000;
        ok("blanks correctly before the clock reaches one timeout",
           !awake(blankedAt(early), early));
    }

    // isAlarm() still exists — it blinks the header band — it just no longer
    // has anything to do with sleeping.
    {
        Facts fault;  fault.status = statusled::FAULT;
        Facts ready;  ready.status = statusled::READY;
        ok("a fault still blinks the band", render(fault).barBlink);
        ok("a ready board does not",       !render(ready).barBlink);
    }

    printf("\n%d/%d passed\n", passed, passed + failed);
    return failed == 0 ? 0 : 1;
}

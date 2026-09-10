// =============================================================================
// OutletSweep.h — knock on every address on the subnet, one per loop() pass.
//
// WHY THIS EXISTS. mDNS finds a Shelly in three seconds and will never find a
// Tasmota at all: Tasmota's mDNS needs USE_DISCOVERY compiled in and is not in
// the precompiled builds (see the warning on MdnsHit::devicetype in
// utils/MdnsQuery.h). Since a no-relay Tasmota is now the preferred plug for a
// tool — it structurally CANNOT switch, which is a safety property and not a
// preference (see CLAUDE.md) — "you can only find it if you already know its
// IP" is not an acceptable answer for the plug we most want people to use.
//
// So we knock on all 254. That is survivable because it happens ONCE PER PLUG,
// EVER: a paired plug's address lives in the layout, so the sweep is only ever
// looking for something new. It is why the button says "Look for new" rather
// than "Scan".
//
// THIS CLASS OWNS THE CURSOR, NOT THE PROBING. It hands out one address at a
// time and takes back whatever the caller found. That split is deliberate:
// probing means blocking HTTP, which must happen on the main loop task (the
// same constraint discover and ping already live under — see
// consumeDiscoverRequest() in firmware.ino), while the progress this reports
// has to be readable from the HTTP task at any moment.
//
// ONE ADDRESS PER PASS, and that is the whole concurrency design. An address
// with nothing at it costs a full connect timeout — ~250 ms, and there are
// usually ~250 of those — so a sweep is a minute of wall time no matter how it
// is sliced. Slicing it to a single address per loop() pass means the watchdog
// gets petted, the routing runtime still runs, and gate moves see at most one
// probe's latency rather than the whole minute. A tool can start mid-sweep and
// its gate still opens.
//
// NOT A DEFERRED REPLY. discover and ping both hold the HTTP request open while
// the main loop works, which is fine for three seconds and impossible for
// sixty — HttpApiServer's deferred budget is 15 s. So the shape here is
// start / poll / cancel: POST returns immediately, GET reports progress from
// this struct without blocking on anything.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <vector>
#include "../config.h"

class OutletSweep {
public:
    // Start a pass over <prefix>.1 .. <prefix>.254, skipping our own address.
    // Returns false if one is already running — a second Start is a no-op
    // rather than a restart, so double-tapping the button cannot lose progress.
    bool begin(const IPAddress& self) {
        if (_running) return false;
        _a = self[0]; _b = self[1]; _c = self[2]; _self = self[3];
        _cursor    = kFirstHost;
        _scanned   = 0;
        _running   = true;
        _cancelled = false;
        _startedMs = millis();
        _finishedMs = 0;
        _rows.clear();
        return true;
    }

    // Stop early. The rows found so far are KEPT: someone who stops a sweep
    // because they saw the plug they wanted appear should not lose it.
    void cancel() {
        if (!_running) return;
        _running    = false;
        _cancelled  = true;
        _finishedMs = millis();
    }

    // Next address to probe, or false when the pass is done. Advances the
    // cursor, so each call hands out a different address.
    bool nextAddress(char* out, size_t len) {
        if (!_running) return false;
        while (_cursor <= kLastHost && _cursor == _self) _cursor++;  // that is us
        if (_cursor > kLastHost) {
            _running    = false;
            _finishedMs = millis();
            return false;
        }
        snprintf(out, len, "%d.%d.%d.%d", _a, _b, _c, _cursor);
        _cursor++;
        _scanned++;
        return true;
    }

    // Record a plug that answered. `row` is the serialized JSON object the
    // picker reads — built by describeOutletInto(), so a swept plug and a
    // discovered one are the same shape, because they must be interchangeable
    // in the list the user picks from.
    //
    // Silently DROPS past the cap rather than growing without bound: this runs
    // on a device with 320 KB of RAM and a malicious or misconfigured network
    // must not be able to grow a vector until the heap gives out. The cap is
    // the same one discovery uses, and no real shop approaches it.
    void record(const String& row) {
        if ((int)_rows.size() >= DISCOVER_MAX_RESULTS) return;
        _rows.push_back(row);
    }

    bool     running()    const { return _running; }
    bool     cancelled()  const { return _cancelled; }
    int      scanned()    const { return _scanned; }
    int      total()      const { return kLastHost - kFirstHost + 1; }
    int      foundCount() const { return (int)_rows.size(); }
    uint32_t startedMs()  const { return _startedMs; }
    uint32_t finishedMs() const { return _finishedMs; }
    // Has a pass ever completed? Distinguishes "never looked" from "looked and
    // found nothing", which the bar renders differently and which is exactly
    // the difference the empty state has to carry.
    bool     everRan()    const { return _startedMs != 0; }

    const std::vector<String>& rows() const { return _rows; }

private:
    // .0 is the network and .255 the broadcast address; neither is a host.
    static const int kFirstHost = 1;
    static const int kLastHost  = 254;

    uint8_t  _a = 0, _b = 0, _c = 0, _self = 0;
    int      _cursor  = kFirstHost;
    int      _scanned = 0;
    bool     _running   = false;
    bool     _cancelled = false;
    uint32_t _startedMs  = 0;
    uint32_t _finishedMs = 0;
    std::vector<String> _rows;
};

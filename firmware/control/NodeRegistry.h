// =============================================================================
// NodeRegistry.h — the boards this primary is PAIRED with, persisted in NVS.
//
// WHY THIS EXISTS SEPARATELY FROM THE TOPOLOGY:
//
// Links used to be created solely from `controllers[]` in the stored topology.
// That made pairing a property of the shop layout, with three consequences that
// all bit during bring-up:
//
//   1. No topology → no links. A full `dev.sh flash` rewrites the LittleFS image
//      (the Angular bundle and topology.json share a partition), so every UI
//      deploy silently un-paired every node. The node sat on WiFi showing
//      "online, nobody talking to me" and nothing anywhere said why.
//   2. You couldn't pair before designing. The natural order is: put the board on
//      the wall, confirm it answers, THEN draw the shop around it. The old order
//      demanded a valid layout naming the node before it would ever be dialled.
//   3. A layout edit could tear down a healthy link, because adopting a topology
//      rebuilt every RemoteActuatorBus from scratch.
//
// So pairing is now its own fact, keyed by mDNS host, surviving reboots, topology
// wipes and layout edits. The topology still says which BOARD DRIVES WHICH GATE —
// that genuinely is layout — but it no longer owns the connection's lifetime.
//
// Keyed by host rather than by a controllerId: the host is what you actually dial
// and what the node itself reports in WELCOME. A controllerId is a name someone
// chose in the UI, and names change.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "NodeBus.h"   // topo::bareHost — one spelling of a host, everywhere

namespace topo {

// Same ceiling as the link-slot array in the sketch (MAX_SECONDARY_NODES), and
// it must STAY the same: a board this registry accepts but the sketch has no
// slot for is paired, persisted, and never dialled.
//
// Ten since 2026-09-16, up from three — which was written as "not a limit anyone
// will hit" and was hit by the first real shop layout, a board per machine
// rather than per wall. add() returning false is the only symptom, and the UI
// shows it as a pairing that did not take.
static const int kMaxPairedNodes = 10;

// Long enough for "dustgate-node-1.local" and then some. Hosts are stored as
// entered — bare or qualified — and RemoteActuatorBus appends ".local" at dial
// time for anything without a dot.
static const size_t kMaxHostLen = 64;

// Friendly name, e.g. "Back wall".
static const size_t kMaxNameLen = 32;

// "255.255.255.255" and a terminator.
static const size_t kMaxIpLen = 16;

class NodeRegistry {
public:
    // NVS namespace is shared with the API server's config on purpose: one
    // Preferences namespace per flash partition avoids opening several.
    void begin() { load(); }

    int      count() const { return _count; }
    const char* host(int i) const { return (i >= 0 && i < _count) ? _hosts[i] : ""; }

    // ── the address this board last answered on ──────────────────────────────
    //
    // NOT the pairing. The HOST is what a person chose and what the layout
    // names; this is a cache of where it was, kept so a silent mDNS querier at
    // boot does not cost us the board entirely. Stored here rather than in the
    // bus because this is the file that already owns per-board persistence.
    //
    // A stale entry is harmless: RemoteActuatorBus re-resolves on its own
    // cadence whenever the link is down and overwrites this the moment mDNS
    // answers, so DHCP moving a board costs a reconnect rather than a reboot.
    const char* lastIp(int i) const { return (i >= 0 && i < _count) ? _ips[i] : ""; }

    /** Remember where a paired board answered. No-op for an unknown host, and
     *  writes only on CHANGE — this is called from the main loop and NVS has a
     *  finite number of erase cycles. */
    void setLastIp(const char* h, const char* ip) {
        if (!h || !*h || !ip || !*ip) return;
        const std::string want = bareHost(h);
        for (int i = 0; i < _count; i++) {
            if (bareHost(_hosts[i]) != want) continue;
            if (strcmp(_ips[i], ip) == 0) return;      // unchanged — no write
            strlcpy(_ips[i], ip, kMaxIpLen);
            Preferences p;
            p.begin("api_cfg", false);
            char key[12]; snprintf(key, sizeof(key), "node_i%d", i);
            p.putString(key, _ips[i]);
            p.end();
            return;
        }
    }
    // Friendly name ("Back wall"). Lives HERE rather than in the topology's
    // controllers[] so the boards screen needs no layout at all — you can pair and
    // name every board before drawing a single duct. Falls back to the host.
    const char* name(int i) const {
        if (i < 0 || i >= _count) return "";
        return _names[i][0] ? _names[i] : _hosts[i];
    }

    bool has(const char* h) const {
        if (!h || !*h) return false;
        // Bare-label comparison: "node-1" and "node-1.local" are the same board,
        // and the UI can hand us either.
        const std::string want = bareHost(h);
        for (int i = 0; i < _count; i++) if (bareHost(_hosts[i]) == want) return true;
        return false;
    }

    // Returns false only when full. Idempotent by design: pairing the same board
    // twice is a no-op, not an error the UI has to explain — and re-pairing with a
    // new name is how a rename is applied, so there is one code path for both.
    bool add(const char* h, const char* n = nullptr) {
        if (!h || !*h || strlen(h) >= kMaxHostLen) return false;
        const std::string want = bareHost(h);
        for (int i = 0; i < _count; i++) {
            if (bareHost(_hosts[i]) != want) continue;
            if (n && *n) { strlcpy(_names[i], n, kMaxNameLen); save(); }
            return true;
        }
        if (_count >= kMaxPairedNodes) return false;
        strlcpy(_hosts[_count], h, kMaxHostLen);
        strlcpy(_names[_count], (n && *n) ? n : "", kMaxNameLen);
        _ips[_count][0] = '\0';          // nothing known yet; the first resolve fills it
        _count++;
        save();
        return true;
    }

    bool remove(const char* h) {
        const std::string want = bareHost(h);
        for (int i = 0; i < _count; i++) {
            if (bareHost(_hosts[i]) != want) continue;
            for (int j = i; j + 1 < _count; j++) {
                strlcpy(_hosts[j], _hosts[j + 1], kMaxHostLen);
                strlcpy(_names[j], _names[j + 1], kMaxNameLen);
                strlcpy(_ips[j],   _ips[j + 1],   kMaxIpLen);
            }
            _count--;
            save();
            return true;
        }
        return false;
    }

private:
    // One key per slot rather than a serialized blob: a corrupt or partially
    // written entry then costs one node, not the whole pairing set.
    void load() {
        Preferences p;
        p.begin("api_cfg", true);
        _count = p.getInt("node_n", 0);
        if (_count < 0 || _count > kMaxPairedNodes) _count = 0;
        char key[12];
        for (int i = 0; i < _count; i++) {
            snprintf(key, sizeof(key), "node_h%d", i);
            strlcpy(_hosts[i], p.getString(key, "").c_str(), kMaxHostLen);
            snprintf(key, sizeof(key), "node_n%d", i);
            strlcpy(_names[i], p.getString(key, "").c_str(), kMaxNameLen);
            snprintf(key, sizeof(key), "node_i%d", i);
            strlcpy(_ips[i], p.getString(key, "").c_str(), kMaxIpLen);
        }
        p.end();
        // Drop any slot that came back empty, so a half-written registry can't
        // leave the sketch dialling "".
        int w = 0;
        for (int i = 0; i < _count; i++) {
            if (!_hosts[i][0]) continue;
            if (w != i) { strlcpy(_hosts[w], _hosts[i], kMaxHostLen);
                          strlcpy(_names[w], _names[i], kMaxNameLen);
                          strlcpy(_ips[w],   _ips[i],   kMaxIpLen); }
            w++;
        }
        _count = w;
    }

    void save() {
        Preferences p;
        p.begin("api_cfg", false);
        p.putInt("node_n", _count);
        char key[12];
        for (int i = 0; i < _count; i++) {
            snprintf(key, sizeof(key), "node_h%d", i);
            p.putString(key, _hosts[i]);
            snprintf(key, sizeof(key), "node_n%d", i);
            p.putString(key, _names[i]);
            snprintf(key, sizeof(key), "node_i%d", i);
            p.putString(key, _ips[i]);
        }
        p.end();
    }

    char _hosts[kMaxPairedNodes][kMaxHostLen] = {};
    char _names[kMaxPairedNodes][kMaxNameLen] = {};
    char _ips[kMaxPairedNodes][kMaxIpLen]     = {};
    int  _count = 0;
};

} // namespace topo

// =============================================================================
// MdnsQuery.h — mDNS service queries that don't stall the board
//
// ESPmDNS's MDNSResponder::queryService() blocks for a HARDCODED 3000ms (see
// ESPmDNS.cpp), not configurable through the Arduino wrapper, and discovery
// retries a few times to work around lossy UDP — 9+ seconds of blocked main
// loop, long enough to trip a watchdog reset or let the browser's request go
// stale and crash the device on the reply. So this talks to the ESP-IDF mDNS
// API directly.
//
// It did that with a SHORT timeout until 2026-08-22, when the short timeout
// turned out to be the reason a C5 primary could see none of its four plugs.
// mdnsQueryService() below now polls an async search instead, which makes a
// long window cheap — read its comment before changing any timeout, because it
// records what the two obvious ways of measuring this get wrong.
//
// Shelly discovery uses _shelly._tcp, advertised ONLY by Gen2+ Shelly devices —
// so a hit is unambiguously a supported plug, with no hostname guessing. (Gen1
// is not supported; the generic _http._tcp helper below is retained only for
// possible future use.) Gen2+ devices include a "gen" TXT key (gen=2, gen=3,
// ...) giving the generation without probing.
// See https://shelly-api-docs.shelly.cloud/gen2/General/mDNS/
// =============================================================================

#pragma once
#include <Arduino.h>
#include <mdns.h>
#include "Watchdog.h"   // the poll below outlives a loop() iteration

struct MdnsHit {
    String hostname;
    String ip;
    int    gen;   // "gen" TXT value (2, 3, ...); 0 = key absent (Gen1 or not a Shelly)
    String role;  // "role" TXT — DustGate nodes advertise role=secondary
    String board; // "board" TXT — build target of a DustGate node
    int    servos; // "servos" TXT — servo channel count on a DustGate node
    String owner;  // "owner" TXT — the primary that has claimed this node
                   // ("" = unclaimed). A hint published at the node's boot, not
                   // a live authority; see the note where the node sets it.
};

// Fills `out` from one mDNS answer. TXT records are optional and their order
// isn't guaranteed, so the keys get scanned for rather than indexed: Shelly
// plugs carry "gen"; DustGate secondary nodes carry role/board/servos/owner
// (see node/dustgate_node.cpp).
inline void mdnsParseHit(const mdns_result_t* r, MdnsHit& out) {
    IPAddress ip;
    for (mdns_ip_addr_t* a = r->addr; a; a = a->next) {
        if (a->addr.type == MDNS_IP_PROTOCOL_V4) {
            ip = IPAddress(a->addr.u_addr.ip4.addr);
            break;
        }
    }

    int gen = 0, servos = 0;
    String role, board, owner;
    for (size_t t = 0; t < r->txt_count; t++) {
        const char* k = r->txt[t].key;
        const char* v = r->txt[t].value;
        if (!k || !v) continue;
        if      (strcmp(k, "gen")    == 0) gen    = atoi(v);
        else if (strcmp(k, "servos") == 0) servos = atoi(v);
        else if (strcmp(k, "role")   == 0) role   = v;
        else if (strcmp(k, "board")  == 0) board  = v;
        else if (strcmp(k, "owner")  == 0) owner  = v;
    }

    out.hostname = r->hostname ? String(r->hostname) : String();
    out.ip       = ip.toString();
    out.gen      = gen;
    out.role     = role;
    out.board    = board;
    out.servos   = servos;
    out.owner    = owner;
}

// Queries <service>.<proto>, waiting up to timeoutMs for responses. Returns the
// number of hits written into `hits` (capped at maxHits).
//
// ASYNC UNDERNEATH, AND THAT IS THE POINT (2026-08-22). This used to call the
// blocking mdns_query_ptr(), which is why every caller passed it a stingy
// timeout — 400ms for plugs, 800ms for nodes — and why a XIAO C5 primary could
// not find a single one of the four Shelly plugs sitting on its own network. At
// 3000ms it found all four, first try, from cold. The window was the whole bug.
//
// The blocking call made that window expensive in two ways. It stalled loop(),
// and loop() is the only thing that pets the watchdog (firmware.ino) — a probe
// that spent ~11s inside one processLine() call reset the board outright, with
// WDT_TIMEOUT_SEC at 10. So the timeout was caught between a floor set by the
// network and a ceiling set by the watchdog, with the shop's actual answer time
// above the ceiling.
//
// mdns_query_async_new() starts the search and returns immediately, so this
// polls it in 25ms slices and pets the watchdog on every one. The window is now
// free: a 3s query costs 120 pets and no reset. What it still costs is a stalled
// loop for the length of the scan — gate control does not run while a scan is
// in flight — which is acceptable because discovery is an explicit, infrequent
// wizard action taken while nothing is cutting. If that ever stops being true,
// the next step is to hoist the poll into loop() itself and let the deferred
// HTTP reply (HttpApiServer.h, 15s budget) collect it across iterations. The
// call sites are shaped for that already.
//
// NOTE ON MEASURING THIS: don't bother trying to time individual answers.
// mdns_query_async_get_results() only fills num_results when the search has
// FINISHED, so polling reports zero, zero, zero, then all of them at the
// window's edge. And a second query for the same service type reads back the
// mDNS cache the first one filled, which makes any repeated-query "ladder"
// report the cache's speed rather than the network's. Both mistakes were made
// here on 2026-08-22; the second one produced a confident, entirely fictional
// "200ms is plenty".
inline int mdnsQueryService(const char* service, const char* proto,
                            uint32_t timeoutMs, MdnsHit hits[], int maxHits) {
    mdns_search_once_t* search = mdns_query_async_new(
        nullptr, service, proto, MDNS_TYPE_PTR, timeoutMs, (size_t)maxHits, nullptr);
    if (!search) {
        Serial.print(F("[MDNS] ")); Serial.print(service);
        Serial.println(F(": could not start the search (out of memory?)"));
        return 0;
    }

    const uint32_t t0 = millis();
    mdns_result_t* results = nullptr;
    bool done = false;
    while (!done) {
        watchdog::pet();
        delay(25);
        done = mdns_query_async_get_results(search, 0, &results, nullptr);
        // The search owns its own timeout; this only catches a search that
        // somehow never reports finished, so the loop can't become the stall it
        // was written to prevent.
        if (millis() - t0 > timeoutMs + 1000) break;
    }

    int count = 0;
    for (mdns_result_t* r = results; r && count < maxHits; r = r->next) {
        mdnsParseHit(r, hits[count]);
        count++;
    }

    // Every query says how it went, not just the ones that blow up. A bare
    // "0 hit(s)" is several different bugs wearing the same face, and the
    // elapsed time is what separates "waited and heard nothing" from "never
    // waited at all".
    Serial.print(F("[MDNS] ")); Serial.print(service); Serial.print(F("."));
    Serial.print(proto);
    Serial.print(F(" -> ")); Serial.print(count);
    Serial.print(F(" in ")); Serial.print(millis() - t0);
    Serial.print(F("/")); Serial.print(timeoutMs); Serial.println(F("ms"));

    if (results) mdns_query_results_free(results);
    mdns_query_async_delete(search);
    watchdog::pet();
    return count;
}

// Shelly-only service (Gen2+). Every hit is a Shelly device.
inline int mdnsQueryShellyTcp(uint32_t timeoutMs, MdnsHit hits[], int maxHits) {
    return mdnsQueryService("_shelly", "_tcp", timeoutMs, hits, maxHits);
}

// DustGate secondary nodes (_dustgate._tcp). Every hit is a board offering a
// /nodelink WebSocket; check hit.role == "secondary" before offering it as a
// target, so a primary never lists itself or another shop's brain.
inline int mdnsQueryDustgateTcp(uint32_t timeoutMs, MdnsHit hits[], int maxHits) {
    return mdnsQueryService("_dustgate", "_tcp", timeoutMs, hits, maxHits);
}

// Generic HTTP service — callers must filter; used to reach Gen1 Shellies.
inline int mdnsQueryHttpTcp(uint32_t timeoutMs, MdnsHit hits[], int maxHits) {
    return mdnsQueryService("_http", "_tcp", timeoutMs, hits, maxHits);
}

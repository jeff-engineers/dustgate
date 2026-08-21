// =============================================================================
// MdnsQuery.h — mDNS service queries with a short, caller-controlled timeout
//
// ESPmDNS's MDNSResponder::queryService() calls the underlying ESP-IDF
// mdns_query_ptr() with a HARDCODED 3000ms timeout (see ESPmDNS.cpp) — not
// configurable through the Arduino wrapper. The outlet discovery feature
// retries the query a few times to work around lossy UDP responses, which at
// 3000ms/attempt meant 9+ seconds of blocking the main loop task in one
// stretch — long enough to either trip a watchdog reset or let the browser's
// HTTP request go stale, so by the time discovery finished and tried to
// respond, the connection/request object was gone and sending to it crashed
// the device. This calls the ESP-IDF mDNS API directly with a much shorter
// timeout instead.
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

// Queries <service>.<proto>, waiting up to timeoutMs for responses. Returns the
// number of hits written into `hits` (capped at maxHits).
inline int mdnsQueryService(const char* service, const char* proto,
                            uint32_t timeoutMs, MdnsHit hits[], int maxHits) {
    mdns_result_t* results = nullptr;
    esp_err_t err = mdns_query_ptr(service, proto, timeoutMs, (size_t)maxHits, &results);
    if (err != ESP_OK || !results) return 0;

    int count = 0;
    for (mdns_result_t* r = results; r && count < maxHits; r = r->next) {
        IPAddress ip;
        for (mdns_ip_addr_t* a = r->addr; a; a = a->next) {
            if (a->addr.type == MDNS_IP_PROTOCOL_V4) {
                ip = IPAddress(a->addr.u_addr.ip4.addr);
                break;
            }
        }

        // TXT records are optional and order isn't guaranteed — scan for the
        // keys we care about. Shelly plugs carry "gen"; DustGate secondary nodes
        // carry "role"/"board"/"servos"/"owner" (see node/dustgate_node.cpp).
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

        hits[count].hostname = r->hostname ? String(r->hostname) : String();
        hits[count].ip       = ip.toString();
        hits[count].gen      = gen;
        hits[count].role     = role;
        hits[count].board    = board;
        hits[count].servos   = servos;
        hits[count].owner    = owner;
        count++;
    }
    mdns_query_results_free(results);
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

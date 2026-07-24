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
// Two service types matter for Shelly discovery:
//   _shelly._tcp  Advertised by Gen2+ devices, and ONLY by Shelly devices — a
//                 hit here is a Shelly, with no hostname guessing needed.
//   _http._tcp    Advertised by every HTTP responder on the LAN (printers,
//                 NAS, TVs...). Used only as a second pass to catch Gen1
//                 devices, which do not advertise _shelly._tcp.
//
// Gen2+ devices include a "gen" TXT key (gen=2, gen=3, ...) on BOTH services.
// Its absence on an _http._tcp hit is what identifies a device as Gen1 —
// which is why the generation no longer has to be discovered by probing.
// See https://shelly-api-docs.shelly.cloud/gen2/General/mDNS/
// =============================================================================

#pragma once
#include <Arduino.h>
#include <mdns.h>

struct MdnsHit {
    String hostname;
    String ip;
    int    gen;   // "gen" TXT value (2, 3, ...); 0 = key absent (Gen1 or not a Shelly)
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

        // TXT records are optional and order isn't guaranteed — scan for "gen".
        int gen = 0;
        for (size_t t = 0; t < r->txt_count; t++) {
            if (r->txt[t].key && r->txt[t].value && strcmp(r->txt[t].key, "gen") == 0) {
                gen = atoi(r->txt[t].value);
                break;
            }
        }

        hits[count].hostname = r->hostname ? String(r->hostname) : String();
        hits[count].ip       = ip.toString();
        hits[count].gen      = gen;
        count++;
    }
    mdns_query_results_free(results);
    return count;
}

// Shelly-only service (Gen2+). Every hit is a Shelly device.
inline int mdnsQueryShellyTcp(uint32_t timeoutMs, MdnsHit hits[], int maxHits) {
    return mdnsQueryService("_shelly", "_tcp", timeoutMs, hits, maxHits);
}

// Generic HTTP service — callers must filter; used to reach Gen1 Shellies.
inline int mdnsQueryHttpTcp(uint32_t timeoutMs, MdnsHit hits[], int maxHits) {
    return mdnsQueryService("_http", "_tcp", timeoutMs, hits, maxHits);
}

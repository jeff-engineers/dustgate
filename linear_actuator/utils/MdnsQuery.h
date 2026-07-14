// =============================================================================
// MdnsQuery.h — mDNS _http._tcp query with a short, caller-controlled timeout
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
// =============================================================================

#pragma once
#include <Arduino.h>
#include <mdns.h>

struct MdnsHit {
    String hostname;
    String ip;
};

// Queries for _http._tcp, waiting up to timeoutMs for responses. Returns the
// number of hits written into `hits` (capped at maxHits).
inline int mdnsQueryHttpTcp(uint32_t timeoutMs, MdnsHit hits[], int maxHits) {
    mdns_result_t* results = nullptr;
    esp_err_t err = mdns_query_ptr("_http", "_tcp", timeoutMs, (size_t)maxHits, &results);
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
        hits[count].hostname = r->hostname ? String(r->hostname) : String();
        hits[count].ip       = ip.toString();
        count++;
    }
    mdns_query_results_free(results);
    return count;
}

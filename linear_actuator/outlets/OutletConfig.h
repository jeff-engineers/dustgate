// =============================================================================
// OutletConfig.h — NVS-backed storage for outlet-to-stop mappings
//
// Stored in ESP32 Preferences (NVS flash) under the namespace "outlets".
// Keys are compact so they fit within the 15-char NVS key limit.
//
//   o<N>_gen    int   Shelly generation (1 or 2)
//   o<N>_ip     str   IP address ("192.168.1.x")
//   o<N>_host   str   mDNS hostname (no ".local"), empty if manually entered.
//                      Lets the outlet re-resolve its IP after a DHCP lease
//                      change instead of going silently unreachable.
//   o<N>_name   str   Display name ("Table Saw")
//   o<N>_stop   int   Stop index this outlet maps to (1-based)
//   o<N>_thr    float On-threshold in watts
//   outlet_cnt  int   Number of configured outlet slots
//
//   dc_gen      int   Dust collector plug Shelly generation (1 or 2)
//   dc_ip       str   Dust collector plug IP address
//   dc_host     str   Dust collector plug mDNS hostname (see o<N>_host above)
//
// The setup agent writes these; SmartOutletControl reads them at boot.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "../config.h"

struct OutletEntry {
    int   generation;               // 1 or 2
    char  ip[16];                   // "xxx.xxx.xxx.xxx\0"
    char  host[40];                 // mDNS hostname, empty if manually entered
    char  name[32];                 // display name
    int   stopIndex;                // 1-based stop this outlet maps to
    float thresholdW;               // watts threshold for "tool on"
    bool  valid;                    // false = slot is empty
};

// The dust collector plug is a switchable outlet (we turn it on/off), not a
// power sensor, so it has no stop/threshold — just a generation and IP.
struct DustCollectorEntry {
    int   generation;               // 1 or 2
    char  ip[16];                   // "xxx.xxx.xxx.xxx\0"
    char  host[40];                 // mDNS hostname, empty if manually entered
    bool  valid;                    // false = not configured
};

namespace OutletConfig {

    static const char* NVS_NS = "outlets";

    // Load all configured outlets from NVS. Returns the number of valid entries.
    inline int load(OutletEntry entries[], int maxEntries) {
        Preferences prefs;
        prefs.begin(NVS_NS, /*readOnly=*/true);
        int count = prefs.getInt("outlet_cnt", 0);
        count = min(count, maxEntries);

        for (int i = 0; i < count; i++) {
            char key[12];
            OutletEntry& e = entries[i];

            snprintf(key, sizeof(key), "o%d_gen",  i); e.generation = prefs.getInt(key, 1);
            snprintf(key, sizeof(key), "o%d_ip",   i); prefs.getString(key, e.ip,   sizeof(e.ip));
            e.host[0] = '\0';
            snprintf(key, sizeof(key), "o%d_host", i); prefs.getString(key, e.host, sizeof(e.host));
            snprintf(key, sizeof(key), "o%d_name", i); prefs.getString(key, e.name, sizeof(e.name));
            snprintf(key, sizeof(key), "o%d_stop", i); e.stopIndex  = prefs.getInt(key, 0);
            snprintf(key, sizeof(key), "o%d_thr",  i); e.thresholdW = prefs.getFloat(key, OUTLET_DEFAULT_THRESHOLD_W);

            // A valid entry maps to a stop; ip may be empty (name-only gate).
            e.valid = (e.stopIndex > 0);
        }

        prefs.end();
        return count;
    }

    // Save a single outlet slot to NVS.
    inline void saveSlot(int slot, const OutletEntry& e) {
        if (slot < 0 || slot >= SMART_OUTLET_COUNT) return;
        Preferences prefs;
        prefs.begin(NVS_NS, /*readOnly=*/false);

        char key[12];
        snprintf(key, sizeof(key), "o%d_gen",  slot); prefs.putInt(key,    e.generation);
        snprintf(key, sizeof(key), "o%d_ip",   slot); prefs.putString(key, e.ip);
        snprintf(key, sizeof(key), "o%d_host", slot); prefs.putString(key, e.host);
        snprintf(key, sizeof(key), "o%d_name", slot); prefs.putString(key, e.name);
        snprintf(key, sizeof(key), "o%d_stop", slot); prefs.putInt(key,    e.stopIndex);
        snprintf(key, sizeof(key), "o%d_thr",  slot); prefs.putFloat(key,  e.thresholdW);

        // Update count if this slot extends beyond the current total
        int current = prefs.getInt("outlet_cnt", 0);
        if (slot + 1 > current) prefs.putInt("outlet_cnt", slot + 1);

        prefs.end();
    }

    // Load the dust collector plug config from NVS. Returns false if none set.
    inline bool loadDustCollector(DustCollectorEntry& e) {
        e.ip[0]   = '\0';  // getString leaves buf untouched if the key is missing
        e.host[0] = '\0';
        Preferences prefs;
        prefs.begin(NVS_NS, /*readOnly=*/true);
        e.generation = prefs.getInt("dc_gen", 2);
        prefs.getString("dc_ip", e.ip, sizeof(e.ip));
        prefs.getString("dc_host", e.host, sizeof(e.host));
        prefs.end();
        // A hostname alone is enough: the outlet resolves its own address on
        // first poll, so a DHCP plug paired by mDNS name needs no static IP.
        e.valid = (strlen(e.ip) > 0 || strlen(e.host) > 0);
        return e.valid;
    }

    // Save the dust collector plug config to NVS.
    inline void saveDustCollector(const DustCollectorEntry& e) {
        Preferences prefs;
        prefs.begin(NVS_NS, /*readOnly=*/false);
        prefs.putInt("dc_gen", e.generation);
        prefs.putString("dc_ip", e.ip);
        prefs.putString("dc_host", e.host);
        prefs.end();
    }

    // Clear the dust collector plug config from NVS.
    inline void eraseDustCollector() {
        Preferences prefs;
        prefs.begin(NVS_NS, /*readOnly=*/false);
        prefs.remove("dc_gen");
        prefs.remove("dc_ip");
        prefs.remove("dc_host");
        prefs.end();
    }

    // Erase all outlet config from NVS.
    inline void erase() {
        Preferences prefs;
        prefs.begin(NVS_NS, false);
        prefs.clear();
        prefs.end();
    }

    // Print all stored config to Serial (for debugging / setup agent verification).
    inline void print(const OutletEntry entries[], int count) {
        Serial.println(F("--- Outlet Config ---"));
        for (int i = 0; i < count; i++) {
            const OutletEntry& e = entries[i];
            if (!e.valid) continue;
            Serial.print(F("  [")); Serial.print(i); Serial.print(F("] "));
            Serial.print(e.name);
            Serial.print(F("  gen=")); Serial.print(e.generation);
            Serial.print(F("  ip="));  Serial.print(e.ip);
            Serial.print(F("  stop=")); Serial.print(e.stopIndex);
            Serial.print(F("  thr=")); Serial.print(e.thresholdW, 1); Serial.println(F("W"));
        }
        Serial.println(F("---------------------"));
    }

} // namespace OutletConfig

// =============================================================================
// OutletConfig.h — NVS-backed storage for outlet-to-stop mappings
//
// Stored in ESP32 Preferences (NVS flash) under the namespace "outlets".
// Keys are compact so they fit within the 15-char NVS key limit.
//
//   o<N>_gen    int   Shelly generation (1 or 2)
//   o<N>_ip     str   IP address ("192.168.1.x")
//   o<N>_name   str   Display name ("Table Saw")
//   o<N>_stop   int   Stop index this outlet maps to (1-based)
//   o<N>_thr    float On-threshold in watts
//   outlet_cnt  int   Number of configured outlet slots
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
    char  name[32];                 // display name
    int   stopIndex;                // 1-based stop this outlet maps to
    float thresholdW;               // watts threshold for "tool on"
    bool  valid;                    // false = slot is empty
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
            snprintf(key, sizeof(key), "o%d_name", i); prefs.getString(key, e.name, sizeof(e.name));
            snprintf(key, sizeof(key), "o%d_stop", i); e.stopIndex  = prefs.getInt(key, 0);
            snprintf(key, sizeof(key), "o%d_thr",  i); e.thresholdW = prefs.getFloat(key, OUTLET_DEFAULT_THRESHOLD_W);

            e.valid = (strlen(e.ip) > 0 && e.stopIndex > 0);
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
        snprintf(key, sizeof(key), "o%d_name", slot); prefs.putString(key, e.name);
        snprintf(key, sizeof(key), "o%d_stop", slot); prefs.putInt(key,    e.stopIndex);
        snprintf(key, sizeof(key), "o%d_thr",  slot); prefs.putFloat(key,  e.thresholdW);

        // Update count if this slot extends beyond the current total
        int current = prefs.getInt("outlet_cnt", 0);
        if (slot + 1 > current) prefs.putInt("outlet_cnt", slot + 1);

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

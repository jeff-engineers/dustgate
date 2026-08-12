// =============================================================================
// Watchdog.h — main-loop task watchdog, portable across Arduino-ESP32 cores.
//
// Both programs arm a task watchdog on their main loop so an unattended hang
// (a wedged HTTP handler, a servo sweep that never settles) reboots the board
// instead of leaving a shop with gates stuck wherever they were.
//
// This header exists for one reason: IDF 5 changed esp_task_wdt_init()'s
// signature from (timeout_seconds, panic_bool) to a config struct, so the same
// call cannot compile on both core lines. The repo currently straddles them —
// the four supported targets are on Arduino core 2.0.x, and the ESP32-C5 spike
// is on 3.x/IDF 5 because the part does not exist in the older core (see the
// xiao_c5 env in platformio.ini). Rather than let that difference leak into two
// sketches, it is resolved once, here.
//
// If every target eventually moves to the newer platform, delete the #else
// branch and this file becomes a two-line convenience.
// =============================================================================
#pragma once

#include <Arduino.h>
#include <esp_task_wdt.h>
#include "../config.h"

namespace watchdog {

// Arm the watchdog and subscribe the CALLING task to it. Call from setup(), on
// whichever task owns the main loop.
inline void begin() {
#if ESP_IDF_VERSION_MAJOR >= 5
    // idle_core_mask 0: watch this task only, not the idle tasks. Watching idle
    // would fire on any board that legitimately keeps a core busy, which is not
    // the failure this is here to catch.
    const esp_task_wdt_config_t cfg = {
        .timeout_ms     = (uint32_t)WDT_TIMEOUT_SEC * 1000U,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_init(&cfg);
#else
    esp_task_wdt_init(WDT_TIMEOUT_SEC, /*panic=*/true);
#endif
    esp_task_wdt_add(NULL);
}

// Pet it. Call once per loop() pass, unconditionally — the point is to prove
// the loop is still turning, so anything that could skip this is a bug.
inline void pet() { esp_task_wdt_reset(); }

} // namespace watchdog

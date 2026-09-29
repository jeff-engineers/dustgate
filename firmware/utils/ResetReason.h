// =============================================================================
// utils/ResetReason.h — why this chip last reset, as one short word.
//
// Used in two places that need the SAME vocabulary: a node's WELCOME (`rst`, see
// withBootInfo() in nodelink.js) and the primary's own boot entry in its link log
// (utils/LinkLog.h). One word rather than the enum's number because it ends up in
// a log a person reads, and ≤ kMaxRstLen (16) because the WELCOME validator
// refuses anything longer.
//
// The distinction that matters on a shop floor is POWER vs CRASH: "poweron" or
// "brownout" on a CT node is the tool being switched off at the wall (RFC §5.6a),
// which is normal; "panic" / "task_wdt" / "int_wdt" is a bug in our code.
// =============================================================================
#pragma once
#include <esp_system.h>

namespace resetreason {

inline const char* word(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "ext";
        case ESP_RST_SW:        return "sw";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        // USB covers the USB-Serial/JTAG reset — i.e. a flash or a monitor
        // toggling the lines, which on a bench is by far the most common reason.
        case ESP_RST_USB:       return "usb";
        case ESP_RST_JTAG:      return "jtag";
        // The C5's list is longer than the classic ESP32's, and these three were
        // missing until 2026-09-29 — so a node resetting on a POWER GLITCH (a
        // servo dragging its supply down) reported "unknown" and read as a
        // mystery rather than as the power fault it is.
        case ESP_RST_EFUSE:      return "efuse";
        case ESP_RST_PWR_GLITCH: return "pwr_glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
        default:                return "unknown";
    }
}

inline const char* now() { return word(esp_reset_reason()); }

} // namespace resetreason

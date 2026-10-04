// =============================================================================
// utils/OtaGuard.h — a freshly installed image is on PROBATION until it proves it
// can do its job.
//
// WHY (2026-10-03). The SDK is built with app rollback ENABLED, which means the
// bootloader boots a just-written OTA image as "pending verify" and, if the board
// resets before the image calls esp_ota_mark_app_valid_cancel_rollback(), goes back
// to the previous slot. The Arduino core normally marks valid at once, in
// initArduino(), which would make that safety net decorative: an image that boots
// and then cannot join WiFi would be "valid" for ever. verifyRollbackLater() here
// (a strong symbol over the core's weak one) takes that decision out of the core's
// hands, and tick() makes it with a definition of "working" each ROLE supplies:
//
//   primary  WiFi associated, with the HTTP server up
//   node     WiFi associated AND the primary has completed a HELLO/WELCOME
//
// Healthy for kProbationHealthyMs  → mark valid, rollback cancelled.
// Not valid by kProbationMaxMs     → mark INVALID and reboot into the old slot.
//
// The second rule matters as much as the first: a bad image that boots but hangs
// would otherwise sit on probation for ever, because the bootloader only rolls
// back on a RESET, and nothing resets a board that is merely useless.
//
// A cable flash is never on probation (state "undefined"), and a board whose
// partition table has no second slot reports hasSlots() == false so the OTA
// routes can say so in a sentence instead of failing inside Update.
// =============================================================================
#pragma once

namespace otaguard {

constexpr unsigned long kProbationHealthyMs = 30UL * 1000;   // working this long → believed
constexpr unsigned long kProbationMaxMs     = 180UL * 1000;  // not believed by now → roll back

// True when the partition table has a slot to write an update into.
bool hasSlots();

// True when THIS boot is a fresh OTA image that has not been marked valid yet.
bool onProbation();

// "valid" (marked, or never needed marking), "probation", or "nogo" for a
// board with no second slot. For /api/info and the node's WELCOME.
const char* state();

// Which slot is running: "app0" / "app1" / "factory".
const char* slot();

// Call from loop(). `healthy` is the ROLE's own answer to "is this working?".
void tick(bool healthy);

} // namespace otaguard

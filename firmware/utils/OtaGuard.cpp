// utils/OtaGuard.cpp — see OtaGuard.h.
#include "OtaGuard.h"
#include <Arduino.h>
#include <esp_ota_ops.h>

// The Arduino core marks an image valid inside initArduino() unless this says
// otherwise. Ours says: not yet — tick() decides.
extern "C" bool verifyRollbackLater() { return true; }   // C linkage: the core's is a weak C symbol

namespace otaguard {

bool hasSlots() {
    return esp_ota_get_next_update_partition(nullptr) != nullptr &&
           esp_ota_get_next_update_partition(nullptr) != esp_ota_get_running_partition();
}

static esp_ota_img_states_t imgState() {
    esp_ota_img_states_t s = ESP_OTA_IMG_UNDEFINED;
    const esp_partition_t* run = esp_ota_get_running_partition();
    if (run) esp_ota_get_state_partition(run, &s);
    return s;
}

bool onProbation() { return imgState() == ESP_OTA_IMG_PENDING_VERIFY; }

const char* state() {
    if (!hasSlots()) return "nogo";
    return onProbation() ? "probation" : "valid";
}

const char* slot() {
    const esp_partition_t* run = esp_ota_get_running_partition();
    return run ? run->label : "?";
}

void tick(bool healthy) {
    static bool decided = false;          // valid, or rolled back: nothing more to do
    static unsigned long healthySince = 0;
    static bool wasHealthy = false;
    if (decided) return;
    if (!onProbation()) { decided = true; return; }

    const unsigned long now = millis();
    if (healthy) {
        if (!wasHealthy) { wasHealthy = true; healthySince = now; }
        if (now - healthySince >= kProbationHealthyMs) {
            esp_ota_mark_app_valid_cancel_rollback();
            decided = true;
            Serial.printf("[OTA] %s healthy for %lu s — marked valid, rollback cancelled\n",
                          slot(), kProbationHealthyMs / 1000);
        }
    } else {
        wasHealthy = false;
    }
    if (!decided && now >= kProbationMaxMs) {
        Serial.printf("[OTA] %s never became healthy in %lu s — rolling back\n",
                      slot(), kProbationMaxMs / 1000);
        Serial.flush();
        esp_ota_mark_app_invalid_rollback_and_reboot();   // does not return on success
    }
}

} // namespace otaguard

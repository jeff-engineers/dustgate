// =============================================================================
// CalibrationStore.cpp
// =============================================================================

#include "CalibrationStore.h"
#include "../utils/MotionMath.h"   // stepsPerMM() — the board's own scale

// -----------------------------------------------------------------------------
// CRC-16/CCITT-FALSE over all fields except the crc field itself
// -----------------------------------------------------------------------------
uint16_t CalibrationStore::computeCRC(const CalibrationData& data) {
    uint16_t crc = 0xFFFF;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&data);
    size_t len = sizeof(CalibrationData) - sizeof(uint16_t); // exclude crc field

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)bytes[i] << 8;
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
    }
    return crc;
}

// -----------------------------------------------------------------------------
void CalibrationStore::begin() {
    // ESP32: EEPROM is flash-backed and requires begin() before any access.
    // Allocate enough space for our struct plus a small margin.
    EEPROM.begin(CALIB_EEPROM_SIZE);
}

// -----------------------------------------------------------------------------
void CalibrationStore::save(const CalibrationData& dataIn) {
    CalibrationData data = dataIn;
    data.magic   = CALIB_MAGIC;
    data.version = CALIB_VERSION;
    data.crc     = computeCRC(data);

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&data);
    for (size_t i = 0; i < sizeof(CalibrationData); i++) {
        EEPROM.write(CALIB_ADDRESS + i, bytes[i]);
    }
    EEPROM.commit(); // Required on ESP32 to flush buffer to flash
    Serial.println(F("[CAL] Calibration saved to EEPROM."));
}

// -----------------------------------------------------------------------------
bool CalibrationStore::load(CalibrationData& data) {
    uint8_t* bytes = reinterpret_cast<uint8_t*>(&data);
    for (size_t i = 0; i < sizeof(CalibrationData); i++) {
        bytes[i] = EEPROM.read(CALIB_ADDRESS + i);
    }

    if (data.magic != CALIB_MAGIC) return false;
    if (data.version != CALIB_VERSION) return false;
    if (computeCRC(data) != data.crc) return false;
    return true;
}

// -----------------------------------------------------------------------------
bool CalibrationStore::isValid() {
    CalibrationData tmp;
    return load(tmp);
}

// -----------------------------------------------------------------------------
void CalibrationStore::erase() {
    EEPROM.write(CALIB_ADDRESS,     0x00);
    EEPROM.write(CALIB_ADDRESS + 1, 0x00);
    EEPROM.commit(); // Flush to flash on ESP32
    Serial.println(F("[CAL] EEPROM calibration erased."));
}

// -----------------------------------------------------------------------------
void CalibrationStore::print(const CalibrationData& data) {
    // The scale the BOARD believes in, from utils/MotionMath.h. It used to be
    // recomputed here from the stepper's own gear numbers, which stopped being
    // anything once the rack became a bus servo — and a second expression for
    // one quantity is a second place for it to disagree.
    const float theoreticalSPMM = stepsPerMM();

    Serial.println(F(""));
    Serial.println(F("=== Calibration Data ==="));
    Serial.print(F("  Stops trained:       ")); Serial.println(data.numStops);
    Serial.print(F("  Max travel:          ")); Serial.print(data.maxTravelMM, 2); Serial.println(F(" mm"));
    Serial.print(F("  Steps/mm (measured): ")); Serial.println(data.measuredStepsPerMM, 3);
    Serial.print(F("  Steps/mm (config):   ")); Serial.println(theoreticalSPMM, 3);
    Serial.print(F("  Error:               "));
    float err = ((data.measuredStepsPerMM - theoreticalSPMM) / theoreticalSPMM) * 100.0f;
    Serial.print(err, 2); Serial.println(F("%"));
    Serial.println(F(""));
    Serial.println(F("  Stop   Position(mm)   Gap to prev(mm)"));
    Serial.println(F("  -----  -----------    ---------------"));
    for (int i = 0; i <= (int)data.numStops; i++) {
        Serial.print(F("  "));
        if (i < 10) Serial.print(F(" "));
        Serial.print(i);
        Serial.print(F("      "));
        if (data.stopMM[i] < 100.0f) Serial.print(F(" "));
        if (data.stopMM[i] < 10.0f)  Serial.print(F(" "));
        Serial.print(data.stopMM[i], 2);
        if (i > 0) {
            float gap = data.stopMM[i] - data.stopMM[i-1];
            Serial.print(F("          "));
            Serial.print(gap, 2);
        }
        Serial.println();
    }
    Serial.println(F("========================"));
}

// -----------------------------------------------------------------------------
void CalibrationStore::printConfigSnippet(const CalibrationData& data) {
    Serial.println(F(""));
    Serial.println(F("// === Paste into config.h ==="));
    Serial.print(F("#define STOP_DISTANCES_MM { "));
    for (int i = 0; i <= (int)data.numStops; i++) {
        Serial.print(data.stopMM[i], 2);
        Serial.print(F("f"));
        if (i < (int)data.numStops) Serial.print(F(", "));
    }
    Serial.println(F(" }"));
    Serial.println(F(""));
    Serial.print(F("// Measured steps/mm: "));
    Serial.println(data.measuredStepsPerMM, 3);
    // The old snippet here suggested a STEPS_PER_REV × MICROSTEPS pair that would
    // reproduce the measured scale. There is nothing to suggest on a bus servo:
    // its counts/rev is fixed at 4096 and the only adjustable term is the
    // GEOMETRY, so a scale that disagrees means the rack or the pinion is not
    // what config.h says it is.
    Serial.print(F("// Nominal is "));
    Serial.print(stepsPerMM(), 3);
    Serial.println(F(" — if the measured value disagrees, check the pinion tooth"));
    Serial.println(F("//   count and the rack pitch against wiring/st3215-bench.md §5.0.3."));
    Serial.println(F("// ============================"));
}

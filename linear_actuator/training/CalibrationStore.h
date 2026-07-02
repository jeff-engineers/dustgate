// =============================================================================
// CalibrationStore.h — EEPROM persistence for trained stop positions
//
// Layout (starting at EEPROM address 0):
//   [magic 2B][version 1B][numStops 1B][stopMM 8×4B][maxTravelMM 4B]
//   [measuredStepsPerMM 4B][crc 2B]
//   Total: ~46 bytes
//
// ESP32 note: EEPROM is emulated in flash. Call CalibrationStore::begin()
// once at startup before any load/save calls (done in setup() via #include <EEPROM.h>).
//
// At runtime: if EEPROM contains valid data it overrides STOP_DISTANCES_MM
// from config.h. This means you can retrain without recompiling.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <EEPROM.h>
#include "../config.h"

static const uint16_t CALIB_MAGIC   = 0xCA1B;
static const uint8_t  CALIB_VERSION = 1;
static const int      CALIB_ADDRESS = 0;    // EEPROM start address

struct CalibrationData {
    uint16_t magic;
    uint8_t  version;
    uint8_t  numStops;                  // number of trained intermediate stops (1–7)
    float    stopMM[NUM_STOPS + 1];     // index 0 = home = 0.0, 1–N = measured mm
    float    maxTravelMM;               // distance to far physical endstop
    float    measuredStepsPerMM;        // derived from endstop-to-endstop travel
    uint16_t crc;
};

// EEPROM size needed (add margin for future growth)
static const int CALIB_EEPROM_SIZE = 64;

class CalibrationStore {
public:
    // Must be called once before any load/save (calls EEPROM.begin on ESP32)
    static void begin();

    // Save data to EEPROM (computes CRC before writing)
    static void save(const CalibrationData& data);

    // Load from EEPROM into data. Returns true if magic + CRC valid.
    static bool load(CalibrationData& data);

    // True if EEPROM currently contains valid calibration
    static bool isValid();

    // Erase calibration (overwrites magic bytes)
    static void erase();

    // Print loaded calibration to Serial in human-readable form
    static void print(const CalibrationData& data);

    // Print a ready-to-paste config.h snippet
    static void printConfigSnippet(const CalibrationData& data);

private:
    static uint16_t computeCRC(const CalibrationData& data);
};

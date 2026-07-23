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
static const uint8_t  CALIB_VERSION = 4;     // v4: homeIsMaxEndstop (left = home datum)
static const int      CALIB_ADDRESS = 0;    // EEPROM start address

// Port roles — mirror shared/device-model PORT_ROLES. Stored per stop so a
// blocked/feed port survives reboot. Index 0 (home) is always ROLE_HOME.
enum PortRole : uint8_t {
    ROLE_TOOL = 0,
    ROLE_UNASSIGNED = 1,
    ROLE_BLOCKED = 2,
    ROLE_FEED = 3,        // reserved for v2 (feeds a downstream cluster)
    ROLE_HOME = 4,
};

struct CalibrationData {
    uint16_t magic;
    uint8_t  version;
    uint8_t  numStops;                  // number of trained intermediate stops (1–N)
    float    stopMM[NUM_STOPS + 1];     // index 0 = home = 0.0, 1–N = measured mm
    float    maxTravelMM;               // measured span in mm (near→far endstop)
    float    measuredStepsPerMM;        // derived from endstop-to-endstop travel
    uint8_t  stopRole[NUM_STOPS + 1];   // PortRole per stop (v2 layout)
    char     manifoldModel[16];         // "rockler-2.5" | "rockler-4" | "custom"
    uint8_t  homeIsMaxEndstop;          // which endstop is the HOME datum (= the user's
                                        //   LEFT). 0 = D10/PIN_ENDSTOP_HOME, 1 = D11/
                                        //   PIN_ENDSTOP_MAX. Set during first-home setup
                                        //   so the carriage always homes to the left end.
    uint16_t crc;
};

// EEPROM size needed.
// With NUM_STOPS=16 (v2): 2+1+1+(17×4)+4+4+(17×1)+16+2 = 115 bytes; 128 leaves margin.
// NOTE: changing NUM_STOPS or the struct layout invalidates the CRC/version of any
// existing cal data — run clearcal after reflash (the version bump forces this too).
static const int CALIB_EEPROM_SIZE = 128;

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

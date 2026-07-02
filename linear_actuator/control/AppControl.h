// =============================================================================
// AppControl.h — Smartphone app control (STUB)
//
// Concept: A BLE or WiFi-connected smartphone app sends position commands.
// Hardware options:
//   - BLE: HC-08, HM-10, or nRF52 module (UART bridge to Arduino)
//   - WiFi: ESP8266/ESP32 as co-processor or drop-in replacement for Arduino
// Protocol: simple ASCII commands over UART ("POS:3\n", "ENABLE\n", etc.)
//
// This stub reads commands from Serial (for testing) and will be extended
// with actual wireless hardware.
// =============================================================================

#pragma once
#include "ControlInput.h"
#include "../config.h"

#ifdef CONTROL_APP

class AppControl : public ControlInput {
public:
    AppControl();
    bool begin() override;
    int readRequestedStop() override;
    bool isEnabled() override;

    // Call from loop() to process incoming serial/BLE data
    void processIncoming();

private:
    int _requestedStop;
    bool _enabled;

    // TODO: replace with hardware serial/BLE stream when module is selected
    // HardwareSerial* _bleSerial;

    void parseCommand(const String& cmd);
};

#endif // CONTROL_APP

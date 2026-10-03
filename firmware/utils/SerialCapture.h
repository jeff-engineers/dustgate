// =============================================================================
// utils/SerialCapture.h — make every `Serial` in the primary's sources a tee into
// utils/SerialLog.h, without editing the ~1,100 call sites.
//
// Force-included (`-include`) by the PRIMARY envs only, through build_src_flags
// so libraries compile untouched. On this core `Serial` is already a macro for
// HWCDCSerial (HardwareSerial.h, ARDUINO_USB_CDC_ON_BOOT); including Arduino.h
// first means that definition is settled before this one replaces it, and the
// include guards stop it coming back.
//
// A node does not get this: nothing serves its log yet (docs/mockups/brain-log.html,
// question 2), and a node build that doesn't compile SerialLog.cpp would fail to link.
// =============================================================================
#pragma once
#ifdef __cplusplus
#include <Arduino.h>

class SerialTee : public Stream {
public:
    void begin(unsigned long baud);
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buf, size_t len) override;
    int available() override;
    int read() override;
    int peek() override;
    void flush() override;
    int availableForWrite() override;
    explicit operator bool() const;
    using Print::write;
};

extern SerialTee g_serialTee;

#undef Serial
#define Serial g_serialTee
#endif

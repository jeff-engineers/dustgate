// =============================================================================
// ST3215Bus.h — the wire protocol for Feetech serial bus servos (ST/STS/SCS)
//
// Hand-rolled on purpose. The vendor libraries are AVR/Xtensa and lean on
// SoftwareSerial; the protocol itself is a dozen lines:
//
//     FF FF  ID  LEN  INSTR  [params...]  CHKSUM
//
// LEN counts everything after it including the checksum (params + 2). CHKSUM is
// ~(ID + LEN + INSTR + params) & 0xFF. A reply is the same shape with the
// instruction byte replaced by an error byte.
//
// ONE WIRE, HALF DUPLEX. TX and RX both sit on the servo's single signal line,
// so every frame we send arrives back on RX before the servo answers. This
// drains that echo in software rather than trusting the IDF's RS485 mode — the
// mode may well work on the C5, but "may well" is not something to find out
// through a servo that is bolted to a gate. `traceOn()` prints both directions
// as hex, which is the only way to tell a silent bus from a wrong baud from a
// servo that is answering something you didn't mean to ask.
//
// ⚠️ NOTHING HERE HAS TALKED TO A SERVO YET. The frame shape is solid; the
// REGISTER MAP below is from the STS/ST series docs and is the part most likely
// to be wrong for a given part — which is why the bench program can read raw
// addresses and flip endianness without a reflash. Confirm, then trust.
// =============================================================================
#pragma once
#include <Arduino.h>

// -----------------------------------------------------------------------------
// Instructions
// -----------------------------------------------------------------------------
enum : uint8_t {
    ST_PING       = 0x01,
    ST_READ       = 0x02,
    ST_WRITE      = 0x03,
    ST_REG_WRITE  = 0x04,   // staged write, fires on ACTION — how you move several at once
    ST_ACTION     = 0x05,
    ST_RESET      = 0x06,   // factory reset: takes the ID with it. Not exposed at the bench.
    ST_SYNC_WRITE = 0x83,
};

// -----------------------------------------------------------------------------
// Register map (STS/ST series). EEPROM below 40, RAM from 40 up.
//
// UNVERIFIED against the part on the bench. Read them raw first (`dump`), and
// if a number here turns out wrong, fix it HERE — the bench program and the
// eventual MotorDriver both read this map.
// -----------------------------------------------------------------------------
enum : uint8_t {
    ST_REG_ID              = 5,    // EEPROM, 1 byte
    ST_REG_BAUD            = 6,    // EEPROM, 1 byte (index, not a baud)
    ST_REG_MIN_ANGLE       = 9,    // EEPROM, 2 bytes — both 0 means multi-turn
    ST_REG_MAX_ANGLE       = 11,   // EEPROM, 2 bytes
    ST_REG_MODE            = 33,   // EEPROM, 1 byte: 0 = position, 1 = wheel, 2 = PWM, 3 = step
    ST_REG_TORQUE_ENABLE   = 40,
    ST_REG_ACCELERATION    = 41,
    ST_REG_GOAL_POSITION   = 42,   // 2 bytes, 0..4095 over one turn in mode 0
    ST_REG_GOAL_TIME       = 44,   // 2 bytes
    ST_REG_GOAL_SPEED      = 46,   // 2 bytes, steps/s
    ST_REG_LOCK            = 55,   // 1 = EEPROM write-protected
    ST_REG_PRESENT_POS     = 56,   // 2 bytes
    ST_REG_PRESENT_SPEED   = 58,   // 2 bytes, sign in bit 15
    ST_REG_PRESENT_LOAD    = 60,   // 2 bytes, sign in bit 15
    ST_REG_PRESENT_VOLTAGE = 62,   // 1 byte, tenths of a volt
    ST_REG_PRESENT_TEMP    = 63,   // 1 byte, °C
    ST_REG_MOVING          = 66,   // 1 byte
    ST_REG_PRESENT_CURRENT = 69,   // 2 bytes
};

/** Mode 0's position range, one full turn. */
static const uint16_t ST_POS_MAX = 4095;

/** The factory baud. Everything else is a register-6 index, not a number. */
static const uint32_t ST_DEFAULT_BAUD = 1000000;

class ST3215Bus {
public:
    /**
     * Open the UART on the board's bus pins. `baud` is a real baud rate, not
     * the servo's register-6 index — talking to a servo means matching whatever
     * IT was last set to, which is why the bench can re-open at another rate.
     */
    bool begin(uint32_t baud = ST_DEFAULT_BAUD);
    uint32_t baud() const { return _baud; }

    /** Both directions as hex, to Serial. Off by default; the bench turns it on. */
    void trace(bool on) { _trace = on; }
    bool tracing() const { return _trace; }

    /**
     * Byte order for 16-bit registers. ST/STS is little-endian, SCS is big —
     * and a servo sold as one has been known to behave like the other, so this
     * is a runtime switch and the bench program can flip it in a second.
     */
    void littleEndian(bool on) { _little = on; }
    bool isLittleEndian() const { return _little; }

    /** Is anything answering to this id? `err` gets the servo's status byte. */
    bool ping(uint8_t id, uint8_t* err = nullptr);

    bool readRegs(uint8_t id, uint8_t addr, uint8_t len, uint8_t* out, uint8_t* err = nullptr);
    bool writeRegs(uint8_t id, uint8_t addr, const uint8_t* data, uint8_t len, uint8_t* err = nullptr);

    bool read8(uint8_t id, uint8_t addr, uint8_t* out);
    bool read16(uint8_t id, uint8_t addr, uint16_t* out);
    bool write8(uint8_t id, uint8_t addr, uint8_t value);
    bool write16(uint8_t id, uint8_t addr, uint16_t value);

    /** Last transaction's failure, in words. "" when the last one worked. */
    const char* lastError() const { return _lastError; }

    /**
     * A broadcast write nothing can answer — used only where a reply would be a
     * collision (id 254). Returns as soon as the bytes are out.
     */
    bool broadcastWrite(uint8_t addr, const uint8_t* data, uint8_t len);

private:
    bool  send(uint8_t id, uint8_t instr, const uint8_t* params, uint8_t len);
    bool  receive(uint8_t id, uint8_t* params, uint8_t maxParams, uint8_t* got, uint8_t* err);
    void  drainEcho(uint8_t sentBytes);
    void  hexdump(const char* dir, const uint8_t* buf, uint8_t len);

    uint32_t    _baud       = 0;
    bool        _trace      = false;
    bool        _little     = true;
    const char* _lastError  = "";
    // A whole reply is ~10 bytes; 32 is room for a generous raw dump.
    static const uint8_t kMaxParams = 32;
};

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
// ONE WIRE, HALF DUPLEX — AND TWO WAYS TO WIRE IT. With TX and RX tied to the
// signal line through a resistor, every frame we send comes straight back at us;
// with a buffered adapter (Seeed's XIAO Bus Servo Adapter, or a 74LVC1G125 and a
// direction pin) something turns the line around and it never does. This works
// on both: it reads FRAMES and drops any frame identical to the one just sent,
// rather than counting bytes it assumes are an echo. `trace(true)` prints both
// directions as hex, labelling the echo when there is one — which is the only
// way to tell a silent bus from a wrong baud from a servo answering something
// you didn't mean to ask.
//
// TALKED TO A REAL SERVO 2026-08-26: ping, status byte, register reads and a
// move, at 1 Mbps through Seeed's Bus Servo Driver Board. The register map
// below was the part most likely to be wrong for a given part, and it is now
// confirmed for THIS one — `read` reported 12.1 V against a meter's 12 V, which
// is not a number a wrong address or byte order produces by accident. The bench
// program can still read raw addresses and flip endianness without a reflash;
// the next part off a different supplier may still need it.
// =============================================================================
#pragma once
#include <Arduino.h>
#include "../../config.h"      // the board's PIN_SERVO_BUS_* — see txPin()/rxPin()

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
    // Confirmed on the bench for the part in hand (2026-08-26): PRESENT_POS,
    // PRESENT_VOLTAGE, TORQUE_ENABLE, GOAL_POSITION and GOAL_SPEED all behave.
    // The rest are still docs.
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
     *
     * `swapPins` puts TX on the RX pad and vice versa. It exists because a
     * silent bus cannot tell you WHY it is silent, and "the adapter expects the
     * other order" is one of the three reasons — cheaper to try than to trace.
     */
    bool begin(uint32_t baud = ST_DEFAULT_BAUD, bool swapPins = false);
    uint32_t baud() const { return _baud; }
    bool swapped() const { return _swapped; }
    int  txPin() const { return _swapped ? PIN_SERVO_BUS_RX : PIN_SERVO_BUS_TX; }
    int  rxPin() const { return _swapped ? PIN_SERVO_BUS_TX : PIN_SERVO_BUS_RX; }

    /** Both directions as hex, to Serial. Off by default; the bench turns it on. */
    void trace(bool on) { _trace = on; }
    bool tracing() const { return _trace; }

    /**
     * Has this bus ever heard its own transmission come back?
     *
     * TRUE means TX and RX are on the one wire (a series resistor, no buffer);
     * FALSE means something is turning the line around for us — an adapter with
     * a direction driver. Neither is wrong, and the protocol above works on
     * both; this is here because "which one am I actually on" is a question a
     * bench session asks in its first minute.
     */
    bool echoSeen() const { return _echoSeen; }
    void clearEchoSeen() { _echoSeen = false; }

    /**
     * Wire the UART's own TX to its own RX, inside the chip.
     *
     * The one test that needs no servo, no adapter and no wire: with this on, a
     * frame we send must come back to us. If it does, the peripheral, the pin
     * mapping's existence, the baud and this file's framing are all fine and the
     * fault is outside the chip. If it does NOT, nothing beyond this point is
     * worth debugging. `selftest` at the bench drives it.
     */
    bool loopback(bool on);

    /**
     * Write ping frames back to back for `ms`, never waiting for a reply.
     *
     * For a METER, not for a servo. One ping every reply-timeout leaves the line
     * idle ~99% of the time and a DMM reads a flat idle-high — which is exactly
     * what a dead UART reads too. Back to back, the average drops far enough to
     * see, so "is anything coming out of this pad" becomes a question a $20
     * meter can answer.
     */
    void stream(uint8_t id, uint32_t ms);

    /**
     * Byte order for 16-bit registers. ST/STS is little-endian, SCS is big —
     * and a servo sold as one has been known to behave like the other, so this
     * is a runtime switch and the bench program can flip it in a second.
     */
    void littleEndian(bool on) { _little = on; }
    bool isLittleEndian() const { return _little; }

    /**
     * Position, time and speed as ONE write at register 42 — the canonical
     * Feetech move command.
     *
     * Writing GOAL_SPEED on its own and then GOAL_POSITION does not work: the
     * servo ran every move at its maximum no matter what 46 had been set to
     * (measured 2026-08-26 — asked 300, got 1599; asked 1200, got 1646; the
     * ceiling being 1630). Speed is latched when the block lands, so it has to
     * travel with the position that it applies to.
     *
     * `time` is the alternative to speed — a duration for the move — and 0
     * leaves speed in charge. Nothing here has tested it.
     */
    bool moveTo(uint8_t id, uint16_t pos, uint16_t speed, uint16_t time = 0);

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
    void  hexdump(const char* dir, const uint8_t* buf, uint8_t len);

    uint32_t    _baud       = 0;
    bool        _trace      = false;
    bool        _little     = true;
    bool        _echoSeen   = false;
    bool        _swapped    = false;
    const char* _lastError  = "";
    // A whole reply is ~10 bytes; 32 is room for a generous raw dump.
    static const uint8_t kMaxParams = 32;
    // The last frame sent, kept so a bounced copy of it can be recognised and
    // dropped — see receive().
    uint8_t     _sent[6 + kMaxParams];
    uint8_t     _sentLen    = 0;
};

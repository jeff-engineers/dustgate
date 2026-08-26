// =============================================================================
// ST3215Bus.cpp — see ST3215Bus.h for the frame shape and the warnings.
// =============================================================================
#include "ST3215Bus.h"
#include "../../config.h"

#if !defined(PIN_SERVO_BUS_TX) || !defined(PIN_SERVO_BUS_RX)
  #error "ST3215Bus needs PIN_SERVO_BUS_TX/RX — build with -DDUSTGATE_SERVO_BUS"
#endif

// Every read is bounded. WDT_TIMEOUT_SEC is 10 and only loop() pets it, so a
// silent bus must never be able to park us here: at 1 Mbps a reply is ~100 µs,
// and a servo that has not started answering in 20 ms is not going to.
static const uint32_t kReplyTimeoutMs = 20;

// The UART. HardwareSerial 1 — Serial0 is the USB CDC console on this part.
#define BUS Serial1

bool ST3215Bus::begin(uint32_t baud) {
    _baud = baud;
    BUS.end();
    BUS.begin(baud, SERIAL_8N1, PIN_SERVO_BUS_RX, PIN_SERVO_BUS_TX);
    // Anything the line collected while we were not listening is not a reply.
    delay(2);
    while (BUS.available()) BUS.read();
    return true;
}

void ST3215Bus::hexdump(const char* dir, const uint8_t* buf, uint8_t len) {
    Serial.printf("  %s", dir);
    for (uint8_t i = 0; i < len; i++) Serial.printf(" %02X", buf[i]);
    Serial.println();
}

bool ST3215Bus::send(uint8_t id, uint8_t instr, const uint8_t* params, uint8_t len) {
    uint8_t frame[6 + kMaxParams];
    uint8_t n = 0;
    frame[n++] = 0xFF;
    frame[n++] = 0xFF;
    frame[n++] = id;
    frame[n++] = (uint8_t)(len + 2);      // params + instruction + checksum
    frame[n++] = instr;
    for (uint8_t i = 0; i < len; i++) frame[n++] = params[i];

    uint16_t sum = 0;
    for (uint8_t i = 2; i < n; i++) sum += frame[i];   // from ID through the last param
    frame[n++] = (uint8_t)(~sum & 0xFF);

    // Nothing on the line is ours until this frame is: a stale byte from a
    // half-heard reply would otherwise be read back as part of this one.
    while (BUS.available()) BUS.read();

    if (_trace) hexdump("TX", frame, n);
    BUS.write(frame, n);
    BUS.flush();                                        // wait for the last bit out

    // Remember it verbatim. Whether it comes back depends on hardware we do not
    // control (see receive()), so the only safe way to recognise our own echo is
    // to compare against the bytes themselves.
    for (uint8_t i = 0; i < n; i++) _sent[i] = frame[i];
    _sentLen = n;
    return true;
}

/**
 * Read one reply frame addressed to `id`, ignoring our own echo if there is one.
 *
 * WHETHER THERE IS ONE IS NOT OURS TO DECIDE. Two wirings are both normal:
 *
 *   - TX and RX tied to the one signal line through a series resistor. We hear
 *     everything we say, so the first frame back is always our own.
 *   - A buffered adapter (Seeed's XIAO Bus Servo Adapter, a 74LVC1G125 and a
 *     direction pin). It turns the line around for us and the echo never
 *     appears; the first frame back is the servo's.
 *
 * So this reads FRAMES, not bytes, and throws away any frame identical to the
 * one just sent. Counting bytes instead — drain exactly as many as we wrote —
 * is what the first version did, and on a buffered adapter it ate the reply
 * and reported a silent bus. Costing an afternoon to a wiring choice the code
 * could simply tolerate is not a trade worth making.
 *
 * Resyncs on the FF FF header rather than assuming the next byte is one: with a
 * shared wire and an unknown baud, landing mid-frame is the normal failure, and
 * hunting for the header is what turns it into a clean timeout instead of
 * garbage that parses.
 */
bool ST3215Bus::receive(uint8_t id, uint8_t* params, uint8_t maxParams, uint8_t* got, uint8_t* err) {
    uint32_t deadline = millis() + kReplyTimeoutMs;
    uint8_t  raw[6 + kMaxParams];
    uint8_t  n = 0;
    bool     synced = false;

    while ((int32_t)(millis() - deadline) < 0) {
        if (!BUS.available()) continue;
        uint8_t b = BUS.read();

        if (!synced) {
            // Two headers in a row, and only then are we in a frame.
            if (n == 0 && b == 0xFF) { raw[n++] = b; continue; }
            if (n == 1 && b == 0xFF) { raw[n++] = b; synced = true; continue; }
            n = (b == 0xFF) ? 1 : 0;
            if (n == 1) raw[0] = 0xFF;
            continue;
        }

        raw[n++] = b;
        if (n < 4) continue;                       // need ID and LEN before we know the size

        uint8_t total = 4 + raw[3];                // FF FF ID LEN + (LEN bytes)
        if (n < total) {
            if (total > sizeof(raw)) { _lastError = "reply longer than the buffer"; return false; }
            continue;
        }

        // Our own transmission, bounced back off a shared wire. Note it (it is
        // worth knowing which wiring you are on) and keep listening.
        bool isEcho = (n == _sentLen);
        for (uint8_t i = 0; isEcho && i < n; i++) isEcho = (raw[i] == _sent[i]);
        if (isEcho) {
            if (_trace) hexdump("RX(echo)", raw, n);
            _echoSeen = true;
            n = 0; synced = false;
            continue;
        }

        if (_trace) hexdump("RX", raw, n);

        uint16_t sum = 0;
        for (uint8_t i = 2; i < n - 1; i++) sum += raw[i];
        if ((uint8_t)(~sum & 0xFF) != raw[n - 1]) { _lastError = "bad checksum"; return false; }
        if (raw[2] != id) { _lastError = "reply from another id"; return false; }

        if (err) *err = raw[4];
        uint8_t payload = (uint8_t)(raw[3] - 2);   // LEN counts the error byte and the checksum
        if (payload > maxParams) { _lastError = "more params than asked for"; return false; }
        for (uint8_t i = 0; i < payload; i++) params[i] = raw[5 + i];
        if (got) *got = payload;
        _lastError = "";
        return true;
    }

    if (_trace && n) hexdump("RX(partial)", raw, n);
    _lastError = n ? "reply cut short" : "no reply";
    return false;
}

bool ST3215Bus::ping(uint8_t id, uint8_t* err) {
    if (!send(id, ST_PING, nullptr, 0)) return false;
    uint8_t params[kMaxParams], got = 0;
    return receive(id, params, kMaxParams, &got, err);
}

bool ST3215Bus::readRegs(uint8_t id, uint8_t addr, uint8_t len, uint8_t* out, uint8_t* err) {
    if (len > kMaxParams) { _lastError = "read too long"; return false; }
    uint8_t p[2] = { addr, len };
    if (!send(id, ST_READ, p, 2)) return false;

    uint8_t params[kMaxParams], got = 0;
    if (!receive(id, params, kMaxParams, &got, err)) return false;
    if (got != len) { _lastError = "short read"; return false; }
    for (uint8_t i = 0; i < len; i++) out[i] = params[i];
    return true;
}

bool ST3215Bus::writeRegs(uint8_t id, uint8_t addr, const uint8_t* data, uint8_t len, uint8_t* err) {
    if (len + 1 > kMaxParams) { _lastError = "write too long"; return false; }
    uint8_t p[1 + kMaxParams];
    p[0] = addr;
    for (uint8_t i = 0; i < len; i++) p[1 + i] = data[i];
    if (!send(id, ST_WRITE, p, (uint8_t)(len + 1))) return false;

    uint8_t params[kMaxParams], got = 0;
    return receive(id, params, kMaxParams, &got, err);
}

bool ST3215Bus::broadcastWrite(uint8_t addr, const uint8_t* data, uint8_t len) {
    if (len + 1 > kMaxParams) { _lastError = "write too long"; return false; }
    uint8_t p[1 + kMaxParams];
    p[0] = addr;
    for (uint8_t i = 0; i < len; i++) p[1 + i] = data[i];
    // 254 is everyone, and everyone answering at once is a collision — so this
    // one deliberately does not wait for a reply.
    return send(0xFE, ST_WRITE, p, (uint8_t)(len + 1));
}

bool ST3215Bus::read8(uint8_t id, uint8_t addr, uint8_t* out) {
    return readRegs(id, addr, 1, out);
}

bool ST3215Bus::read16(uint8_t id, uint8_t addr, uint16_t* out) {
    uint8_t b[2];
    if (!readRegs(id, addr, 2, b)) return false;
    *out = _little ? (uint16_t)(b[0] | (b[1] << 8))
                   : (uint16_t)(b[1] | (b[0] << 8));
    return true;
}

bool ST3215Bus::write8(uint8_t id, uint8_t addr, uint8_t value) {
    return writeRegs(id, addr, &value, 1);
}

bool ST3215Bus::write16(uint8_t id, uint8_t addr, uint16_t value) {
    uint8_t b[2];
    if (_little) { b[0] = value & 0xFF; b[1] = value >> 8; }
    else         { b[0] = value >> 8;   b[1] = value & 0xFF; }
    return writeRegs(id, addr, b, 2);
}

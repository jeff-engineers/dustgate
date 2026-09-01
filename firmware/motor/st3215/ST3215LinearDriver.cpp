// =============================================================================
// ST3215LinearDriver.cpp — see the header for why this is shaped the way it is.
// =============================================================================

#include "ST3215LinearDriver.h"

#if HAS_LINEAR


// Bit 15 of the step command is the direction, and on the bench it turned the
// shaft CLOCKWISE VIEWED FROM THE BACK OF THE MOTOR (2026-08-28). In the
// intended layout that drives the carriage RIGHT — away from the datum, since
// home is always the user's LEFT.
//
// HALF OF THAT IS PROVEN AND HALF IS NOT. The bit→rotation half is a property of
// the servo and will hold. The rotation→carriage half is a property of a mount
// and a rack THAT DO NOT EXIST YET, and a pinion meshing from the far side of
// the rack reverses it. Do not spend an afternoon on it if the first sweep runs
// the wrong way: HOME_DIRECTION_MOUNT in config.h is the one number to change,
// and it is a property of the mount rather than of any individual unit. There is
// no auto-detect any more — a keyed bus connector cannot be wired backwards.
static const uint16_t kAwayFromDatumBit = 0x8000;

// Register 42 carries the step count in 15 bits.
static const long kMaxStepsPerCommand = 32767;

// How often update() asks the servo how far it has left to go. Every loop() pass
// would be a full bus round trip thousands of times a second, for a carriage
// that takes seconds to cross the rail; 20ms is the cadence the bench's
// `stepdump` traced a move at, so it is a rate known to produce clean readings.
static const uint32_t kPollIntervalMs = 20;

// How many consecutive polls with no progress mean the command has retired.
// 5 × 20ms = 100ms of a counter that is not falling. Long enough not to fire on
// the ramp at the start of a move, short enough that a chunked sweep does not
// visibly stutter between chunks.
static const uint8_t kStallPolls = 5;

// A residual this small is arrival on its own account, without waiting out the
// stall.
//
// LOWERED 8 → 3 ON 2026-08-28. 8 counts is 0.32mm, and because a move always
// stops SHORT in the direction it was travelling, that is 0.32mm of undershoot
// one way and 0.32mm the other — 0.64mm of hysteresis between approach
// directions, manufactured entirely in software and indistinguishable from
// mechanical backlash. It showed up in a jog log: target 2080, saved 2072.
//
// 3 is the value the counter was actually seen to settle at. If it settles at 4
// the stall detector retires the command 100ms later, which costs nothing, and
// the leftover is then closed by an ordinary follow-up command from
// sendNextChunk(). The zero-travel guard in update() is what stops that becoming
// an endless chase after a residual too small to overcome static friction.
static const long kArrivalSlack = 3;

// HOW FAR ONE HOMING COMMAND TRAVELS, and the reason the sweep is chunked at all.
//
// The sweep used to be a single command long enough to cross the whole rail,
// which put the entire safety of homing on stop() cancelling it. On 2026-08-28
// that assumption met hardware and lost: the carriage reached the endstop and
// kept driving into it until the servo's own overload protection tripped and its
// LED began to blink. A stop that does not bite is not a logging problem, it is
// a mechanism grinding itself against a hard stop.
//
// So the sweep is now a train of short moves, and the endstop is checked between
// every one. The worst case if stop() fails COMPLETELY is now this distance of
// over-travel instead of the length of the rail — which is inside the over-travel
// a limit switch has after it trips, and is the difference between a bad noise
// and a broken part.
//
// RAISED 2mm → 25mm ON 2026-08-28, because the reason for 2mm went away. The
// chunk was sized for a stop() that did not work at all, so it had to bound the
// damage from every trigger. Cutting torque is now a proven cancel, and 2mm
// chunks at homing speed are ~200ms each — a visible, audible stutter all the
// way down a 580mm rail, and hundreds of bus round trips.
//
// 25mm still bounds the failure to something a limit switch's over-travel can
// absorb, and it is ~25 commands across a full rail instead of ~290. If a stop
// is ever seen to miss again, this is the number to bring back down — it is the
// safety margin, and it is cheap.
static const float kHomingChunkMm = 25.0f;

// The fault backstop, and ONLY a backstop. Completion is distance — register 56
// reaching zero — never elapsed time, because supply voltage moves top speed
// around and this rail has run at 12V, 9V and 8.7V. This number exists so a
// servo that stops answering ends the move as a FAULT instead of leaving the
// runtime's queue wedged forever. A full-span traverse at the slowest speed this
// build uses is ~60s; double it and round up.
static const uint32_t kMoveTimeoutMs = 120000;

// Positive step counts in SKETCH coordinates mean "toward the datum" when
// HOME_DIRECTION is +1 — the same convention SketchLinearDrive encodes as
// `mm * stepsPerMM() * -HOME_DIRECTION`, which is what puts the far endstop at a
// negative position (the bench-measured -4380 in docs/dual-endstop-calibration.md).
static inline bool isAwayFromDatum(long delta) {
    return (delta * -HOME_DIRECTION) > 0;
}

// -----------------------------------------------------------------------------
bool ST3215LinearDriver::begin() {
    if (!_bus.begin(ST3215_BAUD)) {
        Serial.println(F("[ST3215] UART would not open — check PIN_SERVO_BUS_* in the board header."));
        return false;
    }

    uint8_t err = 0;
    if (!_bus.ping(ST3215_SERVO_ID, &err)) {
        // A silent bus cannot say WHY it is silent, and the reasons are
        // indistinguishable from in here. So lead with the one that is both most
        // likely and cheapest to check, and point at the ladder for the rest.
        //
        // POWER IS FIRST FOR A REASON. A servo with no 12V is SILENT, not noisy
        // — identical on the wire to a wrong baud, a broken signal lead or a
        // board that isn't seated. And it is the reading you cannot get at from
        // here: `read` would report the supply voltage if anything were
        // answering, which is exactly the circular part of first contact. The
        // XIAO runs happily off USB alone while the servo rail is dead, so the
        // board being alive enough to print this says nothing about the jack.
        Serial.print(F("[ST3215] no servo answered at id "));
        Serial.print(ST3215_SERVO_ID);
        Serial.print(F(" ("));
        Serial.print(_bus.lastError());
        Serial.println(F(")."));
        Serial.println(F("         → IS THE SERVO POWERED? Check the 12V barrel jack first."));
        Serial.println(F("           USB alone runs this board but NOT the servo, so a live"));
        Serial.println(F("           log and a dead rail look exactly like this. Meter the"));
        Serial.println(F("           servo socket: 12V across its power pins, 5V at the 5V pad."));
        Serial.println(F("         → Then: seating. Reseat the XIAO and the servo lead — an"));
        Serial.println(F("           hour of silence on the bench (2026-08-26) was cured by"));
        Serial.println(F("           nothing else, with every setting already correct."));
        Serial.println(F("         → Then: signal wiring, common ground, baud. Run the bench"));
        Serial.println(F("           console (pio run -e xiao_c5_bus_bench) and work §5.1 of"));
        Serial.println(F("           firmware/wiring/st3215-bench.md — `selftest` splits a"));
        Serial.println(F("           broken UART from a bus with nothing on it."));
        return false;
    }

    // MODE 3 — the whole reason the slider can travel more than one turn. All
    // three conditions or none: the mode itself, and BOTH angle limits at zero,
    // "otherwise it is impossible to step indefinitely". Every one of these goes
    // through the register-55 EEPROM lock (see ST3215Bus::writeEeprom8), which
    // is what makes them survive the next power cycle instead of reading back
    // correct and evaporating.
    if (!_bus.writeEeprom8(ST3215_SERVO_ID, ST_REG_MODE, 3) ||
        !_bus.writeEeprom16(ST3215_SERVO_ID, ST_REG_MIN_ANGLE, 0) ||
        !_bus.writeEeprom16(ST3215_SERVO_ID, ST_REG_MAX_ANGLE, 0)) {
        Serial.print(F("[ST3215] could not set stepping mode: "));
        Serial.println(_bus.lastError());
        return false;
    }

    _online   = true;
    _position = 0;
    _target   = 0;
    _moving   = false;

    // TORQUE COMES BACK OFF AFTER A POWER CYCLE, and a servo with torque off
    // accepts every move and performs none — which looks exactly like a bus
    // fault. Enabling it here is not tidiness, it is the difference between a
    // slider that works after a brownout and one that silently stops moving.
    enable(true);

    Serial.print(F("[ST3215] servo "));
    Serial.print(ST3215_SERVO_ID);
    Serial.print(F(" ready at "));
    Serial.print(_bus.baud());
    Serial.print(F(" baud, "));
    Serial.print(volts(), 1);
    Serial.print(F("V, "));
    Serial.print(ST3215_COUNTS_PER_MM, 3);
    Serial.println(F(" counts/mm."));
    Serial.println(F("[ST3215] position is UNKNOWN until a homing sweep — a step counter has no datum."));
    return true;
}

// -----------------------------------------------------------------------------
bool ST3215LinearDriver::reconnect() {
    // Everything begin() does, minus the assumption that this is the first time.
    // Position is deliberately NOT preserved: if the servo has been unplugged,
    // re-powered or overloaded, our count of where the carriage is stopped being
    // true at that moment, and the only cure is a sweep.
    _online   = false;
    _moving   = false;
    _homing   = false;
    _position   = 0;
    _target     = 0;
    _chunkStart = 0;
    _chunkEnd   = 0;

    Serial.println(F("[ST3215] retrying the bus..."));
    if (!begin()) return false;

    Serial.println(F("[ST3215] back. Position is UNKNOWN until a homing sweep."));
    return true;
}

// -----------------------------------------------------------------------------
void ST3215LinearDriver::enable(bool on) {
    if (!_online) return;
    // Idempotent. The sketch calls this from the idle power-off path and from
    // every recovery branch, so an unconditional write meant a bus transaction
    // and a log line every pass of loop() — which is what buried the log in
    // "torque off — position is no longer trustworthy" on 2026-08-28.
    if (on == _torque) return;
    if (!_bus.write8(ST3215_SERVO_ID, ST_REG_TORQUE_ENABLE, on ? 1 : 0)) {
        Serial.print(F("[ST3215] torque write failed: "));
        Serial.println(_bus.lastError());
        return;
    }
    _torque = on;
    // Torque off means the shaft is free, so whatever we thought the position
    // was is now a guess — a carriage on a rail can be pushed by hand, and on a
    // near-vertical mount it can move on its own. Say so rather than letting a
    // stale count look like knowledge; the sketch's idle power-off path calls
    // this, and a rehome is required afterward.
    if (!on) DEBUG_PRINTLN(F("[ST3215] torque off — position is no longer trustworthy, rehome before moving."));
}

// -----------------------------------------------------------------------------
void ST3215LinearDriver::setMaxSpeed(float countsPerSec) {
    if (countsPerSec < 1.0f) countsPerSec = 1.0f;
    // The measured ceiling is ~1695 counts/s at ~9V and it scales with supply,
    // so anything above it is a number the servo cannot honour. Clamping here
    // rather than passing it on keeps "asked 4000, got 1650" out of the logs.
    if (countsPerSec > 1695.0f) countsPerSec = 1695.0f;
    _speed = (uint16_t)countsPerSec;
}

// -----------------------------------------------------------------------------
bool ST3215LinearDriver::sendSteps(long steps, uint16_t speed) {
    if (!_online) return false;
    if (steps == 0) return true;

    long magnitude = labs(steps);
    if (magnitude > kMaxStepsPerCommand) {
        // Correct _chunkEnd to what is actually being commanded. Leaving it at
        // the full distance would credit travel that never happened when the
        // command retires, and every later move would work from a position that
        // is wrong by the clamped remainder.
        magnitude = kMaxStepsPerCommand;
        _chunkEnd = _chunkStart + (steps > 0 ? magnitude : -magnitude);
    }

    uint16_t raw = (uint16_t)magnitude;
    if (isAwayFromDatum(steps)) raw |= kAwayFromDatumBit;

    // Position, time and speed as ONE write. Setting GOAL_SPEED separately does
    // not work — the servo ran every move at maximum regardless (bench,
    // 2026-08-26). Speed is latched when the block lands.
    if (!_bus.moveTo(ST3215_SERVO_ID, raw, speed)) {
        Serial.print(F("[ST3215] step command refused: "));
        Serial.println(_bus.lastError());
        return false;
    }

    _moveSteps     = magnitude;
    _moveStartedMs = millis();
    _lastPollMs    = _moveStartedMs;
    _moving        = true;
    // Progress tracking is PER COMMAND: the counter restarts at the new value,
    // so carrying a previous command's readings over would look like an instant
    // stall and retire this one before it moved.
    _lastRemaining = -1;
    _stalledPolls  = 0;
    _sawProgress   = false;
    return true;
}

// -----------------------------------------------------------------------------
// Issue the next slice of travel toward _target.
//
// While homing that slice is capped at kHomingChunkMm; otherwise the whole
// remaining distance goes in one command, because an ordinary move is bounded by
// the rail and protected by the endstop supervisor rather than by chunking.
long ST3215LinearDriver::livePosition() const {
    // Nothing in flight, or nothing read back yet: the committed value is all
    // there is, and it is correct.
    if (!_moving || _lastRemaining < 0) return _position;

    // _lastRemaining is how many steps of the command in flight are still owed.
    // The rest of it has been travelled, in the direction the command was going.
    long travelled = _moveSteps - _lastRemaining;
    if (travelled < 0) travelled = 0;
    return (_chunkEnd >= _chunkStart) ? _chunkStart + travelled
                                      : _chunkStart - travelled;
}

void ST3215LinearDriver::sendNextChunk() {
    long delta = _target - _position;
    // Close enough IS arrived. Position is credited from what the servo actually
    // reports, which lands a few counts short, so an equality test here would
    // chase a residual with ever-smaller commands and never finish.
    if (labs(delta) <= kArrivalSlack) { _moving = false; _homing = false; return; }

    if (_homing) {
        const long chunk = (long)(kHomingChunkMm * ST3215_COUNTS_PER_MM);
        if (labs(delta) > chunk) delta = (delta > 0) ? chunk : -chunk;
    }

    _chunkStart = _position;
    _chunkEnd   = _position + delta;
    if (!sendSteps(delta, _homing ? (uint16_t)HOMING_SPEED_STEPS_PER_SEC : _speed)) {
        _moving = false;
        _homing = false;
    }
}

// -----------------------------------------------------------------------------
long ST3215LinearDriver::stepsRemaining() {
    uint16_t raw = 0;
    if (!_bus.read16(ST3215_SERVO_ID, ST_REG_PRESENT_POS, &raw)) return -1;
    // In step mode register 56 is a countdown of the steps still to go, with bit
    // 15 mirroring the commanded direction — NOT a position. `stepdump 12288`
    // wrote 0xB000 and watched it fall to 0x8004 (2026-08-28). Mask the
    // direction off and what is left is the progress bar.
    return (long)(raw & 0x7FFF);
}

// -----------------------------------------------------------------------------
void ST3215LinearDriver::moveTo(long targetSteps) {
    if (!_online) return;
    if (!_torque) {
        // Not a fault: something asked for a move while the rail is de-energised.
        // Refusing loudly beats sending a command the servo will ignore.
        Serial.println(F("[ST3215] move ignored — torque is off. Rehome to re-energise."));
        return;
    }

    // Against the LIVE position, not the committed one: a move issued while
    // another is in flight would otherwise compute its delta from where the
    // previous command started.
    _position = livePosition();
    if (targetSteps == _position) { _target = targetSteps; return; }

    // A full 8-gate 4" span is ~22000 counts, comfortably inside the 32767 a
    // single command carries, so an ordinary move goes in one go and update()
    // picks up any remainder.
    _target = targetSteps;
    _homing = false;
    sendNextChunk();
}

// -----------------------------------------------------------------------------
void ST3215LinearDriver::startHoming() {
    if (!_online) return;
    enable(true);   // a sweep after an idle power-off has to re-energise first

    // There is no "go home" instruction on a bus servo, so homing is an ordinary
    // move long enough to reach the switch from anywhere on the rail, which the
    // endstop supervisor cuts short with stop(). HOMING_MAX_TRAVEL_MM is the
    // runaway guard: if the switch never triggers, the move simply ends and the
    // sweep reports failure rather than pushing the carriage into the rail end.
    long sweep = (long)(HOMING_MAX_TRAVEL_MM * ST3215_COUNTS_PER_MM);
    _homing = true;
    _target = _position + sweep * HOME_DIRECTION;   // +HOME_DIRECTION is toward the datum

    DEBUG_PRINT(F("[ST3215] homing sweep up to ")); DEBUG_PRINT(sweep);
    DEBUG_PRINT(F(" counts toward the datum at ")); DEBUG_PRINT((int)HOMING_SPEED_STEPS_PER_SEC);
    DEBUG_PRINT(F(" counts/s, in "));
    DEBUG_PRINT(kHomingChunkMm);
    DEBUG_PRINTLN(F("mm chunks"));

    sendNextChunk();
}

// -----------------------------------------------------------------------------
void ST3215LinearDriver::stop() {
    if (!_online || !_moving) return;

    // Take the reading BEFORE the stop lands: what is outstanding right now is
    // what the carriage is not going to travel, and it is the only way to keep
    // _position honest across an abort. An endstop trip is this call, so getting
    // this wrong would put the datum in the wrong place.
    long remaining = stepsRemaining();

    // ── STOPPING A SERVO THAT IS COUNTING STEPS ─────────────────────────────
    //
    // This used to write a step count of ZERO, on the reasoning that register 42
    // in mode 3 is a number of steps and zero of them is none. That reasoning
    // was clean and it was wrong: on 2026-08-28 a real carriage reached its
    // endstop, ignored the stop, and kept driving until the servo's overload
    // protection tripped and its LED started blinking. A zero-length command
    // appears simply not to supersede the one already retiring.
    //
    // (Note what it is NOT: writing speed 0 is not the bug, though it looks like
    // one. Speed 0 means MAXIMUM on this part, not stop — a mistake this project
    // has already made once. With a step count of zero the speed never mattered.)
    //
    // So try the things that can work, cheapest and least disruptive first, and
    // CHECK between them rather than assuming. Each one is verified against
    // register 56, which is the only honest witness available:
    //
    // ── THE ANSWER, FROM HARDWARE, 2026-08-28 ───────────────────────────────
    //
    // A NEW STEP COMMAND DOES NOT SUPERSEDE THE ONE IN FLIGHT. That was the open
    // question the bench never got to ("does a second `step` restart the
    // countdown, or continue it?"), and the carriage answered it: a one-step
    // command issued mid-move changed nothing, every time, and the driver fell
    // through to the torque cycle on every single stop. The attempt is gone
    // rather than kept as a first try — it cost 20ms and one step of travel
    // INTO the switch we were trying to stop at, to learn nothing.
    //
    // (Both readings are now closed. Zero steps does not cancel either — that
    // was the first attempt, and it drove a carriage into its endstop until the
    // servo's overload tripped.)
    //
    // SO: CUT TORQUE. A servo holding nothing performs nothing, which is the one
    // thing that cannot be argued with. The shaft is free for ~20ms — on a
    // loaded or inclined rail the carriage can drift a little, and that is the
    // price of the only cancel this part has. Torque goes straight back on,
    // which also CLEARS A LATCHED OVERLOAD: the state a blinking servo LED is
    // reporting after it has been driven into a hard stop, and one that
    // otherwise makes every later move silently do nothing.
    _bus.write8(ST3215_SERVO_ID, ST_REG_TORQUE_ENABLE, 0);
    delay(20);
    _bus.write8(ST3215_SERVO_ID, ST_REG_TORQUE_ENABLE, 1);
    _torque = true;
    _lastStopMethod = "torque cycle";

    if (remaining >= 0 && remaining <= _moveSteps) {
        // Commit whatever the carriage actually reached. Taken through
        // livePosition() so this and getPosition() cannot drift apart.
        _lastRemaining = remaining;
        _position = livePosition();
    } else {
        // The servo did not answer, so how far it got is genuinely unknown.
        // Leaving _position stale would be a lie the next move would act on.
        Serial.println(F("[ST3215] stopped without a readable step count — position is unknown, rehome."));
    }

    _moving = false;
    _homing = false;
    _target     = _position;
    _chunkStart = _position;
    _chunkEnd   = _position;

    DEBUG_PRINT(F("[ST3215] stopped at ")); DEBUG_PRINT(_position);
    DEBUG_PRINT(F(" counts, by: ")); DEBUG_PRINTLN(_lastStopMethod);
}

// -----------------------------------------------------------------------------
void ST3215LinearDriver::update() {
    if (!_online || !_moving) return;

    uint32_t now = millis();
    if (now - _lastPollMs < kPollIntervalMs) return;
    _lastPollMs = now;

    long remaining = stepsRemaining();

    if (remaining < 0) {
        // A dropped reply is ordinary on a bus beside a running dust collector.
        // Ignore it and poll again; the timeout below is what catches a servo
        // that has genuinely stopped answering.
        return;
    }

    // Progress, or the lack of it. See _lastRemaining in the header: this
    // counter does not reach zero, so "has it stopped falling" is the question,
    // not "is it zero".
    if (_lastRemaining < 0 || remaining < _lastRemaining) {
        _sawProgress  = true;
        _stalledPolls = 0;
    } else if (_stalledPolls < 255) {
        _stalledPolls++;
    }
    _lastRemaining = remaining;

    const bool retired = (remaining <= kArrivalSlack) ||
                         (_sawProgress && _stalledPolls >= kStallPolls);

    if (retired) {
        // Credit the travel the servo ACTUALLY reports, not the travel we asked
        // for. The two differ by the few counts it settles short, and assuming
        // the full amount would bias the position a little further every chunk.
        const long before = _position;
        _position = livePosition();
        const long travelled = labs(_position - before);

        // A COMMAND THAT RETIRED HAVING TRAVELLED NOTHING IS NOT PROGRESS.
        // Re-issuing the remainder in that case is an infinite stutter: the same
        // command goes out, retires with zero travel, and goes out again, which
        // is what a jammed carriage or a servo in the wrong mode looks like from
        // here. Two in a row is a fault, and saying so beats grinding.
        if (travelled == 0) {
            if (++_zeroTravelRetires >= 2) {
                Serial.println(F("[ST3215] two commands retired without moving the carriage."));
                Serial.println(F("         Jammed, unpowered, or not in stepping mode — stopping"));
                Serial.println(F("         rather than re-issuing. `status` prints the mode."));
                _moving = false;
                _homing = false;
                _target = _position;
                return;
            }
        } else {
            _zeroTravelRetires = 0;
        }

        // This COMMAND is done. Whether the MOVE is done is a separate question:
        // a homing sweep is a train of chunks, and this is where the next one
        // goes out.
        if (labs(_target - _position) > kArrivalSlack) {
            sendNextChunk();
            return;
        }
        _moving = false;
        if (_homing) {
            // The sweep ran its whole length and never met a switch. Ending here
            // rather than continuing is the runaway guard; the caller sees a
            // motor that stopped without homing and reports the fault.
            Serial.println(F("[ST3215] homing sweep exhausted HOMING_MAX_TRAVEL_MM without"));
            Serial.println(F("         reaching an endstop. Not homed."));
        }
        _homing = false;
        return;
    }

    if (now - _moveStartedMs > kMoveTimeoutMs) {
        Serial.print(F("[ST3215] move did not finish in "));
        Serial.print(kMoveTimeoutMs / 1000);
        Serial.print(F("s — "));
        Serial.print(remaining);
        Serial.println(F(" counts still outstanding. Treating as a FAULT, not as arrival."));
        stop();
    }
}

// -----------------------------------------------------------------------------
float ST3215LinearDriver::volts() {
    uint8_t v = 0;
    if (!_online || !_bus.read8(ST3215_SERVO_ID, ST_REG_PRESENT_VOLTAGE, &v)) return 0.0f;
    return v / 10.0f;
}

int ST3215LinearDriver::tempC() {
    uint8_t t = 0;
    if (!_online || !_bus.read8(ST3215_SERVO_ID, ST_REG_PRESENT_TEMP, &t)) return -1;
    return (int)t;
}

int ST3215LinearDriver::load() {
    uint16_t raw = 0;
    if (!_online || !_bus.read16(ST3215_SERVO_ID, ST_REG_PRESENT_LOAD, &raw)) return 0;
    int mag = raw & 0x7FFF;
    return (raw & 0x8000) ? -mag : mag;   // sign-magnitude, not two's complement
}

// -----------------------------------------------------------------------------
void ST3215LinearDriver::printDriverRegs() {
    if (!_online) {
        Serial.println(F("[ST3215] bus offline — nothing to read."));
        return;
    }
    uint8_t mode = 255, torque = 255;
    _bus.read8(ST3215_SERVO_ID, ST_REG_MODE, &mode);
    _bus.read8(ST3215_SERVO_ID, ST_REG_TORQUE_ENABLE, &torque);

    Serial.println(F("=== ST3215 ==="));
    Serial.print(F("  id/baud:      ")); Serial.print(ST3215_SERVO_ID);
    Serial.print(F(" @ "));              Serial.println(_bus.baud());
    // Mode is on this line because a mode left over from an experiment is the
    // state that silently decides whether a goal does anything at all.
    Serial.print(F("  mode:         ")); Serial.print(mode);
    Serial.println(mode == 3 ? F("  (stepping — correct for the slider)")
                             : F("  ⚠ NOT 3 — moves will be misread as positions"));
    Serial.print(F("  torque:       ")); Serial.println(torque ? F("on") : F("OFF — it will hold nothing"));
    Serial.print(F("  volts:        ")); Serial.print(volts(), 1); Serial.println(F("V"));
    Serial.print(F("  temp:         ")); Serial.print(tempC());    Serial.println(F(" C"));
    Serial.print(F("  load:         ")); Serial.println(load());
    Serial.print(F("  position:     ")); Serial.print(_position);
    Serial.print(F(" counts = "));       Serial.print(_position / ST3215_COUNTS_PER_MM, 2);
    Serial.println(F("mm from the datum (OUR count — the servo does not know)"));
    Serial.print(F("  steps left:   ")); Serial.println(stepsRemaining());
    // Which cancel actually works on this part is the open hardware question the
    // homing path depends on. Print it wherever anyone is already looking.
    Serial.print(F("  last stop by: ")); Serial.println(_lastStopMethod[0] ? _lastStopMethod : "(none yet)");
    Serial.print(F("  echo wiring:  ")); Serial.println(_bus.echoSeen() ? F("one wire (we hear ourselves)")
                                                                       : F("buffered (no echo)"));
}

#endif // HAS_LINEAR

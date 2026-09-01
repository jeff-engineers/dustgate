// =============================================================================
// ST3215LinearDriver.h — the sliding gate, driven by a Feetech bus servo.
//
// The MotorDriver the rack has now that the stepper is in the attic. It speaks
// the MotorDriver vocabulary — startHoming/moveTo/isMoving/getPosition, all in
// "steps" — and on this board a step IS ONE ENCODER COUNT. utils/MotionMath.h
// returns 24.7 counts/mm instead of 51.47 microsteps/mm on a bus board, and
// every mm↔step call site in the sketch keeps working untouched. That is the
// whole reason this fits behind the existing seam rather than beside it.
//
// THREE FACTS ABOUT THIS SERVO SHAPE EVERYTHING BELOW. All three were learned
// the hard way at the bench; wiring/st3215-bench.md §5.0.2 is the long version.
//
//   1. MODE 3 IS A STEP COUNTER, NOT A POSITION SERVO. Mode 0 clamps at one
//      turn by design, and the slider needs five. So register 42 is written
//      with a NUMBER OF STEPS and bit 15 as the direction, and every move here
//      is RELATIVE. Absolute position is this class's own running total.
//
//   2. NOTHING ON THE WIRE KNOWS WHERE THE SHAFT IS. Register 56 in step mode
//      counts DOWN the steps still to go — it is a progress bar, not an
//      odometer. So _position is authoritative, it is a count of what we
//      commanded, and it does not survive a power cycle. That is why the
//      endstops are not optional and why homing is the calibration path.
//
//   3. A SERVO THAT HAS JUST POWERED UP HOLDS NOTHING. Torque comes back OFF,
//      and a servo with torque off accepts every move and performs none. So
//      begin() enables it explicitly, and any code that power-cycles the rail
//      has to come back through begin().
//
// COMPLETION IS DISTANCE, NEVER TIME. isMoving() asks the servo how many steps
// are outstanding and believes the answer. The timeout in update() is a fault
// backstop — it reports a fault, it never reports arrival — because a supply
// that ran at 12V, 9V and 8.7V across three bench sessions is a supply that
// makes every "this move takes about N milliseconds" rule a lie somewhere.
//
// ✅ THIS HAS DRIVEN A REAL CARRIAGE, 2026-08-28. A 4-gate rockler-2.5 rack
// homed, released its datum switch, ran the reference sweep, and moved to all
// four gates repeatably. The measured trigger-to-trigger span came out 252.0mm
// against 250.7mm predicted from the manifold pitch — 0.53%, and a real check on
// the geometry, because counts/mm is DERIVED from the rack spec rather than
// fitted to anything.
//
// Still unproven: this driver behind a NodeLink socket (the xiao_c5_linear node
// has never run), anything about behaviour under load, and 9V vs 12V.
//
// Note that HOME_DIRECTION came out -1 on that mount — bit 15 moves the carriage
// TOWARD the datum there, the opposite of the bare-shaft prediction below. Which
// is exactly the caveat kAwayFromDatumBit carries: bit-to-rotation is a property
// of the servo, rotation-to-carriage is a property of a mount. The sweep detected
// it and persisted the flip, which is the mechanism working as designed.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "../../config.h"

#if HAS_LINEAR

#include "../MotorDriver.h"
#include "ST3215Bus.h"

class ST3215LinearDriver : public MotorDriver {
public:
    // Open the bus, put the servo in step mode, enable torque. Returns false if
    // the servo does not answer or refuses the mode — unlike NullMotorDriver,
    // a false here is REAL broken hardware and FaultPolicy should see it.
    bool begin() override;

    // Drive toward the datum until the endstop supervisor calls stop().
    //
    // ISSUED IN SHORT CHUNKS, NOT AS ONE LONG MOVE — see kHomingChunkMm. That is
    // the difference between a failed stop costing 2mm of over-travel and
    // costing the whole sweep, and it is why this is safe even though a bus
    // servo's stop behaviour in stepping mode is still not fully pinned down.
    // Bounded overall by HOMING_MAX_TRAVEL_MM so a switch that never triggers
    // ends the sweep instead of grinding into the end of the rail.
    void startHoming() override;

    // Absolute target in counts from the datum. Converted to a relative step
    // command against _position, which is the only thing that knows where we
    // are.
    void moveTo(long targetSteps) override;

    // Cut the current move where the carriage stands, and REPORT WHETHER IT
    // WORKED. Costs ~31 counts of coast at speed 400 (bench), which is why
    // homing runs slower than that: an endstop trip is exactly this call, and
    // the coast is the datum's error.
    //
    // It tries more than one thing and says which one bit — see the
    // implementation. Returns through _lastStopMethod rather than a return value
    // because MotorDriver::stop() is void and every existing call site expects
    // that.
    void stop() override;

    void update() override;

    bool isMoving() override { return _moving; }

    // LIVE position, interpolated from the servo's outstanding step count.
    //
    // IT MUST BE LIVE, and that is not a nicety. The sketch reads it DURING a
    // move — the calibration sweep captures the far-endstop trigger with
    // `motor.getPosition()` before it stops the carriage, and the whole span
    // measurement is that one number. A driver that only updated position when a
    // command retired returned the value from when the command was ISSUED, which
    // straight after setHome() is zero: on 2026-08-28 a sweep that ended at 6217
    // counts was recorded as `[CAL] Far endstop at pos=0`, giving span=1.4mm and
    // gate 1 at -125mm, and every move afterwards worked off that.
    //
    // The stepper this driver replaced tracked position continuously because
    // AccelStepper counted its own pulses. Here the servo counts, so the
    // equivalent is to ask it how much of the command is left and subtract.
    long getPosition() override { return livePosition(); }

    // Adopt the current spot as zero. Called after the sweep backs off the
    // switch; nothing is written to the servo, because the servo has no notion
    // of where it is to correct.
    void setHome() override { _position = 0; _target = 0; _chunkEnd = 0; _chunkStart = 0; }

    void enable(bool on) override;

    // -- Called on the concrete type, not through MotorDriver* --
    // Both exist because the sketch reaches for them directly; see
    // NullMotorDriver.h, which has the same two for the same reason.
    void setMaxSpeed(float countsPerSec);
    // Live too — the over-travel supervisor takes its direction from this sign
    // every pass of loop() while a move is in flight.
    long distanceToGo() { return _target - livePosition(); }
    void printDriverRegs();

    // Try to bring the drive up AGAIN, after begin() failed or the servo went
    // away. Returns true if a servo is answering and in stepping mode afterwards.
    //
    // THIS EXISTS BECAUSE A BUS SERVO IS NOT A SOLDERED-DOWN STEPPER DRIVER.
    // Plugging USB into the ESP32 first and the servo lead in second is the
    // ordinary way to work at a bench, and it used to leave the board latched
    // faulted for the rest of the boot with no way back but a reset. A TMC2209
    // that failed its handshake really was broken; a bus with nothing on it yet
    // is just a bus with nothing on it yet.
    //
    // It also clears a LATCHED OVERLOAD — the state a blinking servo LED is
    // reporting after it has been driven into a hard stop — because that state
    // makes every later move silently do nothing.
    bool reconnect();

    // -- Bus-servo specifics the sketch's serial console surfaces --
    bool     online() const  { return _online; }
    float    volts();
    int      tempC();
    int      load();
    ST3215Bus& bus() { return _bus; }

    // How the last stop() actually took effect. "" until one has run. Printed by
    // printDriverRegs(), because which method works is the open hardware
    // question this driver most needs answered.
    const char* lastStopMethod() const { return _lastStopMethod; }

private:
    // Send one relative step command. `steps` is signed in DATUM-RELATIVE terms:
    // positive is away from the datum. Returns false if the bus refused it.
    bool sendSteps(long steps, uint16_t speed);

    // Issue the next slice of travel toward _target, capped at one chunk while
    // homing and sent whole otherwise.
    void sendNextChunk();

    // Where the carriage is right now: the committed position, plus however much
    // of the command in flight has retired. One place, so getPosition(),
    // distanceToGo(), moveTo() and stop() cannot disagree about it.
    long livePosition() const;

    // Steps still outstanding, straight from register 56, or -1 if the servo
    // did not answer. Bit 15 mirrors the commanded direction and is masked off.
    long stepsRemaining();

    ST3215Bus _bus;
    bool      _online   = false;
    bool      _moving   = false;
    bool      _homing   = false;
    bool      _torque   = false;
    long      _position = 0;        // counts from the datum. OUR count, not the servo's.
    long      _target   = 0;        // where the whole move ends
    long      _chunkStart = 0;      // where the command CURRENTLY IN FLIGHT began
    long      _chunkEnd = 0;        // ...and where it ends
    const char* _lastStopMethod = "";
    uint16_t  _speed    = (uint16_t)MAX_SPEED_STEPS_PER_SEC;
    uint32_t  _moveStartedMs = 0;
    long      _moveSteps = 0;       // magnitude of the command in flight, for the fault backstop
    // Register 56 read at a sane cadence rather than every loop(): a poll is a
    // full bus round trip, and loop() runs thousands of times a second.
    uint32_t  _lastPollMs = 0;

    // ARRIVAL IS "THE COUNT STOPPED FALLING", NOT "THE COUNT IS ZERO".
    // The step counter settles a few counts short — 3 and 4 across the two bench
    // runs — so a driver waiting for 0 waits forever. It is written in bold in
    // wiring/st3215-bench.md §5.0.2 and this driver was written against `== 0`
    // anyway (2026-08-28), which is why homing moved exactly one 2mm chunk and
    // then sat there: the chunk never "finished", so the next one never went out.
    long      _lastRemaining = -1;   // previous poll's reading, -1 = none yet
    uint8_t   _stalledPolls  = 0;    // consecutive polls with no progress
    bool      _sawProgress   = false;// the count has fallen at least once
    // Consecutive commands that retired without the carriage moving. Two is a
    // fault, not a reason to try again — see update().
    uint8_t   _zeroTravelRetires = 0;
};

#endif // HAS_LINEAR

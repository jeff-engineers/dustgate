// mock-state.js — in-process device state machine for eval tool execution.
//
// Mirrors the behaviour of mock-api.js but runs in-process so evals don't need
// the HTTP server running. Each scenario gets a fresh MockState instance.

'use strict';

const NUM_STOPS = 16;

class MockState {
  constructor() {
    this.reset();
  }

  reset() {
    this.state          = 'IDLE';
    this.currentStop    = -1;     // -1 = unhomed
    this.positionMm     = 0;
    this.homed          = false;
    this.homeOnRight    = false;
    this.motorInverted  = false;
    this.numActiveStops = 0;
    this.stops          = Array.from({ length: NUM_STOPS + 1 }, (_, i) => ({
      index: i,
      mm: (i * 25).toFixed(2),
    }));
    this.outlets        = [];
  }

  // ── Tool implementations ────────────────────────────────────────────────────

  getStatus() {
    return {
      state:          this.state,
      currentStop:    this.currentStop,
      positionMm:     this.positionMm,
      homed:          this.homed,
      homeOnRight:    this.homeOnRight,
      motorInverted:  this.motorInverted,
      numActiveStops: this.numActiveStops,
      stops:          this.stops,
      outlets:        this.outlets,
    };
  }

  home() {
    this.state       = 'IDLE';
    this.currentStop = 0;
    this.positionMm  = 0;
    this.homed       = true;
    return { ok: true };
  }

  moveToStop(stop) {
    if (stop < 0 || stop > NUM_STOPS) throw new Error(`Stop ${stop} out of range`);
    this.currentStop = stop;
    this.positionMm  = parseFloat(this.stops[stop]?.mm ?? '0');
    return { ok: true };
  }

  jog(mm) {
    this.positionMm += mm;
    return { ok: true };
  }

  saveStop(index) {
    if (index < 1 || index > NUM_STOPS) throw new Error(`Stop index ${index} out of range`);
    this.stops[index] = { index, mm: this.positionMm.toFixed(2) };
    this.currentStop  = index;
    return { ok: true };
  }

  setNumGates(n) {
    if (n < 1 || n > NUM_STOPS) throw new Error(`numGates ${n} out of range`);
    this.numActiveStops = n;
    return { ok: true };
  }

  setOrientation(homeOnRight) {
    this.homeOnRight = homeOnRight;
    return { ok: true };
  }

  setMotorDirection(invert) {
    this.motorInverted = invert;
    return { ok: true };
  }

  pingOutlet(_gen, _ip) {
    // Always succeed in mock — real reachability doesn't matter for evals
    return { reachable: true, powerW: 0 };
  }

  configureOutlet({ slot, generation, ip, name, stop, threshold_w }) {
    const existing = this.outlets.findIndex(o => o.slot === slot);
    const record   = { slot, generation, ip, name, stop, threshold_w: threshold_w ?? 5.0, powerW: 0, active: false, reachable: true };
    if (existing >= 0) {
      this.outlets[existing] = record;
    } else {
      this.outlets.push(record);
    }
    return { ok: true };
  }

  saveOutletConfig() {
    return { ok: true };
  }

  deleteOutlet(slot) {
    this.outlets = this.outlets.filter(o => o.slot !== slot);
    return { ok: true };
  }

  // ── Dispatch by tool name ───────────────────────────────────────────────────

  execute(name, input) {
    switch (name) {
      case 'get_status':        return this.getStatus();
      case 'home':              return this.home();
      case 'move_to_stop':      return this.moveToStop(input.stop);
      case 'jog':               return this.jog(input.mm);
      case 'save_stop':         return this.saveStop(input.index);
      case 'set_num_gates':     return this.setNumGates(input.num_gates);
      case 'set_home_side':     return this.setOrientation(input.home_on_right);
      case 'set_motor_direction': return this.setMotorDirection(input.invert);
      case 'ping_outlet':       return this.pingOutlet(input.generation, input.ip);
      case 'configure_outlet':  return this.configureOutlet(input);
      case 'save_config':       return this.saveOutletConfig();
      case 'delete_outlet':     return this.deleteOutlet(input.slot);
      default:                  throw new Error(`Unknown tool: ${name}`);
    }
  }
}

module.exports = { MockState };

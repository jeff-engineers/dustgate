// fake-node.js — a node board the harness can rearrange: dials a real brain the way firmware/node does (JOIN, then
// WELCOME on HELLO), says what it was flashed as (PWM channels, or bus servos), and keeps where every servo was last
// sent, so a scenario checks WHERE a gate ended up, not only that a frame went out.
//
// Frames come from shared/device-model/nodelink.js, so this cannot drift from the protocol the firmware compiles.
// It also refuses what a real board of that kind would refuse (a bus SET on a PWM board and the reverse), because a
// node reflashed from PWM to serial is one of the scenarios: a fake that accepted anything would hide it.
'use strict';
const WebSocket = require('ws');
const NL = require('../../shared/device-model/nodelink.js');

class FakeNode {
  /** @param {{id:string, kind:'pwm'|'bus', port:number, channels?:number, busServos?:number}} o */
  constructor(o) {
    this.id = o.id;
    this.kind = o.kind;
    this.port = o.port;
    this.channels = o.channels ?? 2;
    this.busServos = o.busServos ?? 6;
    this.at = {};          // 'ch0' / 'bus7' → last angle commanded
    this.gateAt = {};      // selectorId → stateId it last arrived at
    this.sets = [];        // every SET, in order
    this.refused = [];     // every SET this board turned down, with why
    this.online = false;
    this.ws = null;
    this.stopped = false;
  }

  start() {
    this.stopped = false;
    this._dial();
    return this;
  }

  /** Power off: the socket drops and the board stops redialling. Where its servos are stays (a servo holds). */
  stop() {
    this.stopped = true;
    if (this.ws) this.ws.terminate();
    this.online = false;
  }

  /** Reflash as another kind of board, keeping the same name (what `dev.sh flash-node` does to a board). */
  reflash(kind) {
    this.stop();
    this.kind = kind;
    this.at = {};
    this.gateAt = {};
    return this.start();
  }

  _dial() {
    if (this.stopped) return;
    const ws = new WebSocket(`ws://127.0.0.1:${this.port}/nodelink`);
    this.ws = ws;
    ws.on('open', () => ws.send(JSON.stringify(NL.join(this.id))));
    // Only the CURRENT socket answers and redials. A socket dropped by stop()/reflash() closes later, asynchronously; if
    // its close redialled too, the board would dial twice and answer on whichever socket it opened last.
    ws.on('message', (raw) => { if (ws === this.ws) this._onFrame(JSON.parse(String(raw))); });
    ws.on('error', () => {});
    ws.on('close', () => {
      if (ws !== this.ws) return;
      this.online = false;
      if (!this.stopped) setTimeout(() => this._dial(), 300);   // a real node redials too
    });
  }

  _send(f) {
    if (this.ws && this.ws.readyState === 1) this.ws.send(JSON.stringify(f));
    else if (process.env.HARNESS_VERBOSE) console.log(`[fake ${this.id}] could not send ${f.t}: the socket is not open`);
  }

  _onFrame(f) {
    switch (f.t) {
      case 'HELLO': {
        const caps = this.kind === 'bus'
          ? { servos: 0, bus: this.busServos, join: 1 }
          : { servos: this.channels, join: 1 };
        this._send(NL.welcome(this.id, this.kind === 'bus' ? 'xiao_c5_bus' : 'xiao_c5', 'harness', caps));
        this.online = true;
        return;
      }
      case 'PING': this._send(NL.pong()); return;
      case 'CONFIG': this._send(NL.ack(f.seq, true)); return;
      case 'SET': return this._onSet(f);
      default: return;   // anything else a real node would ignore
    }
  }

  _onSet(f) {
    this.sets.push(f);
    let why = '';
    if (this.kind === 'bus' && f.bus === undefined) why = 'a PWM channel sent to a bus board';
    if (this.kind === 'pwm' && f.bus !== undefined) why = 'a bus servo sent to a PWM board';
    if (this.kind === 'pwm' && f.channel !== undefined && f.channel >= this.channels) why = `channel ${f.channel} on a ${this.channels}-channel board`;
    if (f.drive === 'linear') why = why || 'no slider on a node';
    if (why) {
      this.refused.push({ selectorId: f.selectorId, why });
      this._send(NL.ack(f.seq, false, why));
      return;
    }
    this._send(NL.ack(f.seq, true));
    const key = f.bus !== undefined ? `bus${f.bus}` : `ch${f.channel}`;
    this.at[key] = f.angle;
    this.gateAt[f.selectorId] = f.stateId;
    this._send(NL.state(f.selectorId, f.stateId, false));
  }
}

module.exports = { FakeNode };

// fake-plugs.js — the smart plugs in the harness shop, each on its own loopback port (`127.0.0.1:<port>` is its address
// in the layout, so many plugs share one machine without loopback aliases).
//
// A Tasmota answers the few commands the brain sends (Status 8 for power, Status 5 for its MAC, DeviceName, Mem1 for
// who owns it). A Shelly answers its RPCs, and is the collector's switch: Switch.Set turns it on and off, and while on it
// reads the blower's draw, so the brain sees the collector agree.
'use strict';
const http = require('http');

class FakePlug {
  /** @param {{kind:'tasmota'|'shelly', port:number, mac:string, name?:string, blowerW?:number}} o */
  constructor(o) {
    this.kind = o.kind;
    this.port = o.port;
    this.mac = o.mac;
    this.name = o.name ?? '';
    this.watts = 0;
    this.on = false;            // a Shelly's relay (the collector's contactor)
    this.blowerW = o.blowerW ?? 0;
    this.owner = '';
    this.polls = 0;
    this.switches = [];         // every Switch.Set, in order: true / false
    this.server = null;
  }

  get address() { return `127.0.0.1:${this.port}`; }

  start() {
    this.server = http.createServer((req, res) => this._answer(req, res));
    return new Promise((ok) => this.server.listen(this.port, '127.0.0.1', ok));
  }
  stop() { return new Promise((ok) => (this.server ? this.server.close(() => ok()) : ok())); }

  _json(res, body) { res.writeHead(200, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(body)); }

  _answer(req, res) {
    const u = new URL(req.url, 'http://x');
    if (this.kind === 'tasmota' && u.pathname === '/cm') {
      const cmd = (u.searchParams.get('cmnd') || '').trim();
      if (/^Status 8$/i.test(cmd)) { this.polls++; return this._json(res, { StatusSNS: { ENERGY: { Power: this.watts } } }); }
      if (/^Status 5$/i.test(cmd)) return this._json(res, { StatusNET: { Mac: this.mac } });
      if (/^DeviceName$/i.test(cmd)) return this._json(res, { DeviceName: this.name });
      if (/^Mem1$/i.test(cmd)) return this._json(res, { Mem1: this.owner });
      const m = cmd.match(/^Mem1 (.*)$/i);
      if (m) { this.owner = m[1] === '"' ? '' : m[1]; return this._json(res, { Mem1: this.owner }); }
      return this._json(res, { Command: 'Unknown' });
    }
    if (this.kind === 'shelly') {
      switch (u.pathname) {
        case '/rpc/Switch.GetStatus':
          this.polls++;
          return this._json(res, { id: 0, output: this.on, apower: this.on ? this.blowerW : this.watts });
        case '/rpc/Switch.Set': {
          this.on = u.searchParams.get('on') === 'true';
          this.switches.push(this.on);
          return this._json(res, { was_on: !this.on });
        }
        case '/rpc/Shelly.GetDeviceInfo': return this._json(res, { mac: this.mac.replace(/:/g, ''), gen: 2, app: 'PlugUS' });
        case '/rpc/Ws.GetConfig': return this._json(res, { enable: false, server: '' });
        case '/rpc/Switch.GetConfig': return this._json(res, { id: 0, name: this.name });
        case '/rpc/Sys.GetConfig': return this._json(res, { device: { name: this.name } });
        default: return this._json(res, {});
      }
    }
    res.writeHead(404); res.end();
  }
}

module.exports = { FakePlug };

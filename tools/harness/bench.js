// bench.js — a whole shop on one machine: the REAL native brain (native/build/dustgate-brain, --sim) in a scratch state
// folder, fake node boards that dial it, and fake plugs on loopback ports. A scenario drives it the way a person drives
// the shop: save a layout, switch a tool on, unplug a board, reflash it, rearrange.
//
// Never the real ~/.dustgate, never a real address: the brain's state is a mktemp folder and every plug is 127.0.0.1.
'use strict';
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const net = require('net');
const { FakeNode } = require('./fake-node');
const { FakePlug } = require('./fake-plugs');

const ROOT = path.resolve(__dirname, '../..');
const BRAIN = path.join(ROOT, 'native/build/dustgate-brain');
const KEY = 'harness-key-0000';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function freePort() {
  return new Promise((ok, no) => {
    const s = net.createServer();
    s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => ok(p)); });
    s.on('error', no);
  });
}

class Bench {
  constructor() {
    this.nodes = {};      // board id → FakeNode
    this.plugs = {};      // key → FakePlug
    this.brain = null;
    this.log = [];        // the brain's output, kept for a failure report
  }

  /** Stand up the plugs `plugs` names ([{key, kind, mac, blowerW}]) and return key → port, for the shop fixture. */
  async addPlugs(plugs) {
    const ports = {};
    for (const p of plugs) {
      const port = await freePort();
      this.plugs[p.key] = new FakePlug({ ...p, port });
      await this.plugs[p.key].start();
      ports[p.key] = port;
    }
    return ports;
  }

  /** Start the brain, paired with `pair` (board ids). */
  async startBrain(pair = []) {
    if (!fs.existsSync(BRAIN)) throw new Error(`no brain at ${BRAIN} — run: make -C native`);
    this.dir = this.dir || fs.mkdtempSync(path.join(os.tmpdir(), 'dg-harness-'));
    this.port = this.port || await freePort();
    fs.mkdirSync(path.join(this.dir, 'state'), { recursive: true });   // as deploy.sh makes it on the Pi
    const args = ['--id', 'harness-brain', '--key', KEY, '--port', String(this.port), '--ip', '127.0.0.1',
                  '--broadcast', '127.255.255.255', '--state', path.join(this.dir, 'state'), '--sim'];
    if (pair.length) args.push('--pair', pair.join(','));
    this.brain = spawn(BRAIN, args, { env: { ...process.env, DUSTGATE_STATE: path.join(this.dir, 'state') } });
    const keep = (b) => { for (const l of String(b).split('\n')) if (l) this.log.push(l); };
    this.brain.stdout.on('data', keep);
    this.brain.stderr.on('data', keep);
    for (let i = 0; i < 50; i++) {
      try { await this.get('/api/info'); return; } catch { await sleep(100); }
    }
    throw new Error('the brain never answered');
  }

  /** Power the brain off and on again, keeping its state folder (a Pi reboot). */
  async restartBrain(pair = []) {
    await this.stopBrain();
    await this.startBrain(pair);
  }

  async stopBrain() {
    if (!this.brain) return;
    const b = this.brain; this.brain = null;
    await new Promise((ok) => { b.on('exit', ok); b.kill('SIGTERM'); setTimeout(ok, 2000); });
  }

  /** A board, powered on: it dials the brain and stays dialling. */
  addNode(id, kind, opts = {}) {
    const n = new FakeNode({ id, kind, port: this.port, ...opts });
    this.nodes[id] = n;
    return n.start();
  }

  async request(method, route, body) {
    const res = await fetch(`http://127.0.0.1:${this.port}${route}`, {
      method, headers: { 'X-Api-Key': KEY, 'Content-Type': 'application/json' },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    const text = await res.text();
    let json = null; try { json = JSON.parse(text); } catch { /* not JSON */ }
    return { status: res.status, json, text };
  }
  async get(route) {
    const r = await this.request('GET', route);
    if (r.status >= 400) throw new Error(`GET ${route} → ${r.status} ${r.text}`);
    return r.json;
  }

  /** Save a layout, as the app's Save does. Returns the brain's answer. */
  save(doc) { return this.request('PUT', '/api/topology', doc); }
  status() { return this.get('/api/status'); }
  problems() { return this.get('/api/problems'); }
  layout() { return this.get('/api/topology'); }
  /** Switch a tool on or off by hand from the app (a tool with no plug, like the planer). */
  manual(toolId, on) { return this.request('POST', '/api/tool', { toolId, on }); }
  /** A tool on a plug starts or stops drawing power. */
  power(plugKey, watts) { this.plugs[plugKey].watts = watts; }
  pair(host, name = host) { return this.request('POST', '/api/nodes/pair', { host, name }); }
  unpair(host) { return this.request('POST', '/api/nodes/pair', { host, remove: true }); }
  resetAll() { return this.request('POST', '/api/reset-all', {}); }

  /** Wait until `cond()` (sync or async) is true, or give up after `ms`. */
  async until(cond, ms = 5000, step = 100) {
    const end = Date.now() + ms;
    for (;;) {
      if (await cond()) return true;
      if (Date.now() > end) return false;
      await sleep(step);
    }
  }

  async waitOnline(ids, ms = 8000) {
    return this.until(() => ids.every((id) => this.nodes[id] && this.nodes[id].online), ms);
  }

  async close() {
    for (const n of Object.values(this.nodes)) n.stop();
    await this.stopBrain();
    for (const p of Object.values(this.plugs)) await p.stop();
    if (this.dir) fs.rmSync(this.dir, { recursive: true, force: true });
  }
}

module.exports = { Bench, sleep, KEY };

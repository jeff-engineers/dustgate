import { Injectable } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import {
  ApiService,
  DeviceInfo,
  DiscoveredOutlet,
  OutletConfigCmd,
  OutletStatus,
  PingResult,
  SystemStatus,
} from './api.service';
import { HardwareProfileService } from './hardware-profile.service';
import { getAccessCode } from './access-code';

// ── In-browser mock state (mirrors tools/mock-api.js) ─────────────────────────

interface MockState {
  stateName:      string;
  homed:          boolean;
  positionMM:     number;
  positionSteps:  number;
  currentStop:    number;
  targetStop:     number;
  homeOnRight:    boolean;
  motorInverted:  boolean;
  numActiveStops: number;
  dcConfigured:   boolean;
  dcOn:           boolean;
  dcIp:           string | null;
  stops:          Array<{ index: number; mm: string | null }>;
  outlets:        OutletStatus[];
}

// Ping simulation (mirrors tools/mock-api.js): the first two distinct IPs
// ever pinged (excluding the dust collector's own plug) simulate a tool that
// hasn't spun up yet — gate 1's outlet reads 0W once then a steady load,
// gate 2's outlet reads 0W twice then a random load — so the wizard's "no
// load yet, try again" flow has something to demonstrate without hardware.
interface PingSim {
  count: Record<string, number>;   // pings seen per IP (drives the turn-on model)
  base:  Record<string, number>;   // stable per-outlet running draw (W)
}

const NUM_STOPS = 16;
const STEPS_PER_MM = 40;  // mirrors tools/mock-api.js — was 51.47 here, an unintentional drift

// ── mDNS discovery simulation (mirrors tools/mock-api.js) ─────────────────────
// A handful of Shelly outlets "on the network," each with a plausible mDNS
// hostname (ShellyPlugUSG4-<MAC-shaped hex>) and IP. Generated once per page
// load so repeated scans return the same devices, like real mDNS would. Most
// mock devices get no local name (real-world testing found most Shelly Plugs
// never have one set) — only a couple get one, to exercise both code paths.
const MOCK_TOOL_NAMES = ['Table Saw', 'Drill Press', 'Router Table'];

interface MockDiscoveredDevice {
  ip: string;
  hostname: string;
  name: string;
  reachable: boolean;
  powerW: number;
  gen: number;
}

function randomHex(len: number): string {
  let s = '';
  for (let i = 0; i < len; i++) s += Math.floor(Math.random() * 16).toString(16);
  return s.toUpperCase();
}

function randomLanIp(usedIps: Set<string>): string {
  let ip: string;
  do {
    ip = `192.168.87.${20 + Math.floor(Math.random() * 60)}`;
  } while (usedIps.has(ip));
  usedIps.add(ip);
  return ip;
}

// ── Service ────────────────────────────────────────────────────────────────────

/**
 * Drop-in replacement for ApiService used in demo / Vercel mode.
 * All device state is in-memory; agentChat() proxies through /api/claude
 * (the Vercel serverless function) instead of the ESP32.
 *
 * Provided via DI override in app.config.ts when running outside localhost.
 */
@Injectable()
export class DemoApiService extends ApiService {

  private mock: MockState = {
    stateName:      'IDLE',
    homed:          false,
    positionMM:     0,
    positionSteps:  0,
    currentStop:    -1,
    targetStop:     0,
    homeOnRight:    false,
    motorInverted:  false,
    numActiveStops: 0,
    dcOn:           false,
    // Seed the showcase with a pre-configured dust collector so the dashboard
    // toggle works on a fresh page load without running the wizard. A start-over
    // (clearCal) still clears it, matching firmware — this is just the initial
    // demo state, not a hardcoded-always-true override.
    dcIp:           '192.168.87.50',
    dcConfigured:   true,
    stops: Array.from({ length: NUM_STOPS + 1 }, (_, i) => ({ index: i, mm: null as string | null })),
    outlets:        [],
  };

  private pingSim: PingSim = { count: {}, base: {} };

  /** Lazily generated, stable for the life of the page (mirrors tools/mock-api.js). */
  private discovered: MockDiscoveredDevice[] | null = null;

  private ensureDiscovered(): MockDiscoveredDevice[] {
    if (this.discovered) return this.discovered;
    const usedIps = new Set<string>();
    const count = 2 + Math.floor(Math.random() * 3); // 2-4, mirrors real mDNS variability
    // Names drawn from a shuffled pool WITHOUT replacement, so two devices
    // never share a name (e.g. two "Drill Press" entries).
    const namePool = [...MOCK_TOOL_NAMES].sort(() => Math.random() - 0.5);
    const namedIdx = new Set<number>();
    while (namedIdx.size < Math.min(2, count)) namedIdx.add(Math.floor(Math.random() * count));

    let nameCursor = 0;
    this.discovered = Array.from({ length: count }, (_, i) => ({
      ip:        randomLanIp(usedIps),
      hostname:  `ShellyPlugUSG4-${randomHex(12)}`,
      name:      namedIdx.has(i) ? namePool[nameCursor++] : '',
      reachable: true,
      // Tools are off during the setup scan (real power switches, no standby),
      // so a freshly discovered outlet reads a clean 0W. Its running draw shows
      // up later at the threshold step, once the user switches the tool on.
      powerW:    0,
      gen:       2,
    }));
    return this.discovered;
  }

  /** Real device name for a given IP if it's one of the discovered/simulated ones, else ''. */
  private nameForIp(ip: string): string {
    const d = this.ensureDiscovered().find(d => d.ip === ip);
    return d ? d.name : '';
  }

  // Power tools have real on/off switches — no standby draw. An outlet reads a
  // clean 0W until its tool is switched on, then a stable running draw in the
  // 500–1000W range (with a few percent of per-reading jiggle). The setup flow
  // asks the user to turn the tool on before capturing a threshold, so we model
  // that: the FIRST ping to an outlet catches it still off (0W), and subsequent
  // pings read its running draw once "switched on."
  private simulatedPingPower(ip: string): number {
    this.pingSim.count[ip] = (this.pingSim.count[ip] || 0) + 1;
    if (this.pingSim.count[ip] === 1) return 0;   // still off on the first read
    if (!(ip in this.pingSim.base)) this.pingSim.base[ip] = 500 + Math.random() * 500;
    return Math.round(this.pingSim.base[ip] * (1 + (Math.random() - 0.5) * 0.06)); // ±3%
  }

  constructor(http: HttpClient, hardwareProfile: HardwareProfileService) {
    super(http, hardwareProfile);
    // super() triggers init() via the parent ctor;
    // our override runs instead — no HTTP calls made.
  }

  // ── Bootstrap (no HTTP, no WebSocket) ────────────────────────────────────────

  protected override async init(): Promise<void> {
    this.deviceInfo = {
      apiKey:        'demo',
      numStops:      0,
      version:       '1.0-demo',
      homeOnRight:   false,
      motorInverted: false,
    } satisfies DeviceInfo;
    this.apiKey = 'demo';
    this.ready$.next(true);
    this.connected$.next(true);
    this.pushStatus();
  }

  protected override connectWebSocket(): void {
    // No real WebSocket in demo mode — pushStatus() drives status$ instead.
  }

  // ── Status helpers ────────────────────────────────────────────────────────────

  private buildStatus(): SystemStatus {
    return {
      state:          this.mock.stateName,
      currentStop:    this.mock.currentStop,
      targetStop:     this.mock.targetStop,
      positionSteps:  this.mock.positionSteps,
      positionMM:     this.mock.positionMM,
      homed:          this.mock.homed,
      enabled:        true,
      // Only meaningful once homed — before that, position is unknown, so the
      // sensor reads as untriggered rather than misleadingly "at home".
      endstopHome:    this.mock.homed && this.mock.positionMM < 0.5,
      stops:          this.mock.stops,
      outlets:        this.mock.outlets,
      dcConfigured:   this.mock.dcConfigured,
      dcOn:           this.mock.dcOn,
    };
  }

  private pushStatus(): void {
    this.status$.next(this.buildStatus());
  }

  private delay(ms: number): Promise<void> {
    return new Promise(r => setTimeout(r, ms));
  }

  // ── Read ──────────────────────────────────────────────────────────────────────

  override getStatus(): Promise<SystemStatus> {
    return Promise.resolve(this.buildStatus());
  }

  // ── Motion ────────────────────────────────────────────────────────────────────

  override async home(): Promise<{ ok: boolean }> {
    this.mock.stateName = 'HOMING';
    this.pushStatus();
    await this.delay(1500);
    this.mock.stateName      = 'IDLE';
    this.mock.homed          = true;
    this.mock.positionMM     = 0;
    this.mock.positionSteps  = 0;
    this.mock.currentStop    = 0;
    this.mock.targetStop     = 0;
    this.mock.dcOn           = false;   // home → dust collector off
    this.mock.stops[0]       = { index: 0, mm: '0.00' };
    this.pushStatus();
    return { ok: true };
  }

  /** Manual dashboard toggle — mirrors the firmware override. */
  override setDustCollector(on: boolean): Promise<{ ok: boolean }> {
    this.mock.dcOn = on;
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  override configureDustCollector(generation: number, ip: string): Promise<{ ok: boolean }> {
    this.mock.dcIp = ip;
    this.mock.dcConfigured = true;
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  override deleteDustCollector(): Promise<{ ok: boolean }> {
    this.mock.dcIp = null;
    this.mock.dcConfigured = false;
    this.mock.dcOn = false;
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  override async moveToStop(stop: number): Promise<{ ok: boolean }> {
    const fromMM   = parseFloat(this.mock.stops[this.mock.currentStop]?.mm ?? '0');
    const toMM     = parseFloat(this.mock.stops[stop]?.mm ?? '0');
    const distMM   = Math.abs(toMM - fromMM);
    const duration = Math.max(400, distMM * 20);

    this.mock.stateName  = 'MOVING';
    this.mock.targetStop = stop;
    this.pushStatus();
    await this.delay(duration);
    this.mock.stateName  = stop === 0 ? 'IDLE' : 'AT_STOP';
    this.mock.currentStop = stop;
    this.mock.positionMM  = toMM;
    this.mock.positionSteps = Math.round(toMM * STEPS_PER_MM);
    this.mock.dcOn        = stop > 0;   // collector follows gate selection
    this.pushStatus();
    return { ok: true };
  }

  override async jog(mm: number): Promise<{ ok: boolean }> {
    this.mock.stateName = 'MOVING';
    this.pushStatus();
    await this.delay(Math.max(200, Math.abs(mm) * 15));
    this.mock.positionMM    += mm;
    this.mock.positionSteps  = Math.round(this.mock.positionMM * STEPS_PER_MM);
    this.mock.stateName      = 'IDLE';
    this.pushStatus();
    return { ok: true };
  }

  override estop():   Promise<{ ok: boolean }> {
    this.mock.stateName = 'ERROR';
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  override enable():  Promise<{ ok: boolean }> { return Promise.resolve({ ok: true }); }
  override disable(): Promise<{ ok: boolean }> { return Promise.resolve({ ok: true }); }

  // ── Calibration ───────────────────────────────────────────────────────────────

  override async saveStop(index: number): Promise<{ ok: boolean }> {
    const mm = this.mock.positionMM;
    this.checkStopConflict(index, mm);
    this.mock.stops[index] = { index, mm: mm.toFixed(2) };
    if (index > this.mock.numActiveStops) {
      this.mock.numActiveStops = index;
      if (this.deviceInfo) this.deviceInfo.numStops = index;
    }
    this.pushStatus();
    return { ok: true };
  }

  override clearCal(): Promise<{ ok: boolean }> {
    this.mock.homed          = false;
    this.mock.currentStop    = -1;
    this.mock.positionMM     = 0;
    this.mock.positionSteps  = 0;
    this.mock.numActiveStops = 0;
    // Dust collector is part of the outlet config firmware clears on start-over.
    this.mock.dcConfigured   = false;
    this.mock.dcOn           = false;
    this.mock.dcIp           = null;
    this.mock.outlets        = [];
    this.pingSim             = { count: {}, base: {} };
    this.mock.stops = Array.from({ length: NUM_STOPS + 1 }, (_, i) => ({ index: i, mm: null as string | null }));
    if (this.deviceInfo) this.deviceInfo.numStops = 0;
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  // ── Outlets ───────────────────────────────────────────────────────────────────

  override async configureOutlet(cmd: OutletConfigCmd): Promise<{ ok: boolean }> {
    const idx = this.mock.outlets.findIndex(o => o.slot === cmd.slot);
    const hasSwitch = (cmd.ip ?? '').trim().length > 0;
    const record: OutletStatus = {
      slot:      cmd.slot,
      name:      cmd.name,
      stop:      cmd.stop,
      powerW:    0,
      active:    false,
      reachable: hasSwitch,   // name-only gates have no plug to reach
      thresholdW: cmd.threshold_w ?? 5.0,
      hasSwitch,
    };
    if (idx >= 0) {
      this.mock.outlets[idx] = record;
    } else {
      this.mock.outlets.push(record);
    }
    this.pushStatus();
    return { ok: true };
  }

  override async pingOutlet(ip: string): Promise<PingResult> {
    // Always succeed in demo mode. The dust collector's own plug follows its
    // real on/off switch state; every other outlet runs through the ping
    // simulation above. Real auto-detect (Gen 1 then Gen 2) happens
    // device-side; there's no device to detect here, so we always report Gen 2.
    await this.delay(400);
    if (ip === this.mock.dcIp) {
      return { reachable: true, powerW: this.mock.dcOn ? 380 : 0, generation: 2, name: this.nameForIp(ip) };
    }
    return { reachable: true, powerW: this.simulatedPingPower(ip), generation: 2, name: this.nameForIp(ip) };
  }

  /**
   * Scans for Shelly outlets. Mirrors tools/mock-api.js's /api/outlets/discover:
   * a stable set of simulated devices, with slight wattage jitter per call.
   */
  override async discoverOutlets(): Promise<DiscoveredOutlet[]> {
    await this.delay(600);
    return this.ensureDiscovered().map(d => ({
      ip:         d.ip,
      hostname:   d.hostname,
      name:       d.name,
      reachable:  d.reachable,
      // Off tools read a clean 0W; only a running draw gets per-call jitter.
      powerW:     d.powerW === 0
        ? 0
        : Math.max(0, Math.round((d.powerW + (Math.random() - 0.5) * 2) * 10) / 10),
      generation: d.gen,
    }));
  }

  override saveOutletConfig(): Promise<{ ok: boolean }> {
    // State is already in-memory; nothing to persist
    return Promise.resolve({ ok: true });
  }

  override deleteOutlet(slot: number): Promise<{ ok: boolean }> {
    this.mock.outlets = this.mock.outlets.filter(o => o.slot !== slot);
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  // ── Config ────────────────────────────────────────────────────────────────────

  override setOrientation(homeOnRight: boolean): Promise<{ ok: boolean }> {
    this.mock.homeOnRight = homeOnRight;
    if (this.deviceInfo) this.deviceInfo.homeOnRight = homeOnRight;
    return Promise.resolve({ ok: true });
  }

  override setMotorDirection(invert: boolean): Promise<{ ok: boolean }> {
    this.mock.motorInverted = invert;
    if (this.deviceInfo) this.deviceInfo.motorInverted = invert;
    return Promise.resolve({ ok: true });
  }

  override setNumGates(n: number): Promise<{ ok: boolean }> {
    this.mock.numActiveStops = n;
    if (this.deviceInfo) this.deviceInfo.numStops = n;
    // Clear any stale saved positions beyond the new count so they don't
    // trip the proximity check if the count is raised again later.
    for (const s of this.mock.stops) {
      if (s.index > n) s.mm = null;
    }
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  override resetSetup(): Promise<{ ok: boolean }> {
    return this.clearCal();
  }

  override setIdleTimeout(seconds: number): Promise<{ ok: boolean }> {
    // No device to persist to — just reflect it in deviceInfo so the Settings
    // screen shows the value the user picked (base impl would POST and 404).
    if (this.deviceInfo) this.deviceInfo.idleTimeoutSec = seconds;
    return Promise.resolve({ ok: true });
  }

  override forgetWifi(): Promise<{ ok: boolean }> {
    // No real WiFi to forget in demo mode; no-op instead of POSTing to a
    // nonexistent backend.
    return Promise.resolve({ ok: true });
  }

  override async refreshInfo(): Promise<void> {
    // Already in sync — nothing to fetch
  }

  // ── Claude proxy ──────────────────────────────────────────────────────────────

  /**
   * In demo mode, send agent chat requests to the Vercel serverless function
   * (/api/claude) which proxies to Anthropic with a rate-limited server-side key.
   * In local dev (?demo=true), the Angular proxy forwards /api/* to mock-api.js
   * which has an equivalent /api/claude route.
   */
  override agentChat(body: unknown): Promise<Response> {
    const accessCode = getAccessCode();
    const payload = accessCode ? { ...(body as Record<string, unknown>), accessCode } : body;
    return fetch('/api/claude', {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify(payload),
    });
  }

  override setAnthropicKey(_key: string): Promise<{ ok: boolean }> {
    // No-op in demo mode — key lives server-side in Vercel env vars
    return Promise.resolve({ ok: true });
  }
}

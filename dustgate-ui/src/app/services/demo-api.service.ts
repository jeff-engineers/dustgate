import { Injectable } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import { firstValueFrom } from 'rxjs';
import {
  ApiService,
  DeviceInfo,
  OutletConfigCmd,
  OutletStatus,
  PingResult,
  SystemStatus,
} from './api.service';

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
  stops:          Array<{ index: number; mm: string }>;
  outlets:        OutletStatus[];
}

const NUM_STOPS = 16;

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
    stops: Array.from({ length: NUM_STOPS + 1 }, (_, i) => ({ index: i, mm: '0.00' })),
    outlets:        [],
  };

  constructor(http: HttpClient) {
    super(http);
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
      endstopHome:    this.mock.positionMM < 0.5,
      stops:          this.mock.stops,
      outlets:        this.mock.outlets,
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
    this.mock.stops[0]       = { index: 0, mm: '0.00' };
    this.pushStatus();
    return { ok: true };
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
    this.mock.positionSteps = Math.round(toMM * 51.47);
    this.pushStatus();
    return { ok: true };
  }

  override async jog(mm: number): Promise<{ ok: boolean }> {
    this.mock.stateName = 'MOVING';
    this.pushStatus();
    await this.delay(Math.max(200, Math.abs(mm) * 15));
    this.mock.positionMM    += mm;
    this.mock.positionSteps  = Math.round(this.mock.positionMM * 51.47);
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
    this.mock.outlets        = [];
    this.mock.stops = Array.from({ length: NUM_STOPS + 1 }, (_, i) => ({ index: i, mm: '0.00' }));
    if (this.deviceInfo) this.deviceInfo.numStops = 0;
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  // ── Outlets ───────────────────────────────────────────────────────────────────

  override async configureOutlet(cmd: OutletConfigCmd): Promise<{ ok: boolean }> {
    const idx = this.mock.outlets.findIndex(o => o.slot === cmd.slot);
    const record: OutletStatus = {
      slot:      cmd.slot,
      name:      cmd.name,
      stop:      cmd.stop,
      powerW:    0,
      active:    false,
      reachable: true,
      thresholdW: cmd.threshold_w ?? 5.0,
    };
    if (idx >= 0) {
      this.mock.outlets[idx] = record;
    } else {
      this.mock.outlets.push(record);
    }
    this.pushStatus();
    return { ok: true };
  }

  override async pingOutlet(gen: number, ip: string): Promise<PingResult> {
    // Always succeed in demo mode — simulates a reachable outlet at idle draw
    await this.delay(400);
    return { reachable: true, powerW: 0.5 };
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
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  override resetSetup(): Promise<{ ok: boolean }> {
    return this.clearCal();
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
  override agentChat(body: unknown): Promise<unknown> {
    return firstValueFrom(
      this.http.post('/api/claude', body)
    );
  }

  override setAnthropicKey(_key: string): Promise<{ ok: boolean }> {
    // No-op in demo mode — key lives server-side in Vercel env vars
    return Promise.resolve({ ok: true });
  }
}

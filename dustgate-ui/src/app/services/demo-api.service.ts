import { Injectable } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import {
  ApiService,
  DeviceInfo,
  DiscoveredOutlet,
  OutletConfigCmd,
  PingResult,
  SystemStatus,
} from './api.service';
import { HardwareProfileService } from './hardware-profile.service';
import { getAccessCode } from './access-code';
import * as model from '@device-model';

// ── Service ────────────────────────────────────────────────────────────────────

/**
 * Drop-in replacement for ApiService used in demo / Vercel mode.
 *
 * This is a THIN async wrapper over the canonical device model
 * (shared/device-model/device-model.js) — the SAME model that drives the Node
 * dev mock (tools/mock-api.js). All device behaviour lives in the model; this
 * class only owns timing (await delay between the begin/complete motion steps),
 * maps the model's wire shape to the Angular API types, and routes agent chat
 * through the Vercel serverless function instead of the ESP32.
 *
 * Provided via DI override in app.config.ts when running outside localhost.
 */
@Injectable()
export class DemoApiService extends ApiService {

  /** The canonical device instance (in-memory, resets on page load). */
  private d: model.Device = model.createDevice();

  constructor(http: HttpClient, hardwareProfile: HardwareProfileService) {
    super(http, hardwareProfile);
    // super() triggers init() via the parent ctor; our override runs instead.
  }

  // ── Bootstrap (no HTTP, no WebSocket) ────────────────────────────────────────

  protected override async init(): Promise<void> {
    // Seed the showcase with a pre-configured dust collector so the dashboard
    // toggle works on a fresh page load without running the wizard. A start-over
    // (clearCal) still clears it, matching firmware — this is just initial demo
    // state, not a hardcoded-always-true override.
    this.d.dcConfigured = true;
    this.d.dcIp = '192.168.87.50';

    this.deviceInfo = {
      apiKey:        'demo',
      numStops:      0,
      version:       '1.0-demo',
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
    return model.statusView(this.d) as unknown as SystemStatus;
  }

  private pushStatus(): void {
    this.status$.next(this.buildStatus());
  }

  private delay(ms: number): Promise<void> {
    return new Promise(r => setTimeout(r, ms));
  }

  /** Keep deviceInfo (read by Settings/visualizer) in step with the model. */
  private syncInfo(): void {
    if (!this.deviceInfo) return;
    this.deviceInfo.numStops      = this.d.numActiveStops;
    this.deviceInfo.motorInverted = this.d.motorInverted;
    this.deviceInfo.idleTimeoutSec = this.d.idleTimeoutSec;
  }

  // ── Read ──────────────────────────────────────────────────────────────────────

  override getStatus(): Promise<SystemStatus> {
    return Promise.resolve(this.buildStatus());
  }

  // ── Motion (model owns state, we own the delay between begin/complete) ──────────

  override async home(): Promise<{ ok: boolean }> {
    const durMs = model.beginHome(this.d);
    this.pushStatus();
    await this.delay(durMs);
    model.completeHome(this.d);
    this.pushStatus();
    return { ok: true };
  }

  override async moveToStop(stop: number): Promise<{ ok: boolean }> {
    const durMs = model.beginMove(this.d, stop);
    this.pushStatus();
    await this.delay(durMs);
    model.completeMove(this.d, stop);
    this.pushStatus();
    return { ok: true };
  }

  override async jog(mm: number): Promise<{ ok: boolean }> {
    const durMs = model.beginJog(this.d, mm);
    this.pushStatus();
    await this.delay(durMs);
    model.completeJog(this.d);
    this.pushStatus();
    return { ok: true };
  }

  override estop(): Promise<{ ok: boolean }> {
    model.estop(this.d);
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  override enable():  Promise<{ ok: boolean }> { return Promise.resolve(model.setEnabled(this.d, true)); }
  override disable(): Promise<{ ok: boolean }> { return Promise.resolve(model.setEnabled(this.d, false)); }

  // ── Dust collector ──────────────────────────────────────────────────────────────

  override setDustCollector(on: boolean): Promise<{ ok: boolean }> {
    model.switchDustCollector(this.d, on);
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  override configureDustCollector(generation: number, ip: string): Promise<{ ok: boolean }> {
    model.configureDustCollector(this.d, { gen: generation, ip });
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  override deleteDustCollector(): Promise<{ ok: boolean }> {
    model.deleteDustCollector(this.d);
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  // ── Calibration ───────────────────────────────────────────────────────────────

  override async saveStop(index: number): Promise<{ ok: boolean }> {
    // Client-side friendly pre-check (throws a helpful message) stays in the base
    // ApiService; the model then applies the device-level behaviour (an overlap
    // is silently skipped, matching firmware).
    this.checkStopConflict(index, this.d.positionMM);
    model.saveStop(this.d, index);
    this.syncInfo();
    this.pushStatus();
    return { ok: true };
  }

  override clearCal(): Promise<{ ok: boolean }> {
    model.clearCal(this.d);
    this.syncInfo();
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  // ── Outlets ───────────────────────────────────────────────────────────────────

  override async configureOutlet(cmd: OutletConfigCmd): Promise<{ ok: boolean }> {
    model.configureOutlet(this.d, {
      slot:      cmd.slot,
      name:      cmd.name,
      stop:      cmd.stop,
      ip:        cmd.ip,
      host:      cmd.host,
      gen:       cmd.generation,
      threshold: cmd.threshold_w,
    });
    this.pushStatus();
    return { ok: true };
  }

  override async pingOutlet(ip: string): Promise<PingResult> {
    await this.delay(400);
    const r = model.pingOutlet(this.d, ip);
    return { reachable: r.reachable, powerW: r.powerW, generation: r.gen, name: r.name };
  }

  override async discoverOutlets(): Promise<DiscoveredOutlet[]> {
    await this.delay(600);
    return model.discoverOutlets(this.d).map(x => ({
      ip:         x.ip,
      hostname:   x.hostname,
      name:       x.name,
      reachable:  x.reachable,
      powerW:     x.powerW,
      generation: x.gen,
    }));
  }

  override saveOutletConfig(): Promise<{ ok: boolean }> {
    return Promise.resolve({ ok: true }); // state is already in-memory
  }

  override deleteOutlet(slot: number): Promise<{ ok: boolean }> {
    model.deleteOutlet(this.d, slot);
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  // ── Config ────────────────────────────────────────────────────────────────────

  override setHomedLeft(homedLeft: boolean): Promise<{ ok: boolean }> {
    model.setHomedLeft(this.d, homedLeft);
    this.syncInfo();
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  override setMotorDirection(invert: boolean): Promise<{ ok: boolean }> {
    model.setMotorInverted(this.d, invert);
    this.syncInfo();
    return Promise.resolve({ ok: true });
  }

  override setNumGates(n: number): Promise<{ ok: boolean }> {
    model.setNumGates(this.d, n);
    this.syncInfo();
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  override async calibrate(modelId: string, gateCount: number): Promise<{ ok: boolean }> {
    const durMs = model.beginCalibrate(this.d, modelId, gateCount);
    this.pushStatus();
    await this.delay(durMs);
    model.completeCalibrate(this.d);
    this.syncInfo();
    this.pushStatus();
    return { ok: true };
  }

  override setPortRole(index: number, role: string): Promise<{ ok: boolean }> {
    model.setPortRole(this.d, index, role as model.PortRole);
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  override setIdleTimeout(seconds: number): Promise<{ ok: boolean }> {
    model.setIdleTimeout(this.d, seconds);
    this.syncInfo();
    return Promise.resolve({ ok: true });
  }

  override resetSetup(): Promise<{ ok: boolean }> {
    return this.clearCal();
  }

  override forgetWifi(): Promise<{ ok: boolean }> {
    return Promise.resolve({ ok: true }); // no real WiFi to forget
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
    return Promise.resolve({ ok: true }); // key lives server-side in Vercel env vars
  }
}

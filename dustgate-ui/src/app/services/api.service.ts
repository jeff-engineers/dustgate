import { Injectable } from '@angular/core';
import { HttpClient, HttpHeaders } from '@angular/common/http';
import { BehaviorSubject, Observable, Subject, firstValueFrom } from 'rxjs';
import { HardwareProfileService } from './hardware-profile.service';

// ── Types ──────────────────────────────────────────────────────────────────────

export interface OutletStatus {
  slot: number;
  name: string;
  stop: number;
  powerW: number;
  active: boolean;
  reachable: boolean;
  thresholdW?: number;
}

export interface StopInfo {
  index: number;
  mm: string | null;      // millimetres from home as string (e.g. "25.00");
                          // null = position not yet saved (distinct from a
                          // gate legitimately saved at 0.00, right at home)
}

export interface SystemStatus {
  state: string;          // 'IDLE' | 'HOMING' | 'MOVING' | 'AT_STOP' | 'ERROR' | ...
  currentStop: number;    // -1 = unknown
  targetStop: number;
  positionSteps: number;
  positionMM?: number;    // raw actuator position, independent of any saved stop (used to
                          // render continuous movement while jogging between stops)
  homed: boolean;
  enabled: boolean;
  endstopHome: boolean;
  manualOverride?: boolean;   // true while user-commanded move blocks outlet auto-select
  stops?: StopInfo[];         // per-stop mm positions; embedded in every status push
  outlets: OutletStatus[];
}

export interface OutletConfigCmd {
  slot: number;
  generation: number;     // 1 or 2
  ip: string;
  name: string;
  stop: number;
  threshold_w?: number;
}

export interface PingResult {
  reachable: boolean;
  powerW: number;
}

export interface DeviceInfo {
  apiKey: string;
  numStops: number;
  version: string;
  homeOnRight?: boolean;    // true = home endstop is on the right side of the manifold
  motorInverted?: boolean;  // true = homing direction was flipped via setup agent
}

// ── Service ────────────────────────────────────────────────────────────────────

@Injectable({ providedIn: 'root' })
export class ApiService {

  // Base URL is the device itself (app is served from it).
  // In dev mode the Angular proxy rewrites /api → ESP32 IP (see proxy.conf.json).
  private readonly baseUrl = '';

  protected apiKey = '';
  private ws: WebSocket | null = null;

  /** Live system status pushed from the WebSocket. */
  readonly status$ = new BehaviorSubject<SystemStatus | null>(null);
  /** Emits true while WebSocket is connected. */
  readonly connected$ = new BehaviorSubject<boolean>(false);
  /** Emits when the API key / device info is ready. */
  readonly ready$ = new BehaviorSubject<boolean>(false);

  deviceInfo: DeviceInfo | null = null;

  constructor(protected http: HttpClient, protected hardwareProfile: HardwareProfileService) {
    // Deferred to a microtask so subclass field initializers (which run after
    // super() returns) are set before an overridden init() can read them.
    queueMicrotask(() => this.init());
  }

  // ── Bootstrap ────────────────────────────────────────────────────────────────

  protected async init() {
    try {
      // /api/info is unauthenticated — gives us the API key so we can make
      // all subsequent calls.  The app is served FROM the device, so this
      // is no less secure than any other local-network device API.
      const info = await firstValueFrom(
        this.http.get<DeviceInfo>(`${this.baseUrl}/api/info`)
      );
      this.apiKey    = info.apiKey;
      this.deviceInfo = info;
      this.ready$.next(true);
      this.connectWebSocket();
    } catch (e) {
      console.error('[API] Failed to fetch /api/info:', e);
      // Retry after 3s (device might still be booting)
      setTimeout(() => this.init(), 3000);
    }
  }

  // ── WebSocket ─────────────────────────────────────────────────────────────────

  protected connectWebSocket() {
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    const host  = location.host;  // same host as the page (ESP32 IP or dev proxy)
    const url   = `${proto}://${host}/ws`;

    this.ws = new WebSocket(url);

    this.ws.onopen = () => {
      this.connected$.next(true);
      console.log('[WS] Connected');
    };

    this.ws.onmessage = (ev) => {
      try {
        const status = JSON.parse(ev.data) as SystemStatus;
        this.status$.next(status);
      } catch { /* ignore malformed frames */ }
    };

    this.ws.onclose = () => {
      this.connected$.next(false);
      console.log('[WS] Disconnected — reconnecting in 3s');
      setTimeout(() => this.connectWebSocket(), 3000);
    };

    this.ws.onerror = () => {
      this.ws?.close();
    };
  }

  // ── HTTP helpers ──────────────────────────────────────────────────────────────

  private headers(): HttpHeaders {
    return new HttpHeaders({ 'X-Api-Key': this.apiKey });
  }

  private post<T = { ok: boolean }>(path: string, body: unknown = {}): Promise<T> {
    return firstValueFrom(
      this.http.post<T>(`${this.baseUrl}${path}`, body, { headers: this.headers() })
    );
  }

  private put<T = { ok: boolean }>(path: string, body: unknown): Promise<T> {
    return firstValueFrom(
      this.http.put<T>(`${this.baseUrl}${path}`, body, { headers: this.headers() })
    );
  }

  private delete<T = { ok: boolean }>(path: string): Promise<T> {
    return firstValueFrom(
      this.http.delete<T>(`${this.baseUrl}${path}`, { headers: this.headers() })
    );
  }

  getStatus(): Promise<SystemStatus> {
    return firstValueFrom(
      this.http.get<SystemStatus>(`${this.baseUrl}/api/status`, { headers: this.headers() })
    );
  }

  // ── Motion commands ───────────────────────────────────────────────────────────

  home()                    { return this.post('/api/home'); }
  moveToStop(stop: number)  { return this.post('/api/move', { stop }); }
  jog(mm: number)           { return this.post('/api/jog', { mm }); }
  estop()                   { return this.post('/api/estop'); }
  enable()                  { return this.post('/api/enable'); }
  disable()                 { return this.post('/api/disable'); }
  clearCal()                { return this.post('/api/clearcal'); }

  // ── Outlet commands ───────────────────────────────────────────────────────────

  configureOutlet(cmd: OutletConfigCmd) {
    return this.put(`/api/outlets/${cmd.slot}`, {
      gen:       cmd.generation,
      ip:        cmd.ip,
      name:      cmd.name,
      stop:      cmd.stop,
      threshold: cmd.threshold_w ?? 5.0
    });
  }

  pingOutlet(gen: number, ip: string): Promise<PingResult> {
    return this.post<PingResult>('/api/outlets/ping', { gen, ip });
  }

  saveOutletConfig() { return this.post('/api/outlets/save'); }

  deleteOutlet(slot: number) { return this.delete(`/api/outlets/${slot}`); }

  /**
   * Save current motor position as a numbered stop.
   * Call while the motor is stationary at the desired gate position.
   */
  saveStop(index: number) {
    this.checkStopConflict(index, this.status$.value?.positionMM ?? 0);
    return this.post('/api/setstop', { index });
  }

  /**
   * Guards against saving two gates on top of each other. Only compares
   * against other saved GATES — home (stop 0) is excluded, since a gate
   * legitimately being close to home isn't a conflict the same way two gates
   * overlapping is. Minimum separation is half the expected gate spacing for
   * the selected port size — loose enough to tolerate 3D-printed rack/pinion
   * tolerance and non-uniform spacing, but tight enough to catch "forgot to
   * jog" / accidental re-saves.
   */
  protected checkStopConflict(index: number, mm: number): void {
    const stops = this.status$.value?.stops ?? [];
    const minSpacingMm = this.hardwareProfile.expectedGateSpacingMm * 0.5;
    for (const s of stops) {
      if (s.index === index || s.index === 0) continue;
      if (s.mm === null) continue; // position not yet saved — not a real gate
      const stopMm = parseFloat(s.mm);
      if (Math.abs(mm - stopMm) < minSpacingMm) {
        throw new Error(
          `This looks too close to Gate ${s.index}'s saved position (${stopMm.toFixed(1)} mm) — jog further away before saving.`
        );
      }
    }
  }

  /**
   * Persist orientation preference (which side the home endstop is on).
   * Stored in device NVS; returned in /api/info on next boot.
   */
  setOrientation(homeOnRight: boolean) {
    if (this.deviceInfo) this.deviceInfo.homeOnRight = homeOnRight;
    return this.post('/api/config/orientation', { homeOnRight });
  }

  /**
   * Flip the motor homing direction (normal vs inverted).
   * Use when the actuator runs away from the endstop instead of toward it.
   */
  setMotorDirection(invert: boolean) {
    if (this.deviceInfo) this.deviceInfo.motorInverted = invert;
    return this.post('/api/config/motor', { invertDirection: invert });
  }

  /**
   * Set the number of active blast gates.
   * Updates /api/info and the visualizer gate count without recompiling.
   */
  setNumGates(n: number) {
    if (this.deviceInfo) this.deviceInfo.numStops = n;
    return this.post('/api/config/gates', { numGates: n });
  }

  /**
   * Reset calibration and gate count — returns the device to unconfigured state
   * so the setup wizard can run again from scratch.
   */
  resetSetup() {
    if (this.deviceInfo) this.deviceInfo.numStops = 0;
    return this.post('/api/clearcal', {});
  }

  /**
   * Re-fetch /api/info and update deviceInfo in place.
   * Call after any operation that changes the device's configuration state
   * (e.g. start-over) so the visualizer reflects reality immediately.
   */
  async refreshInfo(): Promise<void> {
    try {
      const info = await firstValueFrom(
        this.http.get<DeviceInfo>(`${this.baseUrl}/api/info`)
      );
      this.apiKey    = info.apiKey;
      this.deviceInfo = info;
    } catch {
      // Non-fatal — optimistic update from resetSetup() already set numStops = 0
    }
  }

  // ── Agent ─────────────────────────────────────────────────────────────────────

  /**
   * Forward a full Anthropic /v1/messages request through the ESP32 proxy.
   * body should be the complete request object (model, messages, tools, etc.)
   */
  agentChat(body: unknown): Promise<unknown> {
    return firstValueFrom(
      this.http.post(`${this.baseUrl}/api/agent/chat`, body, { headers: this.headers() })
    );
  }

  setAnthropicKey(key: string) {
    return this.put('/api/agent/key', { key });
  }
}

import { Injectable } from '@angular/core';
import { HttpClient, HttpErrorResponse, HttpHeaders } from '@angular/common/http';
import { BehaviorSubject, Observable, Subject, filter, firstValueFrom, take } from 'rxjs';
import { HardwareProfileService } from './hardware-profile.service';
import type { Topology } from '@topology';
import type { TopologyStatus } from '@topology-device';

// Re-export so components/services can import topology types from one place.
export type { Topology } from '@topology';
export type { TopologyStatus } from '@topology-device';

// ── Types ──────────────────────────────────────────────────────────────────────

export interface OutletStatus {
  slot: number;
  name: string;
  stop: number;
  powerW: number;
  active: boolean;
  reachable: boolean;
  thresholdW?: number;
  hasSwitch?: boolean;   // false = name-only gate (no smart plug attached)
}

export interface StopInfo {
  index: number;
  mm: string | null;      // millimetres from home as string (e.g. "25.00");
                          // null = position not yet saved (distinct from a
                          // gate legitimately saved at 0.00, right at home)
  role?: string;          // 'tool' | 'unassigned' | 'blocked' | 'feed' | 'home'
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
  farEndstop?: boolean;       // far-end limit switch triggered (dual-endstop builds)
  manifoldModel?: string;     // manifold profile used for calibration ('custom' if none)
  measuredSpanSteps?: number | null; // endstop-to-endstop span from the reference sweep
  stepsPerMm?: number;        // calibrated steps/mm (nominal until a sweep runs)
  dcConfigured?: boolean;     // true once a dust collector plug has been assigned
  dcOn?: boolean;             // current dust collector switch state
  ssid?: string;              // WiFi network the board is joined to; absent on older firmware
  stops?: StopInfo[];         // per-stop mm positions; embedded in every status push
  outlets: OutletStatus[];
}

export interface OutletConfigCmd {
  slot: number;
  generation: number;     // 1 or 2
  ip: string;
  /** mDNS hostname (no ".local"), if this outlet was picked from a scan rather than typed in. Lets the device re-resolve its IP after a DHCP change. */
  host?: string;
  name: string;
  stop: number;
  threshold_w?: number;
}

export interface PingResult {
  reachable: boolean;
  powerW: number;
  /** Shelly API generation the device answered on (1 or 2); 0 if unreachable. */
  generation: number;
  /** The name the user gave this device in the Shelly app, if any set. */
  name?: string;
}

export interface DiscoveredOutlet {
  ip: string;
  hostname: string;
  /** The name the user gave this device in the Shelly app (e.g. "Drill Press"), empty if unset. */
  name: string;
  reachable: boolean;
  powerW: number;
  /** Shelly API generation (1 or 2); 0 if the mDNS hit didn't respond to a probe. */
  generation: number;
}

/** A secondary board found on the network by GET /api/nodes/discover. */
export interface DiscoveredNode {
  /** mDNS hostname — the STABLE key a topology binds link.host to. */
  host: string;
  /** Last-known address. Informational: the link re-resolves via host. */
  ip: string;
  /** Build target it reports, e.g. "qtpy_s3". */
  board: string;
  /** Servo channels it offers. */
  servos: number;
}

/** Live link state for one controller in the topology (GET /api/nodes). */
export interface NodeLinkState {
  id: string;
  host: string;
  /** Friendly name, stored on the device beside the pairing — so a board keeps it
   *  through a layout wipe and can be named before any layout exists. Empty until
   *  someone renames it. */
  name?: string;
  online: boolean;
  /** millis() on the device when we last heard from it. Informational. */
  lastSeen: number;
  board: string;
  fw: string;
  caps: { servos: number; linear: number };
}

export interface DeviceInfo {
  apiKey: string;
  numStops: number;
  version: string;
  motorInverted?: boolean;  // true = homing direction was auto-flipped during homing
  idleTimeoutSec?: number;  // seconds of inactivity before the driver powers off; 0 = never
  manifoldModel?: string;   // manifold profile last calibrated against
  stepsPerMm?: number;      // calibrated steps/mm
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

  /** Current /api/info retry delay, doubled on each failure up to a minute. */
  private infoRetryMs = 3000;

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
      this.infoRetryMs = 3000;   // so a later reconnect isn't stuck at the backed-off delay
      this.ready$.next(true);
      this.connectWebSocket();
    } catch (e) {
      // A 200 that still lands here is the dev-server SPA fallback answering /api/info
      // with index.html, which fails JSON parsing — i.e. `ng serve` is running with no
      // /api proxy and there is no backend at all. That surfaces as
      // "HttpErrorResponse {status: 200, ok: false}", which reads like a device fault
      // and isn't one, so name it instead of logging the raw error.
      if (e instanceof HttpErrorResponse && e.status === 200) {
        console.error(
          '[API] /api/info returned HTML, not JSON — the dev server has no /api proxy ' +
          'and no backend is running. Use `bash dev.sh mock` (mock ESP32) or ' +
          '`bash dev.sh demo` (no backend; needs ?demo=true in the URL).');
      } else {
        console.error('[API] Failed to fetch /api/info:', e);
      }
      // Back off rather than retrying at a fixed 3s forever: a missing backend never
      // recovers on its own, and hammering it buried the console in repeats of the
      // same stack. A booting device still gets caught by the early retries.
      this.infoRetryMs = Math.min(this.infoRetryMs * 2, 60_000);
      setTimeout(() => this.init(), this.infoRetryMs);
    }
  }

  /**
   * Resolves once the API key is in hand. init() runs on a microtask, so a component
   * that fetches straight out of ngOnInit would otherwise race it and get a 401 —
   * which reads as "no shop configured" rather than "not ready yet". Await this first.
   */
  whenReady(): Promise<void> {
    if (this.ready$.value) return Promise.resolve();
    return firstValueFrom(this.ready$.pipe(filter(Boolean), take(1))).then(() => undefined);
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

  private get<T>(path: string): Promise<T> {
    return firstValueFrom(
      this.http.get<T>(`${this.baseUrl}${path}`, { headers: this.headers() })
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

  /** Motion-hardware view: stepper state, position, endstops, outlet readings. */
  getMotionStatus(): Promise<SystemStatus> {
    return firstValueFrom(
      this.http.get<SystemStatus>(`${this.baseUrl}/api/motion`, { headers: this.headers() })
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
      host:      cmd.host ?? '',
      name:      cmd.name,
      stop:      cmd.stop,
      threshold: cmd.threshold_w ?? 5.0
    });
  }

  /** Pings a Shelly outlet — the device speaks the Shelly Gen2+ local API (Gen1 is not supported). */
  async pingOutlet(ip: string): Promise<PingResult> {
    const raw = await this.post<{ reachable: boolean; powerW: number; gen: number; name?: string }>(
      '/api/outlets/ping', { ip }
    );
    return { reachable: raw.reachable, powerW: raw.powerW, generation: raw.gen, name: raw.name };
  }

  /**
   * Scans the local network via mDNS for Shelly outlets (no manual IP entry
   * required — devices show up as long as they're powered and on the same
   * subnet, regardless of whether a static IP was ever set in the Shelly app).
   * Each hit is already probed for reachability/generation/wattage, same as pingOutlet().
   */
  async discoverOutlets(): Promise<DiscoveredOutlet[]> {
    const raw = await this.get<Array<{ ip: string; hostname: string; name: string; reachable: boolean; powerW: number; gen: number }>>(
      '/api/outlets/discover'
    );
    return raw.map(r => ({
      ip: r.ip,
      hostname: r.hostname,
      name: r.name,
      reachable: r.reachable,
      powerW: r.powerW,
      generation: r.gen
    }));
  }

  saveOutletConfig() { return this.post('/api/outlets/save'); }

  deleteOutlet(slot: number) { return this.delete(`/api/outlets/${slot}`); }

  /** Manually switch the dust collector on/off (holds until the next auto event). */
  setDustCollector(on: boolean) { return this.post('/api/dustcollector/switch', { on }); }

  /** Assign a Shelly outlet as the dust collector's switchable plug. */
  configureDustCollector(generation: number, ip: string, host: string = '') {
    return this.put('/api/dustcollector', { gen: generation, ip, host });
  }

  /** Unassign the dust collector's plug. */
  deleteDustCollector() { return this.delete('/api/dustcollector'); }

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
    const numGates = this.deviceInfo?.numStops ?? Infinity;
    const minSpacingMm = this.hardwareProfile.expectedGateSpacingMm * 0.5;
    for (const s of stops) {
      if (s.index === index || s.index === 0) continue;
      if (s.index > numGates) continue; // beyond the currently configured gate count — stale, not a real gate
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
   * Report which side the actuator homed to. The home datum is always the user's
   * LEFT endstop; if it homed right, the firmware switches the datum to the other
   * endstop so the next home parks on the left and gates read Gate 1..N left→right.
   */
  setHomedLeft(homedLeft: boolean) {
    return this.post('/api/config/orientation', { homedLeft });
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
   * Run the dual-endstop reference sweep: auto-direction, home, sweep to the far
   * endstop, calibrate steps/mm, and (for a known manifold) auto-place every gate.
   * `model` is a manifold profile id ('rockler-2.5' | 'rockler-4' | 'custom').
   */
  calibrate(model: string, gateCount: number) {
    return this.post('/api/calibrate', { model, gateCount });
  }

  /** Set a port's role: 'tool' | 'unassigned' | 'blocked' | 'feed'. */
  setPortRole(index: number, role: string) {
    return this.post('/api/config/port-role', { index, role });
  }

  // ── topology API (additive; DemoApiService overrides these in-process) ──
  /** Fetch the configured topology, or throws/404 if none is set. */
  getTopology(): Promise<Topology> { return this.get<Topology>('/api/topology'); }
  /** Replace the topology (validated device-side; 400 on invalid). */
  putTopology(topology: Topology): Promise<{ ok: boolean }> { return this.put('/api/topology', topology); }
  /** Live status: actuator states, tool activity, collector, conflicts, reachability. */
  getStatus(): Promise<TopologyStatus> { return this.get<TopologyStatus>('/api/status'); }
  /** Sim/demo only: inject a tool's power reading to drive routing (real firmware senses plugs). */
  simTool(toolId: string, watts: number): Promise<TopologyStatus> {
    return this.post<TopologyStatus>('/api/sim/tool', { toolId, watts });
  }

  /**
   * Switch a tool on or off BY HAND — what the Live view's rows do.
   *
   * Distinct from simTool(), which injects a fake wattage and exists only in the
   * mock and the browser demo. The Live view used to call that, so on real
   * hardware every tap 404'd and the daily-driver screen drove nothing. Sensed
   * tools still routed themselves off their Shelly plugs, which is what hid it.
   */
  setToolManual(toolId: string, on: boolean): Promise<unknown> {
    return this.post('/api/tool', { toolId, on });
  }

  /**
   * Setup only: drive one servo directly so the user can watch the valve and capture
   * where it lands. Rejects with 501 on a build without servo support, which the gate
   * configurator surfaces rather than pretending the nudge worked.
   */
  // ── Secondary boards ───────────────────────────────────────────────────
  /**
   * Sweep the network for secondary boards (mDNS _dustgate._tcp, role=secondary).
   * Discovery only POPULATES the picker — the saved topology still binds each
   * board by an explicit link.host, so a spare board on the bench or a second
   * system in the shop can never silently adopt itself into a live layout.
   */
  discoverNodes(): Promise<DiscoveredNode[]> {
    return this.get<DiscoveredNode[]>('/api/nodes/discover');
  }

  /**
   * Pair a secondary board, by mDNS host. The device persists this in NVS and
   * dials it immediately — INDEPENDENT of the topology, so a paired board stays
   * paired across a layout wipe (a full firmware+filesystem flash rewrites the
   * partition the topology lives in) and links before any shop is drawn.
   */
  pairNode(host: string, name?: string): Promise<unknown> {
    return this.post('/api/nodes/pair', { host, ...(name ? { name } : {}) });
  }
  unpairNode(host: string): Promise<unknown> {
    return this.post('/api/nodes/pair', { host, remove: true });
  }

  /** Live link state per controller — which boards are actually answering. */
  async getNodes(): Promise<NodeLinkState[]> {
    const r = await this.get<{ nodes: NodeLinkState[] }>('/api/nodes');
    return r?.nodes ?? [];
  }

  /**
   * Nudge one servo to an absolute angle for calibration.
   *
   * `controllerId` says WHICH board — the primary relays it to that node. Pass the
   * selector's own controllerId always, not just for secondaries: every board
   * numbers its channels 0-3, so omitting it silently jogs the primary's servo on
   * the same channel, which is a different valve entirely.
   */
  jogServo(channel: number, angle: number, controllerId?: string): Promise<unknown> {
    return this.post('/api/servo/jog', { channel, angle, ...(controllerId ? { controllerId } : {}) });
  }
  /** De-energize a servo — the valve holds by friction/detent. */
  detachServo(channel: number, controllerId?: string): Promise<unknown> {
    return this.post('/api/servo/jog', { channel, detach: true, ...(controllerId ? { controllerId } : {}) });
  }

  /**
   * Reset calibration and gate count — returns the device to unconfigured state
   * so setup can run again from scratch.
   */
  resetSetup() {
    if (this.deviceInfo) this.deviceInfo.numStops = 0;
    return this.post('/api/clearcal', {});
  }

  /**
   * Set how many seconds of no move/home activity before the driver powers
   * off (0 = never). Waking it back up always requires a rehome.
   */
  setIdleTimeout(seconds: number) {
    if (this.deviceInfo) this.deviceInfo.idleTimeoutSec = seconds;
    return this.post('/api/config/idle-timeout', { seconds });
  }

  /**
   * Erases stored WiFi credentials and reboots the device into its captive
   * setup portal. The device disappears from the network almost immediately,
   * so callers should assume the response may not arrive.
   */
  forgetWifi() {
    return this.post('/api/wifi/reset', {});
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
}

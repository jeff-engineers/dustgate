import { Injectable } from '@angular/core';
import { HttpClient, HttpHeaders } from '@angular/common/http';
import { BehaviorSubject, Observable, Subject, firstValueFrom } from 'rxjs';

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

export interface SystemStatus {
  state: string;          // 'IDLE' | 'HOMING' | 'MOVING' | 'AT_STOP' | 'ERROR' | ...
  currentStop: number;    // -1 = unknown
  targetStop: number;
  positionSteps: number;
  homed: boolean;
  enabled: boolean;
  endstopHome: boolean;
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
}

// ── Service ────────────────────────────────────────────────────────────────────

@Injectable({ providedIn: 'root' })
export class ApiService {

  // Base URL is the device itself (app is served from it).
  // In dev mode the Angular proxy rewrites /api → ESP32 IP (see proxy.conf.json).
  private readonly baseUrl = '';

  private apiKey = '';
  private ws: WebSocket | null = null;

  /** Live system status pushed from the WebSocket. */
  readonly status$ = new BehaviorSubject<SystemStatus | null>(null);
  /** Emits true while WebSocket is connected. */
  readonly connected$ = new BehaviorSubject<boolean>(false);
  /** Emits when the API key / device info is ready. */
  readonly ready$ = new BehaviorSubject<boolean>(false);

  deviceInfo: DeviceInfo | null = null;

  constructor(private http: HttpClient) {
    this.init();
  }

  // ── Bootstrap ────────────────────────────────────────────────────────────────

  private async init() {
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

  private connectWebSocket() {
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

// Type contract for device-model.js — the canonical DustGate device model.
// Hand-written to keep the Node mock build-free while giving TypeScript
// consumers (demo-api.service.ts) full types. Keep in step with the .js.

export const NUM_STOPS: number;
export const STEPS_PER_MM: number;
export const MIN_STOP_SEPARATION_MM: number;
export const IDLE_TIMEOUT_SEC_DEFAULT: number;
export const HOME_MS: number;
export const CALIBRATE_MS: number;
export const PORT_ROLES: PortRole[];
export const MANIFOLD_PROFILES: Record<string, { firstGateOffsetMm: number; gatePitchMm: number; endMarginMm: number }>;

export type PortRole = 'tool' | 'unassigned' | 'blocked' | 'feed' | 'home';

export interface StopEntry {
  index: number;
  mm: string | null; // "25.00"; null = not yet saved
  role: PortRole;
}

export interface ManifoldGeometry {
  spanMm: number;
  gatesMm: number[];
}

export interface OutletRecord {
  slot: number;
  name: string;
  stop: number;
  powerW: number;
  active: boolean;
  reachable: boolean;
  thresholdW: number;
  gen: number;
  ip: string;
  host: string;
  hasSwitch: boolean;
}

/** Full internal device object. Only *View projections go on the wire. */
export interface Device {
  state: string;
  currentStop: number;
  targetStop: number;
  positionSteps: number;
  positionMM: number;
  homed: boolean;
  enabled: boolean;
  manualOverride: boolean;
  homeOnRight: boolean;
  motorInverted: boolean;
  numActiveStops: number;
  idleTimeoutSec: number;
  farEndstop: boolean;
  manifoldModel: string;
  measuredSpanSteps: number | null;
  stepsPerMm: number;
  dcConfigured: boolean;
  dcOn: boolean;
  dcIp: string | null;
  dcHost: string;
  stops: StopEntry[];
  outlets: OutletRecord[];
  // internal sim state (never serialized)
  _discovered: DiscoveredOutlet[] | null;
  _pingCount: Record<string, number>;
  _pingBase: Record<string, number>;
  _jogMM?: number;
  _calGateCount?: number;
}

export interface StatusView {
  state: string;
  currentStop: number;
  targetStop: number;
  positionSteps: number;
  positionMM: number;
  homed: boolean;
  enabled: boolean;
  endstopHome: boolean;
  manualOverride: boolean;
  farEndstop: boolean;
  manifoldModel: string;
  measuredSpanSteps: number | null;
  stepsPerMm: number;
  dcConfigured: boolean;
  dcOn: boolean;
  stops: StopEntry[];
  outlets: OutletRecord[];
}

export interface InfoView {
  apiKey: string;
  numStops: number;
  version: string;
  homeOnRight: boolean;
  motorInverted: boolean;
  idleTimeoutSec: number;
  manifoldModel: string;
  stepsPerMm: number;
}

export interface OutletConfigInput {
  slot: number;
  name: string;
  stop: number;
  ip?: string;
  host?: string;
  gen?: number;
  threshold?: number;
}

export interface DustCollectorInput {
  gen?: number;
  ip?: string;
  host?: string;
}

export interface DiscoveredOutlet {
  ip: string;
  hostname: string;
  name: string;
  reachable: boolean;
  powerW: number;
  gen: number;
}

export interface PingResult {
  reachable: boolean;
  powerW: number;
  gen: number;
  name: string;
}

export interface SaveStopResult {
  ok: boolean;
  skipped: boolean;
}

export function createDevice(): Device;
export function statusView(d: Device): StatusView;
export function infoView(d: Device, apiKey: string, version: string): InfoView;

export function beginHome(d: Device): number;
export function completeHome(d: Device): void;
export function beginMove(d: Device, stop: number): number;
export function completeMove(d: Device, stop: number): void;
export function beginJog(d: Device, mm: number): number;
export function completeJog(d: Device): void;
export function estop(d: Device): { ok: boolean };
export function setEnabled(d: Device, on: boolean): { ok: boolean };

export function saveStop(d: Device, index: number): SaveStopResult;
export function setOrientation(d: Device, homeOnRight: boolean): { ok: boolean };
export function setMotorInverted(d: Device, invert: boolean): { ok: boolean };
export function setNumGates(d: Device, n: number): { ok: boolean };
export function setIdleTimeout(d: Device, seconds: number): { ok: boolean };
export function clearCal(d: Device): { ok: boolean };

export function configureOutlet(d: Device, cmd: OutletConfigInput): { ok: boolean };
export function deleteOutlet(d: Device, slot: number): { ok: boolean };
export function configureDustCollector(d: Device, cmd: DustCollectorInput): { ok: boolean };
export function deleteDustCollector(d: Device): { ok: boolean };
export function switchDustCollector(d: Device, on: boolean): { ok: boolean };

export function manifoldProfile(model: string, gateCount: number): ManifoldGeometry | null;
export function beginCalibrate(d: Device, model: string, gateCount: number): number;
export function completeCalibrate(d: Device): void;
export function setPortRole(d: Device, index: number, role: PortRole): { ok: boolean };

export function ensureDiscovered(d: Device): DiscoveredOutlet[];
export function discoverOutlets(d: Device): DiscoveredOutlet[];
export function pingOutlet(d: Device, ip: string): PingResult;
export function nameForIp(d: Device, ip: string): string;

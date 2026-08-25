// Minimal TypeScript types for the topology device simulator (topology-device.js).

import { Topology } from './topology';

export interface TopologyDevice {
  topology: Topology;
  actuatorStates: Record<string, string | null>;
  collectorOn: boolean;
  collectorCoasting: boolean;
  [k: string]: unknown;
}

/**
 * What this system's collector PLUG reported back, as opposed to what we told
 * it to do. Absent when no plug is paired, or when nothing has reported yet —
 * an all-zero reading would render as a dead blower rather than an absent one.
 */
export interface CollectorPlug {
  watts: number;
  reachable: boolean;
  /** How long it has been commanded ON. An age, not a timestamp. */
  onForMs: number;
}

/** One airflow system's blower, as the device reports it. */
export interface SystemStatus {
  /** What we COMMANDED. Not proof the blower is running — see `plug`. */
  collectorOn: boolean;
  coasting?: boolean;
  /** Firmware only — no mock analogue. Additive, so the contract holds. */
  deadHeadRisk?: boolean;
  transitioning?: boolean;
  /** This blower is running because a person switched it on, with no machine
   *  asking for it. See setCollectorManual. */
  manual?: boolean;
  plug?: CollectorPlug;
}

/**
 * What became of one machine's ports this routing pass.
 *
 *   routed    every enabled port got a clear path
 *   partial   some ports lost, and every one that lost is SUPPLEMENTAL
 *   stripped  a PRIMARY port lost — the alarm case (RFC §10.3)
 *
 * `stripped` is the one the Live view has to shout about: a machine drawing
 * power with its primary gate shut is running with no air, which is the single
 * thing that page exists to explain.
 */
export interface MachineStatus {
  status: 'routed' | 'partial' | 'stripped';
  routed: string[];
  blocked: string[];
}

export interface TopologyStatus {
  actuators: Record<string, string | null>;
  /**
   * Keyed by MACHINE, not by port — this answers "what is running", and what
   * runs is a machine. `manual` is firmware-only: the mock drives a hand-run
   * through synthetic watts, so it cannot tell one apart. Absent means "don't
   * know", never "no".
   */
  tools: Record<string, { watts: number; active: boolean; manual?: boolean }>;
  /**
   * "Is ANY blower running." What a one-system shop has always read, and what it
   * still means there — but a SUMMARY, and wrong as a decision once a shop has
   * two blowers. Anything that speaks per system reads `systems` instead.
   */
  collectorOn: boolean;
  /** Present only while a blower is finishing its coast-down. Also shop-wide. */
  collectorCoasting?: boolean;
  /**
   * Per-blower truth, keyed by system id. Both the firmware
   * (TopologyRuntime::writeStatus) and the model (statusView) have published this
   * all along; it was simply missing from this interface, so no caller could see
   * it. Optional because a device older than the field would omit it — callers
   * fall back to `collectorOn`, which is exactly right for the one-system case.
   */
  systems?: Record<string, SystemStatus>;
  conflicts: unknown[];
  /**
   * Keyed by PORT id — the thing that either got air or didn't. NOT usable with
   * a machine id: a two-port saw has no entry under its own id, so a lookup by
   * machine silently reads `undefined` and the tool renders as not collecting.
   * Read `machines` for a per-machine answer.
   */
  reachable: Record<string, boolean>;
  /**
   * The rolled-up verdict per machine. Both the firmware
   * (TopologyRuntime::writeStatus) and the model (statusView) have published
   * this all along; it was simply missing from this interface. Optional so a
   * device older than the field degrades rather than breaks.
   */
  machines?: Record<string, MachineStatus>;
}

export function createTopologyDevice(t: Topology): TopologyDevice;
export function setToolPower(d: TopologyDevice, toolId: string, watts: number, nowMs?: number): unknown;
export function statusView(d: TopologyDevice, nowMs?: number): TopologyStatus;
/** Run ONE system's blower by hand, or stop it. Holds until switched off; opens a
 *  path first, so it can never dead-head. Mirrors
 *  TopologyRuntime::setCollectorManual. */
export function setCollectorManual(d: TopologyDevice, systemId: string, on: boolean, nowMs?: number): unknown;
export function collectorIsManual(d: TopologyDevice, systemId: string): boolean;
/** Stage a plug failure on one blower — mock/demo only. */
export function setCollectorPlugFault(
  d: TopologyDevice, systemId: string, fault: 'dead' | 'offline' | null): { ok: boolean };
/**
 * What the plug says is happening, as opposed to what we asked for. The verdict
 * lives HERE and only here — the firmware reports the facts and does not judge
 * them. See the note on COLLECTOR_RUNNING_W in topology-device.js.
 */
export function collectorPlugState(
  plug: CollectorPlug | undefined, commandedOn: boolean,
): 'noplug' | 'unknown' | 'off' | 'starting' | 'running' | 'notStarting';
export const COLLECTOR_RUNNING_W: number;
export const COLLECTOR_SPINUP_GRACE_MS: number;
export function tickCollector(d: TopologyDevice, nowMs: number): void;
/** Coast-down applied when a collector names none. Mirrors
 *  kDefaultCollectorOffDelayMs in firmware/control/TopologyRuntime.h. */
export const DEFAULT_COLLECTOR_OFF_DELAY_MS: number;
export function collectorOffDelayMs(topology: Topology, systemId?: string): number;
export function toolThreshold(topology: Topology, toolId: string): number;

// Minimal TypeScript types for the topology device simulator (topology-device.js).

import { Topology } from './topology';

export interface TopologyDevice {
  topology: Topology;
  actuatorStates: Record<string, string | null>;
  collectorOn: boolean;
  collectorCoasting: boolean;
  [k: string]: unknown;
}

/** One airflow system's blower, as the device reports it. */
export interface SystemStatus {
  collectorOn: boolean;
  coasting?: boolean;
  /** Firmware only — no mock analogue. Additive, so the contract holds. */
  deadHeadRisk?: boolean;
  transitioning?: boolean;
}

export interface TopologyStatus {
  actuators: Record<string, string | null>;
  tools: Record<string, { watts: number; active: boolean }>;
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
  reachable: Record<string, boolean>;
}

export function createTopologyDevice(t: Topology): TopologyDevice;
export function setToolPower(d: TopologyDevice, toolId: string, watts: number, nowMs?: number): unknown;
export function statusView(d: TopologyDevice, nowMs?: number): TopologyStatus;
export function tickCollector(d: TopologyDevice, nowMs: number): void;
/** Coast-down applied when a collector names none. Mirrors
 *  kDefaultCollectorOffDelayMs in firmware/control/TopologyRuntime.h. */
export const DEFAULT_COLLECTOR_OFF_DELAY_MS: number;
export function collectorOffDelayMs(topology: Topology, systemId?: string): number;
export function toolThreshold(topology: Topology, toolId: string): number;

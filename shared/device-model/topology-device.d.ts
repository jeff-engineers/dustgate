// Minimal TypeScript types for the topology device simulator (topology-device.js).

import { Topology } from './topology';

export interface TopologyDevice {
  topology: Topology;
  actuatorStates: Record<string, string | null>;
  collectorOn: boolean;
  collectorCoasting: boolean;
  [k: string]: unknown;
}

export interface TopologyStatus {
  actuators: Record<string, string | null>;
  tools: Record<string, { watts: number; active: boolean }>;
  collectorOn: boolean;
  /** Present only while the blower is finishing its coast-down. */
  collectorCoasting?: boolean;
  conflicts: unknown[];
  reachable: Record<string, boolean>;
}

export function createTopologyDevice(t: Topology): TopologyDevice;
export function setToolPower(d: TopologyDevice, toolId: string, watts: number, nowMs?: number): unknown;
export function statusView(d: TopologyDevice, nowMs?: number): TopologyStatus;
export function tickCollector(d: TopologyDevice, nowMs: number): void;
export function collectorOffDelayMs(topology: Topology): number;
export function toolThreshold(topology: Topology, toolId: string): number;

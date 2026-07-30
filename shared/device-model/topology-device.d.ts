// Minimal TypeScript types for the v2 topology device simulator (topology-device.js).

import { Topology } from './topology';

export interface TopologyDevice {
  topology: Topology;
  actuatorStates: Record<string, string | null>;
  collectorOn: boolean;
  [k: string]: unknown;
}

export interface TopologyStatus {
  actuators: Record<string, string | null>;
  tools: Record<string, { watts: number; active: boolean }>;
  collectorOn: boolean;
  conflicts: unknown[];
  reachable: Record<string, boolean>;
}

export function createTopologyDevice(t: Topology): TopologyDevice;
export function setToolPower(d: TopologyDevice, toolId: string, watts: number): unknown;
export function statusView(d: TopologyDevice): TopologyStatus;
export function toolThreshold(topology: Topology, toolId: string): number;

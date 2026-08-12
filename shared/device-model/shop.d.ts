// Minimal TypeScript types for the shop contract (shop.js).
// Same posture as topology.d.ts: the documents stay opaque validated blobs, and
// only the shapes the app actually reads are named.

import type { Topology, ValidationResult } from './topology';

/** One airflow graph. Exactly one collector; shares no duct with a sibling. */
export interface System {
  id: string;
  name?: string;
  elements: Record<string, unknown>[];
  ducts: Record<string, unknown>[];
}

/** The thing you switch on. Owns the plug, the trip point and the display name. */
export interface Machine {
  id: string;
  name?: string;
  sensor?: { outlet?: Record<string, unknown> };
}

export interface Shop {
  schemaVersion: number;
  name?: string;
  controllers: Record<string, unknown>[];
  systems: System[];
  machines: Machine[];
  /** Shop-scoped, non-airflow (air quality, power). Container only for now. */
  devices?: Record<string, unknown>[];
  ui?: Record<string, unknown>;
}

export const SHOP_SCHEMA_VERSION: number;

/** Present one system as a plain topology — controllers spliced in from the shop. */
export function systemView(shop: Shop, system: System): Topology;
export function systemsOf(shop: Shop): System[];
export function machinesOf(shop: Shop): Machine[];
export function machineIndex(shop: Shop): Map<string, Machine>;
/** machineId → every port of that machine, DISABLED ones included. */
export function portsByMachine(shop: Shop): Map<string, { systemId: string; port: Record<string, unknown> }[]>;
/** A port counts for routing unless explicitly disabled (absent means enabled). */
export function portEnabled(port: Record<string, unknown>): boolean;

export function validateShop(shop: unknown): ValidationResult;

/**
 * A machine is only as routed as its worst port.
 *   routed   — every enabled port got a clear path
 *   partial  — some ports lost, and every one that lost was `supplemental`
 *   stripped — a PRIMARY port lost. The alarm case: a saw running, gate shut.
 */
export type MachineStatus = 'routed' | 'partial' | 'stripped';

export interface MachineRouting {
  routed: string[];
  blocked: string[];
  status: MachineStatus;
}

export interface ShopRouting {
  /** selectorId → stateId to command, merged across every system */
  states: Record<string, string | null>;
  /** conflicts, each tagged with the system it happened in */
  conflicts: { systemId: string; selectorId: string; winner: string; winnerState: string; losers: string[] }[];
  /** portId → won a clear path right now */
  reachable: Record<string, boolean>;
  machines: Record<string, MachineRouting>;
}

/** @param activeMachineIds highest priority first */
export function routeShop(shop: Shop, activeMachineIds: string[]): ShopRouting;

export interface SystemPlan {
  systemId: string;
  moves: { selectorId: string; toState: string; kind: string; phase: 'make' | 'break' }[];
  /** per system — two blowers means two answers */
  deadHeadRisk: boolean;
}

export function planShopTransition(
  shop: Shop,
  currentStates: Record<string, string | null>,
  desiredStates: Record<string, string | null>,
  opts?: { collectorRunning?: boolean | Record<string, boolean> },
): SystemPlan[];

/** Lift a schemaVersion-1 topology into a shop. One machine per existing tool. */
export function migrateToShop(topology: Topology, opts?: { systemId?: string; systemName?: string }): Shop;
export function isShop(doc: unknown): boolean;
/** Accept either shape, return a shop. */
export function asShop(doc: unknown, opts?: { systemId?: string; systemName?: string }): Shop;

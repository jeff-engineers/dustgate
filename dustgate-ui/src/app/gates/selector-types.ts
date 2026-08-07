// selector-types.ts — a typed slice of the v2 topology, for the gate configurator.
//
// shared/device-model/topology.d.ts declares a Topology as Record<string, unknown>,
// so every consumer hand-rolls string-keyed casts. Rather than type the whole graph
// speculatively, this narrows just the part the configurator actually edits: a servo
// selector and its states. topology.js remains the authority on what's VALID; these
// are only shapes.
//
// The angle math (absolute ⇄ referenceAngle+offsetDeg) lives in topology.js beside
// servoCommandAngle, so it is unit-tested by topology.test.js and stays in step with
// the firmware's copy. This file re-exports it typed.

import type { Topology } from '@topology';
import { absoluteAngles as absAngles, applyAbsoluteAngles as applyAngles, servoCommandAngle } from '@topology';

export type BranchRole = 'tool' | 'unassigned' | 'blocked' | 'feed';
export type ServoKind = 'servoGate' | 'servoManifold';

export interface ServoBlock {
  channel: number;
  /** Absolute servo angle at which the reference state is exact. Absent = uncalibrated. */
  referenceAngle?: number;
  /** UI hint: the servo sits behind this gate, so a "right" nudge turns the handle left.
   *  Captured by the direction check; firmware never reads it. */
  reversed?: boolean;
  moveMs?: number;
  holdAtRest?: boolean;
  detented?: boolean;
  minAngle?: number;
  maxAngle?: number;
}

export interface ServoState {
  id: string;
  isClosed: boolean;
  offsetDeg: number;
}

export interface Branch {
  id: string;
  opensState: string;
  role: BranchRole;
}

export interface ServoSelector {
  id: string;
  type: 'selector';
  name?: string;
  controllerId: string;
  kind: ServoKind;
  states: ServoState[];
  branches: Branch[];
  servo: ServoBlock;
}

/** What the reference sweep measured, plus which manifold it was measured against. */
export interface LinearCalibration {
  stepsPerMm: number;
  measuredSpanSteps: number;
  homeIsMaxEndstop: boolean;
  manifoldModel: string;
}

export interface LinearState {
  id: string;
  isClosed: boolean;
  /** Millimetres from home. Absent on the closed (home) state. */
  positionMm?: number;
}

export interface LinearSelector {
  id: string;
  type: 'selector';
  name?: string;
  controllerId: string;
  kind: 'linear';
  states: LinearState[];
  branches: Branch[];
  linear?: { channel?: number; calibration?: LinearCalibration };
}

/** Either kind of selector the configurator can set up. */
export type ConfigurableSelector = ServoSelector | LinearSelector;

export interface Controller {
  id: string;
  role: 'primary' | 'secondary';
  name?: string;
}

export interface Duct {
  child: string;
  parent: string;
  parentBranch?: string;
}

/** Loose view of any element, for the walks that don't care what kind it is. */
export interface AnyElement {
  id: string;
  type: string;
  name?: string;
  [k: string]: unknown;
}

// ── Reading a topology without asserting the whole graph ─────────────────────

export function elementsOf(t: Topology): AnyElement[] {
  return ((t as { elements?: unknown }).elements as AnyElement[]) ?? [];
}

export function controllersOf(t: Topology): Controller[] {
  return ((t as { controllers?: unknown }).controllers as Controller[]) ?? [];
}

export function ductsOf(t: Topology): Duct[] {
  return ((t as { ducts?: unknown }).ducts as Duct[]) ?? [];
}

export function isServoSelector(e: AnyElement | null | undefined): e is AnyElement & ServoSelector {
  const el = e as Partial<ServoSelector> | null | undefined;
  return !!el && el.type === 'selector'
    && (el.kind === 'servoGate' || el.kind === 'servoManifold')
    && Array.isArray(el.states);
}

export function servoSelectorsOf(t: Topology): ServoSelector[] {
  return elementsOf(t).filter(isServoSelector);
}

export function isLinearSelector(e: AnyElement | null | undefined): e is AnyElement & LinearSelector {
  const el = e as Partial<LinearSelector> | null | undefined;
  return !!el && el.type === 'selector' && el.kind === 'linear' && Array.isArray(el.states);
}

/** Every selector that has something to set up — both kinds. */
export function configurableSelectorsOf(t: Topology): ConfigurableSelector[] {
  return elementsOf(t).filter(isConfigurableSelector);
}

export function isConfigurableSelector(
  e: AnyElement | null | undefined,
): e is AnyElement & ConfigurableSelector {
  return isServoSelector(e) || isLinearSelector(e);
}

/**
 * Has anyone actually measured this gate on the real hardware?
 *
 * Both kinds hinge on one field that only a calibration run can produce — a servo's
 * `referenceAngle`, a slider's swept `measuredSpanSteps`. Absent means nobody has
 * taught it where its positions are, and the Live view won't drive it.
 */
export function isCalibrated(sel: ConfigurableSelector): boolean {
  if (isServoKind(sel)) return typeof sel.servo?.referenceAngle === 'number';
  const span = (sel as LinearSelector).linear?.calibration?.measuredSpanSteps;
  return typeof span === 'number' && span > 0;
}

export function isServoKind(sel: ConfigurableSelector): sel is ServoSelector {
  return sel.kind === 'servoGate' || sel.kind === 'servoManifold';
}

/** Woodworker-facing name for a selector kind. */
export function kindLabel(sel: ConfigurableSelector): string {
  return sel.kind === 'servoGate' ? 'Ball valve'
    : sel.kind === 'servoManifold' ? 'Manifold'
    : 'Sliding gate';
}

/** The state every other angle is measured from: a gate's OPEN, a manifold's LEFT. */
export function referenceState(sel: ServoSelector): ServoState | null {
  return sel.states.find((s) => !s.isClosed) ?? null;
}

// ── Absolute ⇄ reference+offset (thin typed wrappers over topology.js) ───────

/** Commanded angle for one state, clamped to the servo's travel. */
export function commandAngle(sel: ServoSelector, stateId: string): number | null {
  return servoCommandAngle(sel, stateId);
}

/** stateId → absolute angle, for seeding the calibration widget. */
export function absoluteAngles(sel: ServoSelector): Map<string, number> {
  return new Map(Object.entries(absAngles(sel) ?? {}));
}

export interface ApplyResult {
  ok: boolean;
  /** Present when ok — a copy of the selector with referenceAngle + offsets rewritten. */
  selector?: ServoSelector;
  error?: string;
}

/** Fold captured absolute angles back into referenceAngle + per-state offsetDeg. */
export function applyAbsoluteAngles(sel: ServoSelector, captured: Map<string, number>): ApplyResult {
  return applyAngles<ServoSelector>(sel, Object.fromEntries(captured));
}

// ── Naming positions from the graph, so nothing needs a label field ──────────

/**
 * What each state actually selects, in the user's words: "Left → Drum sander",
 * "Right → capped", "Closed". Walks branch → duct → downstream element, so renaming
 * a tool renames the position for free.
 */
export function positionLabels(t: Topology, sel: ConfigurableSelector): Map<string, string> {
  const byId = new Map(elementsOf(t).map((e) => [e.id, e]));
  const childOfBranch = new Map<string, string>();
  for (const d of ductsOf(t)) if (d.parent === sel.id && d.parentBranch) childOfBranch.set(d.parentBranch, d.child);

  const labels = new Map<string, string>();
  for (const s of sel.states) {
    if (s.isClosed) {
      labels.set(s.id, sel.kind === 'servoGate' ? 'sealed'
        : sel.kind === 'servoManifold' ? 'both sealed'
        : 'parked at home');            // a slider's closed state is the home datum
      continue;
    }
    const branch = sel.branches.find((b) => b.opensState === s.id);
    const child = branch ? byId.get(childOfBranch.get(branch.id) ?? '') : undefined;
    // A junction downstream is bare pipe with an open end — naming it ("Open end")
    // would read like a destination when there's nothing there yet.
    if (child && child.type !== 'junction') labels.set(s.id, child.name || child.id);
    else if (branch?.role === 'blocked') labels.set(s.id, 'capped');
    else labels.set(s.id, '');
  }
  return labels;
}

/** Title case for a state id — "left" → "Left". State ids are the position names. */
export function positionName(stateId: string): string {
  return stateId.charAt(0).toUpperCase() + stateId.slice(1);
}

// Minimal TypeScript types for the v2 topology contract (topology.js).
// The graph configurator UI will refine these; for now the app treats a
// topology as an opaque validated document.

export type Topology = Record<string, unknown>;

export interface ValidationError { code: string; message: string; ref?: string; }
export interface ValidationResult { ok: boolean; errors: ValidationError[]; }

export function validateTopology(t: unknown): ValidationResult;
export function servoCommandAngle(sel: unknown, stateId: string): number | null;

/** stateId → commanded angle, for seeding the calibration widget. */
export function absoluteAngles(sel: unknown): Record<string, number> | null;

export interface ApplyAnglesResult<T = unknown> { ok: boolean; selector?: T; error?: string; }
/** Fold captured absolute angles back into referenceAngle + per-state offsetDeg. */
export function applyAbsoluteAngles<T>(sel: T, captured: Record<string, number>): ApplyAnglesResult<T>;

export interface AirflowIssue {
  id: string;
  name: string;
  /** always-open: no gate at all above it. co-open: routing to it leaves `with` open too. */
  kind: 'always-open' | 'co-open';
  /** the tools that can't be shut off while this one runs (co-open only) */
  with?: { id: string; name: string }[];
}
/** Tools that can't be selected without leaking — ungated, or sharing an outlet. */
export function airflowIssues(t: Topology): AirflowIssue[];

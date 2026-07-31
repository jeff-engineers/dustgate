// Minimal TypeScript types for the v2 topology contract (topology.js).
// The graph configurator UI will refine these; for now the app treats a
// topology as an opaque validated document.

export type Topology = Record<string, unknown>;

export interface ValidationError { code: string; message: string; ref?: string; }
export interface ValidationResult { ok: boolean; errors: ValidationError[]; }

export function validateTopology(t: unknown): ValidationResult;
export function servoCommandAngle(sel: unknown, stateId: string): number | null;

export interface AirflowIssue { id: string; name: string; kind: 'always-open'; }
/** Tools with no actuated selector between them and the collector — permanent leaks. */
export function airflowIssues(t: Topology): AirflowIssue[];

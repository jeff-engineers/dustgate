// Bus servo shapes from the brain's API (2026-11-01). Apart from api.service so plain-TypeScript specs can use them.

/** One bus servo, from GET /api/nodes (2026-11-01). The servo's own readings, as read; `ok` false = it did not answer
 *  the last read, and the readings are absent. `history` is the peak load of its last moves, oldest first. */
export interface BusServoState {
  id: number;
  ok: boolean;
  deg?: number;
  load?: number;
  v?: number;
  temp?: number;
  peak: number;
  missed: number;
  reads: number;
  moves: number;
  history: number[];
}

/** A bus board's answer to a SERVO order. `found` (find): none, one, or garbled (several new servos at once). */
export interface ServoReply { ok: boolean; err?: string; found?: 'none' | 'one' | 'garbled'; ids?: number[] }
export type ServoOp = 'find' | 'assign' | 'wiggle' | 'torque' | 'reset' | 'scan';

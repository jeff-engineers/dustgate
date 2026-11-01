/**
 * What a board is FLASHED to drive, and who gets to say so.
 *
 * `Controller.drives` in shared/device-model/topology.js is a CACHE of a hardware
 * fact, not a setting. The board already knows: a node reports it as `caps.bus`
 * in its WELCOME and the primary as `hasLinear` in its status, both straight from
 * HAS_LINEAR in the firmware, which is derived from the pin map and so cannot
 * disagree with the hardware. The cache exists only so the canvas can draw the
 * right port strip on a board that is asleep or not yet paired.
 *
 * PURE — no Angular, no browser, so it can be tested under plain node. It lives
 * here rather than inside either component because BOTH resolve this, and they
 * resolved it differently: the canvas read the live report first (right) and the
 * Boards list read the cache first (wrong), so the same slider node was drawn as
 * "0/1 SL" on one screen and "0 of 4 gates" on the other. One rule, one place.
 */

// The budget, not a literal: this said "four" for a month after boards dropped to two channels (2026-10-17).
import { SERVO_CHANNELS_PER_BOARD } from '../gates/selector-types';

export type Drives = 'servo' | 'bus';

/** What a board with nothing reported and nothing cached is assumed to drive.
 *  Matches `c.drives || 'servo'` in the validator — and so matches every layout
 *  saved before the field existed. */
export const DEFAULT_DRIVES: Drives = 'servo';

/** The primary's own report, from its status. Null when it has not said. */
export function drivesFromHasLinear(hasLinear: unknown): Drives | null {
  return typeof hasLinear === 'boolean' ? (hasLinear ? 'bus' : 'servo') : null;
}

/** A node's report, from its WELCOME caps. Null when it has not said. */
export function drivesFromCaps(caps: { bus?: number } | null | undefined): Drives | null {
  return caps && typeof caps.bus === 'number' ? (caps.bus > 0 ? 'bus' : 'servo') : null;
}

/**
 * LIVE REPORT FIRST, cache second, 'servo' last.
 *
 * The order is the whole point. The cache is only written when someone opens the
 * Boards screen, and nothing makes them — so reading it first shows a freshly
 * flashed slider as a four-port servo board until something happens to correct it.
 */
export function resolveDrives(reported: Drives | null, cached: Drives | null | undefined): Drives {
  return reported ?? cached ?? DEFAULT_DRIVES;
}

/** Can a board driving `drives` host a selector of `kind`?
 *
 *  A bus board takes any gate (valves, manifolds and one slider, 2026-11-01); a PWM board takes no slider — its bus pads
 *  are PWM pads (config.h #errors on a pin map claiming both). Capacity is a separate question. */
export function canHost(drives: Drives, kind: string): boolean {
  return drives === 'bus' || kind !== 'linear';
}

/** Servos one bus board drives — MAX_BUS_SERVOS_PER_HOST in topology.js, kMaxBusServos in NodeLink.h. */
export const BUS_SERVOS_PER_BOARD = 6;

/**
 * Write the cache onto a controller entry, or clear it.
 *
 * 'servo' DELETES the field rather than storing it, so a document round-trips
 * byte-identical to one saved before `drives` existed. Storing the default would
 * churn every layout on first open for no change in meaning.
 */
export function applyDrivesCache(controller: Record<string, unknown>, drives: Drives | null): void {
  if (drives === null) return;                     // never heard from it — leave the cache alone
  if (drives === 'servo') delete controller['drives'];
  else controller['drives'] = 'bus';
}

/** A shop with more gates of one kind than ports to plug them into. */
export interface PortShortfall {
  kind: Drives;
  /** Gates of that kind drawn in the shop. */
  gates: number;
  /** Ports of that kind the paired boards actually offer. */
  ports: number;
}

/**
 * Gates drawn with nowhere to plug them in, counted PER KIND.
 *
 * Counted twice, not once: a PWM board offers servo channels and no slider; a bus board offers BUS_SERVOS_PER_BOARD
 * servos of which at most one may be a slider (its two endstop inputs). Spare PWM channels are no help to a slider, and a
 * single total would say the shop had room.
 *
 * `servoPortsPerBoard` is passed in rather than defined here so this file does not
 * become a fourth place claiming the number 4 — the callers already have it
 * (SERVO_PORTS in build/wiring/wire-geometry.ts, mirroring MAX_SERVOS_PER_HOST in
 * topology.js and SERVO_COUNT in config.h).
 *
 * Count every PAIRED board, including one not yet placed on the canvas: it drives
 * gates all the same, and treating it as absent would invent a shortage.
 */
export function portShortfalls(
  boardDrives: Drives[], gateKinds: string[], servoPortsPerBoard: number,
): PortShortfall[] {
  let servoPorts = 0, sliderPorts = 0, busBoards = 0;
  for (const d of boardDrives) {
    if (d === 'bus') { sliderPorts += 1; busBoards += 1; } else servoPorts += servoPortsPerBoard;
  }
  let servoGates = 0, sliderGates = 0;
  for (const k of gateKinds) {
    if (k === 'linear') sliderGates += 1; else servoGates += 1;
  }
  const out: PortShortfall[] = [];
  // Valves fill PWM channels and whatever room the bus boards have left after their sliders.
  const valveRoom = servoPorts + busBoards * BUS_SERVOS_PER_BOARD - Math.min(sliderGates, sliderPorts);
  if (servoGates > valveRoom) out.push({ kind: 'servo', gates: servoGates, ports: valveRoom });
  if (sliderGates > sliderPorts) out.push({ kind: 'bus', gates: sliderGates, ports: sliderPorts });
  return out;
}

/** One shortfall as the sentence you would say out loud. No trailing advice — the
 *  caller adds what to DO about it once, however many shortfalls it is reporting. */
export function shortfallText(s: PortShortfall): string {
  const one = s.gates === 1;
  if (s.kind === 'bus') {
    return `${s.gates} sliding ${one ? 'gate needs' : 'gates need'} a bus board and this shop has `
         + `${s.ports === 0 ? 'none' : String(s.ports)} — a bus board drives one slider, so each needs its own.`;
  }
  return `${s.gates} ${one ? 'gate needs' : 'gates need'} a servo and this shop's boards have room for `
       + `${s.ports === 0 ? 'none' : `only ${s.ports}`} — each PWM board drives ${SERVO_CHANNELS_PER_BOARD}, each bus board ${BUS_SERVOS_PER_BOARD}.`;
}

/**
 * What to ask before forgetting a board, worded once.
 *
 * Two screens unpair: the Boards list and the canvas board menu. They said
 * different things — the canvas asked, the list just did it — which is backwards,
 * since the list is the easier of the two to click by accident. The wording is here
 * for the same reason `drivesFromCaps` is: two callers, one answer.
 *
 * It says what SURVIVES as well as what goes. "Unpair" sounds destructive to
 * someone who has just spent an evening provisioning WiFi, and the board keeps
 * every bit of that — this shop simply stops listing it.
 */
export function unpairPrompt(name: string): string {
  return `Unpair ${name}? The board stays powered and keeps its WiFi, `
       + `but this shop forgets it. Pair it again from Boards.`;
}

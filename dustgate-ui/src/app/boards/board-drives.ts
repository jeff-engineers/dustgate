/**
 * What a board is FLASHED to drive, and who gets to say so.
 *
 * `Controller.drives` in shared/device-model/topology.js is a CACHE of a hardware
 * fact, not a setting. The board already knows: a node reports it as `caps.linear`
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

export type Drives = 'servo' | 'linear';

/** What a board with nothing reported and nothing cached is assumed to drive.
 *  Matches `c.drives || 'servo'` in the validator — and so matches every layout
 *  saved before the field existed. */
export const DEFAULT_DRIVES: Drives = 'servo';

/** The primary's own report, from its status. Null when it has not said. */
export function drivesFromHasLinear(hasLinear: unknown): Drives | null {
  return typeof hasLinear === 'boolean' ? (hasLinear ? 'linear' : 'servo') : null;
}

/** A node's report, from its WELCOME caps. Null when it has not said. */
export function drivesFromCaps(caps: { linear?: number } | null | undefined): Drives | null {
  return caps && typeof caps.linear === 'number' ? (caps.linear > 0 ? 'linear' : 'servo') : null;
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
 *  Never both: a sliding gate needs the serial bus and a valve needs the PWM bank,
 *  and the two builds contend for the same pads (config.h #errors on a pin map
 *  claiming both). So this is an equality test, not a capacity one — capacity is a
 *  separate question asked only of a board that passes this. */
export function canHost(drives: Drives, kind: string): boolean {
  return drives === (kind === 'linear' ? 'linear' : 'servo');
}

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
  else controller['drives'] = 'linear';
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
 * The two capacities never substitute for each other, which is the whole reason
 * this counts twice instead of once: a PWM board offers servo channels and no
 * slider port, a slider board offers one rack and no channels, and the two builds
 * contend for the same pads. Four spare servo channels are no help whatsoever to a
 * second sliding gate, and a single total would say the shop had room.
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
  let servoPorts = 0, sliderPorts = 0;
  for (const d of boardDrives) {
    if (d === 'linear') sliderPorts += 1; else servoPorts += servoPortsPerBoard;
  }
  let servoGates = 0, sliderGates = 0;
  for (const k of gateKinds) {
    if (k === 'linear') sliderGates += 1; else servoGates += 1;
  }
  const out: PortShortfall[] = [];
  if (servoGates > servoPorts) out.push({ kind: 'servo', gates: servoGates, ports: servoPorts });
  if (sliderGates > sliderPorts) out.push({ kind: 'linear', gates: sliderGates, ports: sliderPorts });
  return out;
}

/** One shortfall as the sentence you would say out loud. No trailing advice — the
 *  caller adds what to DO about it once, however many shortfalls it is reporting. */
export function shortfallText(s: PortShortfall): string {
  const one = s.gates === 1;
  if (s.kind === 'linear') {
    return `${s.gates} sliding ${one ? 'gate needs' : 'gates need'} a slider board and this shop has `
         + `${s.ports === 0 ? 'none' : String(s.ports)} — a slider board drives one rack, so each rack needs its own.`;
  }
  return `${s.gates} ${one ? 'gate needs' : 'gates need'} a servo channel and this shop has `
       + `${s.ports === 0 ? 'none' : `only ${s.ports}`} — each servo board drives four.`;
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

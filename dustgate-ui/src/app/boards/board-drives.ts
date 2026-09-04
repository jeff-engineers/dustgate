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

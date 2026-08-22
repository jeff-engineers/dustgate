/** Fitting a plug's name into the chip drawn inside a machine on the canvas.
 *
 *  The chip is a fixed-width box with the outlet icon on its left, so the name
 *  gets whatever is left over — about 49 units. It used to be trimmed at a flat
 *  11 characters, which is not a width: "Drum Sander" is 11 characters and 60
 *  units wide, so it ran back over the icon and out through the right-hand edge.
 *  (Both were on screen at once in the shop on 2026-08-22 — "Drum Sander" and
 *  "Router Tab…", each overlapping its own icon.)
 *
 *  So the trim is by MEASURE instead. Nothing here can call getBBox — this runs
 *  during change detection, once per machine per pass, and a layout read there is
 *  the classic way to make a canvas crawl — so widths come from a table.
 *
 *  Pure, no Angular, no DOM: `npm test` runs it under plain node.
 */

/**
 * Advance width per character, in EM, so the table survives a font-size change.
 *
 * Measured off the rendered chip (system-ui at 9.5px, macOS) and then rounded
 * UP into a handful of classes. Both halves of that are deliberate: `system-ui`
 * is a different typeface on every platform this UI is opened on — SF on a
 * phone, Roboto on Android, whatever the desktop has — so a table accurate to
 * three decimals for one of them is false precision for the rest. Rounding up
 * also biases the whole thing the safe way: an over-estimate trims a character
 * early, an under-estimate puts the text back through the icon, which is the bug
 * this exists to fix.
 */
const EM: ReadonlyArray<readonly [string, number]> = [
  ["ijlI.,:;!|'·", 0.30],
  [' ', 0.30],
  ['frt()[]{}-/\\', 0.42],
  ['1', 0.50],
  ['mw', 0.85],
  ['MW', 0.95],
  ['ABCDEFGHJKLNOPQRSTUVXYZ', 0.70],
  ['…', 0.82],
];
/** Lowercase and digits — the bulk of any plug name. */
const DEFAULT_EM = 0.60;

const WIDTHS = new Map<string, number>();
for (const [chars, em] of EM) for (const ch of chars) WIDTHS.set(ch, em);

/** Roughly how wide `s` renders at `fontPx`. Over-estimates; see EM. */
export function textWidth(s: string, fontPx: number): number {
  let em = 0;
  for (const ch of s) em += WIDTHS.get(ch) ?? DEFAULT_EM;
  return em * fontPx;
}

/**
 * `s`, trimmed with an ellipsis until it fits `maxPx`.
 *
 * The ellipsis is measured too — trimming to a budget and then appending a
 * character wider than the two it replaced is how a "fitted" string ends up
 * overflowing anyway. If not even one character plus the ellipsis fits, the
 * ellipsis alone is the honest answer: something is there, and it is not
 * readable at this size.
 */
export function fitText(s: string, maxPx: number, fontPx: number): string {
  if (textWidth(s, fontPx) <= maxPx) return s;
  const ell = textWidth('…', fontPx);
  let cut = s.length - 1;
  while (cut > 0 && textWidth(s.slice(0, cut), fontPx) + ell > maxPx) cut--;
  // Don't leave the space before the ellipsis: "Router …" reads as a word missing,
  // "Router…" as a name continuing.
  return cut > 0 ? s.slice(0, cut).trimEnd() + '…' : '…';
}

/**
 * A Shelly's own hostname leads with the model — `shellyplugus-tablesaw` — which
 * is the same on every plug in the shop and so identifies nothing. The tail is
 * the part the user named.
 */
export function stripPlugPrefix(name: string): string {
  const s = name.replace(/^shelly(plug(us)?|plus)?-?/i, '');
  return s || name;
}

/** What the chip shows for a plug that is sitting idle: which plug, trimmed to
 *  the room the icon leaves it. */
export function plugLabel(name: string, maxPx: number, fontPx: number): string {
  return fitText(stripPlugPrefix(name), maxPx, fontPx);
}

// ── Landing on one piece's outlet panel ──────────────────────────────────────
//
// `/tools?el=<id>` opens that piece's pairing panel on arrival instead of the
// list. It exists so the shop page can stop naming a setup fact and leaving you
// to go find the screen that fixes it: "No outlet paired" is a link now, and
// this is where it lands.
//
// The awkward part is that `<id>` arrives from a screen that speaks a DIFFERENT
// vocabulary. The shop list is a list of MACHINES (a table saw with a cabinet
// port and an overarm is one row); this screen is a list of PORTS and
// collectors. So the id on the query string may be a machine's, and the row it
// should open is that machine's primary port. Resolving that here, as a pure
// function, is what lets it be tested without a browser — the failure mode is
// silent (the link lands on the plain list and the reader is back where they
// started), which is exactly the kind that survives a manual pass.

/** The two kinds of row the list holds. `machineId` is empty for a collector. */
export interface LinkableRow {
  id: string;
  machineId: string;
  /** A machine's primary port is the row a machine-id link should open. */
  primary: boolean;
}

/**
 * Which row `el` names, or null if nothing on this screen does.
 *
 * Exact id first — a collector, or a port linked to by its own id — then the
 * machine. Preferring the primary port matters for a two-port machine: both
 * rows edit the same outlet, but landing on the overarm's row to pair the saw
 * reads as the wrong screen.
 */
export function resolveDeepLink<T extends LinkableRow>(rows: readonly T[], el: string | null | undefined): T | null {
  if (!el) return null;
  const exact = rows.find(r => r.id === el);
  if (exact) return exact;
  const mine = rows.filter(r => r.machineId && r.machineId === el);
  if (!mine.length) return null;
  return mine.find(r => r.primary) ?? mine[0];
}

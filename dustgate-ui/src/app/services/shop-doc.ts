// shop-doc.ts — the UI's typed seam onto a schemaVersion-2 shop document.
//
// shop.js owns the contract (validate / route / plan / migrate). This owns the
// EDITING vocabulary the Angular side needs on top of it, and exists so the
// answer to "where does a tool's plug live now?" is written down once instead of
// re-derived at each of the half-dozen call sites that used to reach straight
// into `element.sensor.outlet`.
//
// Two ideas the rest of the app has to absorb:
//
//   SYSTEMS — elements and ducts live inside `systems[]`, not at the root. Most
//   readers want the whole shop and use elementsOf()/ductsOf() in
//   gates/selector-types.ts, which flatten. The CANVAS is the exception: it draws
//   one duct tree at a time, so it works through activeSystem() below.
//
//   MACHINES — a `tool` element is now a PORT. The thing you switch on is a
//   machine, and it owns the display name, the trip point and the smart plug. A
//   table saw with a cabinet port and an overarm secondary port is ONE machine with one
//   plug behind two gates (RFC §6.3), which is exactly why the plug can't live on
//   the port any more.
//
// A v1 document is migrated on read (asShop), so nothing below ever has to
// branch on the version — that is the whole point of doing it at the seam.

import type { Topology } from '@topology';
import type { Shop } from '@shop';
import { asShop, isShop, portsByMachine, portEnabled, systemView } from '@shop';

export type RawEl = Record<string, unknown>;

export interface ShopSystem {
  id: string;
  name?: string;
  elements: RawEl[];
  ducts: RawEl[];
}

export interface ShopMachine {
  id: string;
  name?: string;
  sensor?: { outlet?: RawEl };
  [k: string]: unknown;
}

export interface ShopDoc extends Record<string, unknown> {
  schemaVersion: number;
  name?: string;
  controllers: RawEl[];
  systems: ShopSystem[];
  machines: ShopMachine[];
  devices?: RawEl[];
}

/**
 * Accept whatever the device or a file gave us and return a shop.
 *
 * Migration happens HERE, on read, and never on the device — the firmware reads
 * both shapes (Shop.h) so an older board keeps working, and a UI that migrated
 * lazily would write back a half-converted document the first time someone
 * saved. One conversion, at the boundary.
 */
export function toShop(doc: Topology | null | undefined): ShopDoc | null {
  if (!doc) return null;
  const shop = asShop(doc) as unknown as ShopDoc;
  healMachineNames(shop);
  return shop;
}

/** What addMachineWithPort calls a machine nobody has named yet. */
export const NEW_MACHINE_NAME = 'New tool';

/**
 * Give a machine back the name its port is carrying.
 *
 * Repairs shops saved before 2026-08-22, when the canvas rename wrote only the
 * port element: every machine in a shop built entirely on the canvas is still
 * called "New tool", so the live view, tool setup and the plug tray's "Use it
 * for" picker all listed a shop full of identical entries while the canvas
 * showed the real names. renameMachine() stops it happening again; this is for
 * the documents it already happened to, and it runs on read so nobody has to
 * retype eleven names.
 *
 * ONE DIRECTION ONLY. A machine that has a name of its own keeps it — the
 * opposite case is real (renamed in tool setup, which always wrote the machine,
 * and never touched on the canvas) and healing both ways would make whichever
 * screen ran last the winner.
 */
export function healMachineNames(doc: ShopDoc | null): void {
  if (!doc) return;
  for (const m of machinesOf(doc)) {
    const own = (m.name as string | undefined)?.trim();
    if (own && own !== NEW_MACHINE_NAME) continue;
    const port = primaryPortOf(doc, m.id as string);
    const fromPort = (port?.['name'] as string | undefined)?.trim();
    if (!fromPort || fromPort === own) continue;
    renameMachine(doc, m.id as string, fromPort);
  }
}

export const isShopDoc = (doc: unknown): boolean => isShop(doc);

export function systemsOf(doc: ShopDoc | null): ShopSystem[] {
  return doc && Array.isArray(doc.systems) ? doc.systems : [];
}

/**
 * The system the canvas is currently drawing.
 *
 * Falls back to the first system rather than returning null: there is always at
 * least one (validateShop requires it), and every caller here is mid-edit and
 * has nothing sensible to do with "no system".
 */
export function systemById(doc: ShopDoc | null, id: string | null): ShopSystem | null {
  const all = systemsOf(doc);
  if (!all.length) return null;
  return all.find(s => s.id === id) ?? all[0];
}

/**
 * Every system as a plain topology, the shape topology.js still speaks.
 *
 * This is the seam for the shop-blind analysers — airflowIssues(),
 * redundantSelectors() — which are each a statement about ONE blower ("can this
 * tool be selected without pulling air somewhere else too") and would silently
 * answer "nothing wrong" if handed a shop, because a shop has no `elements` at
 * its root. Running them per system is what keeps them true rather than quiet.
 */
export function systemViews(doc: ShopDoc | null): Topology[] {
  if (!doc) return [];
  return systemsOf(doc).map(s => systemView(doc as unknown as Shop, s as never) as unknown as Topology);
}

export function machinesOf(doc: ShopDoc | null): ShopMachine[] {
  return doc && Array.isArray(doc.machines) ? doc.machines : [];
}

export function machineById(doc: ShopDoc | null, id: string | null | undefined): ShopMachine | null {
  if (!id) return null;
  return machinesOf(doc).find(m => m.id === id) ?? null;
}

/** The machine a port element belongs to, or null for anything that isn't a port. */
export function machineOfPort(doc: ShopDoc | null, el: RawEl | null | undefined): ShopMachine | null {
  if (!el || el['type'] !== 'tool') return null;
  return machineById(doc, el['machineId'] as string | undefined);
}

/** Every port of a machine, disabled ones included, with the system each is in. */
/**
 * Is this outlet already sensing a DIFFERENT machine?
 *
 * Rows on the tools screen are PORTS — a table saw with a cabinet gate and an
 * overarm is two of them — while an outlet belongs to the MACHINE (one box, one
 * outlet; the routing brain only ever senses machines). So "is it taken" has to
 * be asked machine-to-machine.
 *
 * Asking it row-to-row is what broke: each of a two-port tool's rows saw the
 * other as a different tool, so the saw's own outlet read as taken and could not
 * be re-selected — and because the picker refuses a taken outlet, the row was
 * left with no ip and the save deleted the pairing outright (2026-08-24).
 */
export function outletTakenByAnotherMachine(
  doc: ShopDoc | null,
  rows: { id: string; ip: string; hasPlug: boolean }[],
  ip: string,
  portId: string,
): boolean {
  if (!ip) return false;
  const mine = machineIdOfPort(doc, portId);
  return rows.some(r => r.hasPlug && r.ip === ip && machineIdOfPort(doc, r.id) !== mine);
}

/** The machine a PORT belongs to. Falls back to the port's own id, so a document
 *  we cannot read compares like-for-like instead of matching everything. */
export function machineIdOfPort(doc: ShopDoc | null, portId: string): string {
  for (const sys of systemsOf(doc)) {
    const el = (sys.elements as RawEl[]).find(e => e['id'] === portId);
    if (el) return (machineOfPort(doc, el)?.id as string) || portId;
  }
  return portId;
}

export function portsOf(doc: ShopDoc | null, machineId: string): { systemId: string; port: RawEl }[] {
  if (!doc) return [];
  return (portsByMachine(doc as unknown as Shop).get(machineId) ?? []) as
    { systemId: string; port: RawEl }[];
}

/**
 * A port counts unless explicitly switched off. Absent means enabled.
 *
 * Only SUPPLEMENTAL ports can be switched off (RFC §6.6) — that is the glyph
 * coming off the saw for an afternoon. A primary is always enabled, so the UI
 * must not draw a disable control on one at all.
 */
export const isPortEnabled = (port: RawEl): boolean => portEnabled(port);

/**
 * A port is primary unless it says otherwise (RFC §6.3).
 *
 * Absent means primary because a port nobody has thought about is a port whose
 * air you actually need — and because a migrated v1 doc carries the flag
 * nowhere. Exactly one port of a machine is primary; the other 0–2 are
 * supplemental, and the GUI draws them lighter than the primary.
 */
export const isPortSupplemental = (port: RawEl): boolean => port['supplemental'] === true;

/** Whether the UI may offer a disable toggle for this port. Primaries: never. */
export const canDisablePort = (port: RawEl): boolean => isPortSupplemental(port);

/** The one port a machine cannot do without, or null for a malformed machine. */
export function primaryPortOf(doc: ShopDoc | null, machineId: string): RawEl | null {
  return portsOf(doc, machineId).find(({ port }) => !isPortSupplemental(port))?.port ?? null;
}

/**
 * What to call a system on a LIST screen — /shop, /tools, /gates.
 *
 * The collector's name is the fallback rather than the first choice, and that
 * order matters: a woodworker names the blower ("Cyclone"), and the system is
 * usually left as whatever the canvas called it — but once someone HAS named the
 * system, that is the name they expect to see. One definition because three
 * screens now print it, and three copies would become three names.
 */
export function systemLabel(sys: ShopSystem | null | undefined): string {
  if (!sys) return 'System';
  return (sys.name as string) || (collectorOf(sys)?.['name'] as string) || 'System';
}

/**
 * Every system, in the order the build canvas draws them top to bottom.
 *
 * The canvas stripes its systems by the topmost row any of their pieces stands
 * on, so reading the same saved cells and sorting the same way is what makes the
 * list screens agree with the drawing. Document order is the fallback for a shop
 * that has never been laid out — which is what the canvas would auto-layout from
 * anyway — and ties keep it, so the sort is stable wherever the layout has
 * nothing to say.
 *
 * JUNCTIONS ARE SKIPPED, exactly as the canvas's own systemRowBands() does: a
 * loose run end is where pipe happened to reach, not a row the system stands on.
 */
export function systemsInLayoutOrder(doc: ShopDoc | null): ShopSystem[] {
  const layout = (doc as unknown as { ui?: { layout?: Record<string, { row: number }> } } | null)?.ui?.layout;
  const topRow = (sys: ShopSystem): number => {
    if (!layout) return Number.POSITIVE_INFINITY;
    let lo = Number.POSITIVE_INFINITY;
    for (const e of sys.elements as RawEl[]) {
      if (e['type'] === 'junction') continue;
      const c = layout[e['id'] as string];
      if (c) lo = Math.min(lo, c.row);
    }
    return lo;
  };
  return systemsOf(doc)
    .map((s, i) => ({ s, i, row: topRow(s) }))
    .sort((a, b) => (a.row - b.row) || (a.i - b.i))
    .map(({ s }) => s);
}

/** The collector element of a system, or null for a system drawn without one yet. */
export function collectorOf(sys: ShopSystem | null | undefined): RawEl | null {
  if (!sys) return null;
  return (sys.elements as RawEl[]).find(e => e['type'] === 'collector') ?? null;
}

/** Every element in the shop, flattened across systems. */
function allElems(doc: ShopDoc | null): RawEl[] {
  return systemsOf(doc).flatMap(s => s.elements as RawEl[]);
}

/**
 * Outlets that may NOT be picked for `targetId`, and why, for the outlet picker.
 *
 * One physical outlet driving two tools would make the routing brain believe two
 * machines started at once; a collector's own switch is off-limits for the
 * obvious reason. Exclusion is per MACHINE, not per row: a machine's ports share
 * one outlet by design, so two ports of the same saw are not a clash.
 *
 * `targetId` is whatever is being configured — a port, a machine, or a collector
 * element. All three resolve to "the machine (or collector) this outlet would
 * belong to", which is the only identity the comparison cares about.
 *
 * Lives here rather than on either screen because BOTH the build canvas and the
 * tools list open the same sheet, and two copies of "which outlets are taken"
 * would be two answers the day one of them learned about a new plug holder.
 */
export function outletExcludes(doc: ShopDoc | null, targetId: string):
    { ips: string[]; reason: Record<string, string> } {
  const ips: string[] = [];
  const reason: Record<string, string> = {};
  for (const el of allElems(doc)) {
    if (el['type'] !== 'collector') continue;
    // …unless the collector IS what is being configured: its own outlet has to
    // stay pickable, or re-opening the sheet greys out the current choice.
    if (el['id'] === targetId) continue;
    const ip = ((el['control'] as RawEl | undefined)?.['outlet'] as RawEl | undefined)?.['ip'] as string | undefined;
    if (ip) { ips.push(ip); reason[ip] = 'reserved — dust collector'; }
  }
  const mine = machineOfPort(doc, allElems(doc).find(e => e['id'] === targetId))?.id ?? targetId;
  for (const m of machinesOf(doc)) {
    if (m.id === mine) continue;
    const ip = (m.sensor?.outlet as RawEl | undefined)?.['ip'] as string | undefined;
    if (ip) { ips.push(ip); reason[ip] = `already paired with ${m.name || 'another tool'}`; }
  }
  return { ips, reason };
}

// ── the plug ────────────────────────────────────────────────────────────────
//
// A collector still carries its own switch (`control.outlet`) on the element,
// because a collector belongs to exactly one system and there is no machine to
// lift it onto. A tool's SENSOR moved to its machine. These two helpers hide
// that asymmetry so call sites keep saying "the plug for this thing".

/** The smart plug attached to an element: a collector's switch, or a port's machine's sensor. */
export function outletOf(doc: ShopDoc | null, el: RawEl | null | undefined): RawEl | null {
  if (!el) return null;
  if (el['type'] === 'collector') {
    return ((el['control'] as RawEl | undefined)?.['outlet'] as RawEl | undefined) ?? null;
  }
  const m = machineOfPort(doc, el);
  return ((m?.sensor as RawEl | undefined)?.['outlet'] as RawEl | undefined) ?? null;
}

/** Attach (or with null, detach) the plug for an element. */
export function setOutlet(doc: ShopDoc | null, el: RawEl, outlet: RawEl | null): void {
  if (el['type'] === 'collector') {
    if (outlet) el['control'] = { ...(el['control'] as RawEl ?? {}), outlet };
    else if (el['control']) delete (el['control'] as RawEl)['outlet'];
    return;
  }
  const m = machineOfPort(doc, el);
  if (!m) return;
  if (outlet) m.sensor = { outlet };
  else delete m.sensor;
}

/**
 * The name to SHOW for an element.
 *
 * For a port that is its machine's only port, the machine's name is the answer —
 * "Table Saw", not "Table Saw · port 1". Once there are two, the port carries its
 * own ("Cabinet · 4\"") and the machine name is the heading above them. Falling
 * back to the machine keeps a freshly-added second port readable before anyone
 * has named it.
 */
export function displayName(doc: ShopDoc | null, el: RawEl | null | undefined): string {
  if (!el) return '';
  const own = el['name'] as string | undefined;
  const m = machineOfPort(doc, el);
  if (!m) return own || (el['id'] as string) || '';
  const ports = portsOf(doc, m.id);
  if (ports.length <= 1) return (m.name || own || m.id) as string;
  return (own || m.name || m.id) as string;
}

// ── editing ─────────────────────────────────────────────────────────────────

/**
 * Add a port to a system, creating its machine.
 *
 * The single-port case, which is what "add a tool" has always meant: one machine,
 * one port, sharing an id so anything already holding that id still resolves —
 * the same choice migrateToShop makes, for the same reason (RFC §12).
 */
export function addMachineWithPort(doc: ShopDoc, system: ShopSystem, id: string, name: string): RawEl {
  const port: RawEl = { id, type: 'tool', name, machineId: id };
  system.elements.push(port);
  doc.machines.push({ id, name });
  return port;
}

/**
 * Add an airflow system: a collector with one open end below it, which is exactly
 * what a blank shop starts as.
 *
 * The open end matters. A system with a bare collector has nothing to draw from,
 * so the canvas would show a lone circle with no way in; the junction is the "drag
 * out to run pipe" handle every other run starts from. The collector is deliberately
 * unnamed — it takes the name of whatever smart outlet gets paired to it, so adding
 * a system asks the user nothing.
 *
 * Ids come from the caller because uniqueness is shop-wide, not system-wide: two
 * systems sharing an element id would collide in `ui.layout` and in firmware status,
 * and validateShop rejects it.
 */
export function addSystem(doc: ShopDoc, ids: { system: string; collector: string }): ShopSystem {
  // Bare on purpose: a collector and nothing else. It used to arrive with an open
  // end already hanging off it, which guessed at a first run nobody had asked for —
  // and put it somewhere you then had to move. Every collector shows add-dots on its
  // free outlets, so starting the first run is one drag away.
  const collector: RawEl = { id: ids.collector, type: 'collector', name: 'Dust collector' };
  const system: ShopSystem = { id: ids.system, elements: [collector], ducts: [] };
  doc.systems.push(system);
  return system;
}

/**
 * Add a SUPPLEMENTAL port to a machine that already has a primary — a second port
 * on the same tool.
 *
 * It is a port, not a machine: no name of its own, no smart outlet, no trip point.
 * Those all live on the machine, which already exists. What it carries is a `role`
 * — "overarm", "glyph" — because two ports on one saw are only distinguishable by
 * where on the saw they are.
 *
 * The system is the caller's choice and may be a different one from the primary's:
 * a cabinet port on the cyclone and an overarm on the shop vac is the case the shop
 * container was lifted above the airflow graphs for (RFC §6.3).
 */
export function addSupplementalPort(
  doc: ShopDoc, system: ShopSystem, machineId: string, id: string, role: string,
): RawEl {
  const machine = machineById(doc, machineId);
  const port: RawEl = {
    id, type: 'tool', machineId, supplemental: true, role,
    name: `${machine?.name ?? 'Machine'} · ${role}`,
  };
  system.elements.push(port);
  return port;
}

/**
 * Rename a machine — the ONE place a machine's name is written.
 *
 * A machine's name is stored twice over: once on the machine, and once on each
 * of its ports, where addMachineWithPort and addSupplementalPort snapshot it at
 * creation ("Table saw", "Table saw · overarm"). Both copies have readers — the
 * canvas draws the port's, the live view and the plug tray's "Use it for" picker
 * read the machine's — and until 2026-08-22 each screen wrote only the one it
 * happened to read: renaming on the canvas left the picker on the old name, and
 * renaming in tool setup left the canvas on it. A machine with two names is
 * exactly the "which saw is that?" question this UI exists to answer.
 *
 * So the copies stay, and the WRITER is single. A supplemental port's caption is
 * derived rather than carried across, since it quotes the name it was built from
 * and would otherwise keep quoting the old one.
 */
export function renameMachine(doc: ShopDoc, machineId: string, name: string): void {
  const machine = machineById(doc, machineId);
  if (machine) machine.name = name;
  for (const { port } of portsOf(doc, machineId)) {
    port['name'] = isPortSupplemental(port)
      ? `${name} · ${(port['role'] as string) || 'Auxiliary'}`
      : name;
  }
}

/** How many supplemental ports this machine already has, across every system. */
export function supplementalCount(doc: ShopDoc | null, machineId: string): number {
  return portsOf(doc, machineId).filter(({ port }) => isPortSupplemental(port)).length;
}

/** Drop these element ids and any duct touching them, across every system. */
function dropElements(doc: ShopDoc, ids: Set<string>): void {
  for (const s of systemsOf(doc)) {
    s.elements = s.elements.filter(e => !ids.has(e['id'] as string));
    s.ducts = s.ducts.filter(d => !ids.has(d['child'] as string) && !ids.has(d['parent'] as string));
  }
}

/**
 * Remove a SUPPLEMENTAL port. Refuses anything else, and says so by returning
 * false.
 *
 * Dropping a bonus secondary port is a non-event: the machine survives with one fewer
 * port, keeps its plug and its name, and stays valid. **A primary port is not
 * deletable** (RFC §6.3) — it is the machine's one required connection, so the
 * only way it goes away is with the machine, through `removeMachine`. Callers
 * should not offer a delete on a primary port at all; the false return is the
 * backstop, not the UI.
 */
export function removePort(doc: ShopDoc, portId: string): boolean {
  const el = systemsOf(doc).flatMap(s => s.elements).find(e => e['id'] === portId);
  if (!el || !isPortSupplemental(el)) return false;
  dropElements(doc, new Set([portId]));
  return true;
}

/**
 * Remove a machine and every port it owns.
 *
 * This is the "delete this tool" action, and the ONLY thing that removes a
 * primary port. A machine cannot exist without its primary and a port cannot
 * exist without its machine, so the two go together or not at all — leaving
 * either half behind is a document validateShop rejects rather than a draft.
 */
export function removeMachine(doc: ShopDoc, machineId: string): void {
  dropElements(doc, new Set(portsOf(doc, machineId).map(({ port }) => port['id'] as string)));
  doc.machines = doc.machines.filter(m => m.id !== machineId);
}

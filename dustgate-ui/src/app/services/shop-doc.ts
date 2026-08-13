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
//   table saw with a cabinet port and an overarm pickup is ONE machine with one
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
  return asShop(doc) as unknown as ShopDoc;
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
export function portsOf(doc: ShopDoc | null, machineId: string): { systemId: string; port: RawEl }[] {
  if (!doc) return [];
  return (portsByMachine(doc as unknown as Shop).get(machineId) ?? []) as
    { systemId: string; port: RawEl }[];
}

/**
 * A port counts unless explicitly switched off. Absent means enabled.
 *
 * Only SUPPLEMENTAL ports can be switched off (RFC §6.6) — that is the hood
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
 * Dropping a bonus pickup is a non-event: the machine survives with one fewer
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
